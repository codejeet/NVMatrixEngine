param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$references=@{}
$cases=@(
  @('reference',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('reference-long',600,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('reference-wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-wake-test'),
  @('reference-uniform-wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-wake-test'),
  @('empty',4,'--fluid-pit --fluid-particles=0 --no-whitewater'),
  @('calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('cycle',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-interior-cycle-test'),
  @('calm-long',600,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-interior-wake-test'),
  @('fall',120,'--fluid-pit --no-whitewater'),
  @('room',90,'--fluid-room --fluid-emitter'),
  @('multigrid',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-pressure=multigrid'),
  @('bulk',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-bulk-validate'),
  @('fg',60,'--fluid-room --fluid-emitter --frame-gen=2'),
  @('pt',60,'--fluid-room --fluid-emitter --restir-pt')
)
foreach($case in $cases){
  $dependents=switch($case[0]){'reference'{@('calm','cycle')};'reference-long'{@('calm-long')};'reference-wake'{@('wake')};'reference-uniform-wake'{@('wake')};default{@()}}
  if($case[0] -notmatch $CaseFilter -and !@($dependents|Where-Object{$_ -match $CaseFilter}).Count){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before interior validation'}
  $name="interior-$($case[0])";$started=[DateTime]::UtcNow
  $interiorSwitch=if($case[0] -like 'reference*'){''}else{'--fluid-interior-validate'}
  $samplingSwitch=if($case[0] -eq 'reference-uniform-wake'){'--fluid-adaptive'}else{'--fluid-resample-validate'}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$interiorSwitch --fluid-validate $samplingSwitch --fluid-complexity-validate --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned interior test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Interior test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale interior report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$f=$r.fluid;$b=$f.interior
    if($case[0] -like 'reference*'){
      if(!$f.validated -or $b){throw 'Invalid particle-only reference'}
      $references[$case[0]]=$r;Write-Host "PASS $name | particle-only comparison";continue
    }
    if($r.frames -ne $case[1] -or !$f.validated -or !$b.validated -or $b.invalid -or !$r.complexity.validated){throw 'Missing/invalid interior validation'}
    if($null -eq $f.postStepMaxRelativeDensity -or $null -eq $f.maxPostStepDensityAuditError -or $f.maxPostStepDensityAuditError -gt .0002){throw 'Missing/invalid current-state density audit'}
    if($f.particleMassUnits+$b.massUnits -ne $f.particles -or $f.resampling.massUnits -ne $f.particles){throw 'Lost hybrid liquid mass'}
    if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid fluid optical geometry'}
    if($case[0] -eq 'fg' -and !$r.frameGeneration.enabled){throw 'Frame generation did not enable'}
    if($case[0] -eq 'pt' -and !$r.restirPT){throw 'ReSTIR PT did not enable'}
    if($case[0] -eq 'calm' -and (!$b.ownedCells -or !$b.massUnits -or !$b.demotions)){throw 'No actual particle-free liquid'}
    if($case[0] -eq 'cycle' -and (!$b.demotions -or !$b.promotions -or $b.ownedCells -or $b.massUnits -or $f.active -ne $f.particles)){throw 'Force-fine handoff failed'}
    if($case[0] -eq 'wake' -and (!$b.demotions -or !$b.promotions)){throw 'Moving rigid body did not restore particle detail'}
    if($case[0] -eq 'wake' -and (!$f.densityRepairRequestedLastSubstep -or $f.postStepMaxRelativeDensity -gt 1.05)){throw 'Moving-wall compression exceeded 5 percent after repair'}
    if($case[0] -eq 'calm' -and $f.densityRepairRequestedLastSubstep){throw 'Calm fixture should skip extra density iterations'}
    if($case[0] -match '^(calm|calm-long|cycle|wake)$'){
      $reference=$references[$(if($case[0] -eq 'calm-long'){'reference-long'}elseif($case[0] -eq 'wake'){'reference-uniform-wake'}else{'reference'})]
      $volumeError=[Math]::Abs($r.fluidSurface.tetrahedralVolumeEstimate/$reference.fluidSurface.tetrahedralVolumeEstimate-1)
      $tolerance=if($case[0] -eq 'wake'){.01}else{.0025}
      if($volumeError -gt $tolerance -or $f.maxRelativeDensity -gt $reference.fluid.maxRelativeDensity+.01 -or $f.postStepMaxRelativeDensity -gt $reference.fluid.postStepMaxRelativeDensity+.01){throw "Interior changed reference: volume error $volumeError; pre-density $($f.maxRelativeDensity) vs $($reference.fluid.maxRelativeDensity); post-density $($f.postStepMaxRelativeDensity) vs $($reference.fluid.postStepMaxRelativeDensity)"}
    }
    Write-Host "PASS $name | $($b.ownedCells) coarse owners, $($b.massUnits) mass units / $($f.active) actual samples, $($b.promotions) restorations"
  }finally{$p.Dispose()}
}
