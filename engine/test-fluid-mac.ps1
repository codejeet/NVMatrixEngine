param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
  [ValidateSet('relaxation','multigrid')][string]$Solver='relaxation',[switch]$SplitCoarse)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$references=@{}
$cases=@(
  @('reference-calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('reference-fall',120,'--fluid-pit --no-whitewater'),
  @('reference-wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-wake-test'),
  @('reference-room',90,'--fluid-room --fluid-emitter'),
  @('empty',4,'--fluid-pit --fluid-particles=0 --no-whitewater'),
  @('audit',12,'--fluid-pit --fluid-mac-validate --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('view',12,'--fluid-pit --fluid-view --fluid-mac-view --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('cycle',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-mac-cycle-test'),
  @('fall',120,'--fluid-pit --no-whitewater'),
  @('wake',180,'--fluid-pit --fluid-mac-validate --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater --fluid-wake-test'),
  @('interior',120,'--fluid-pit --fluid-interior-validate --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('room',90,'--fluid-room --fluid-emitter'),
  @('odd',60,'--fluid-room --fluid-emitter --fluid-pressure-iterations=121'),
  @('fg',60,'--fluid-room --fluid-emitter --frame-gen=2'),
  @('pt',60,'--fluid-room --fluid-emitter --restir-pt')
)
foreach($case in $cases){
  $dependent=switch($case[0]){'reference-calm'{@('calm','cycle')};'reference-fall'{@('fall')};'reference-wake'{@('wake')};'reference-room'{@('room')};default{@()}}
  if($case[0] -notmatch $CaseFilter -and !@($dependent|Where-Object{$_ -match $CaseFilter}).Count){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before adaptive MAC validation'}
  $prefix=if($Solver -eq 'multigrid'){'mac-mgpcg'}else{'mac'}
  if($SplitCoarse){if($Solver -ne 'multigrid'){throw 'SplitCoarse requires the multigrid solver'};$prefix+='-split'}
  $name="$prefix-$($case[0])";$started=[DateTime]::UtcNow
  $mode=if($case[0] -like 'reference-*'){'--fluid-adaptive'}else{"--fluid-mac-solver=$Solver"}
  # A converged pressure solver must be compared with a high-accuracy fine
  # reference, not the intentionally bounded 120-sweep interactive baseline.
  if($Solver -eq 'multigrid' -and $case[0] -like 'reference-*'){$mode+=' --fluid-pressure-iterations=1000 --fluid-density-iterations=120'}
  if($SplitCoarse -and $case[0] -notlike 'reference-*'){$mode+=' --fluid-mac-split-coarse'}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$mode --fluid-deterministic-bins --fluid-validate --fluid-complexity-validate --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned MAC test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "MAC test failed: $name"}
    $path="$out/$name.json"
    if((Get-Item $path).LastWriteTimeUtc -lt $started -or (Get-Item $path).LastWriteTimeUtc -lt (Get-Item "$out/NVMatrixFluidLab.exe").LastWriteTimeUtc){throw 'Stale MAC report'}
    $r=Get-Content $path -Raw|ConvertFrom-Json;$f=$r.fluid;$m=$f.adaptiveMac
    if($case[0] -like 'reference-*'){
      if(!$f.validated -or $m){throw 'Invalid fine-grid reference'}
      $references[$case[0]]=$r;Write-Host "PASS $name | fine-grid comparison";continue
    }
    if(!$f.validated -or !$m.validated -or $m.invalid -or !$r.complexity.validated -or $r.frames -ne $case[1]){throw 'Missing/invalid adaptive MAC audit'}
    if($Solver -eq 'multigrid'){
      $mg=$m.multigrid
      if($m.solver -ne 'multigrid-pcg' -or !$mg.validated -or $mg.hierarchyError -gt .00002 -or $mg.factorError -gt .00002 -or $mg.residualError -gt .0002){throw 'Missing/invalid independent MGPCG audit'}
      if($mg.exhaustedSolves -or $mg.peakFinalDivergence -gt .000101){throw "MGPCG missed an intermediate convergence target: peak $($mg.peakFinalDivergence), capped $($mg.exhaustedSolves)"}
      Write-Host "MGPCG $($mg.levels) coarse levels, $($mg.lastIterations) iterations, $($mg.maxDivergence) divergence, $($mg.exhaustedSolves) capped solves"
    }
    if($case[0] -match '^(calm|interior)$' -and (!$m.coarseLeaves -or !$m.junctionFaces)){throw 'No actual mixed-resolution pressure/velocity degrees of freedom'}
    if($null -eq $f.maxPostStepDensityAuditError -or $f.maxPostStepDensityAuditError -gt .0002){throw 'Invalid MAC current-state density audit'}
    # The existing falling-block impact is not within the calm/wake 5% density
    # bound. Require an actual same-build fine-grid comparison for that case.
    if($case[0] -match '^(calm|cycle|wake|interior)$' -and $f.postStepMaxRelativeDensity -gt 1.05){throw 'Adaptive MAC calm/wake compression regression'}
    if($case[0] -match '^(calm|cycle|fall|wake|room)$'){
      $ref=$references[$(if($case[0] -eq 'cycle'){'reference-calm'}else{"reference-$($case[0])"})]
      $volumeError=[Math]::Abs($r.fluidSurface.tetrahedralVolumeEstimate/$ref.fluidSurface.tetrahedralVolumeEstimate-1)
      if($volumeError -gt .01 -or $f.postStepMaxRelativeDensity -gt $ref.fluid.postStepMaxRelativeDensity+.01){throw "Mixed MAC changed fine-grid reference: volume $volumeError, density $($f.postStepMaxRelativeDensity) vs $($ref.fluid.postStepMaxRelativeDensity)"}
    }
    if($case[0] -eq 'cycle' -and (!$m.finePromotions -or !$m.coarseDemotions -or !$m.coarseLeaves)){throw 'MAC LOD transition cycle was not exercised'}
    if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid adaptive MAC fluid optical geometry'}
    if($case[0] -eq 'fg' -and !$r.frameGeneration.enabled){throw 'Frame generation did not enable'}
    if($case[0] -eq 'pt' -and !$r.restirPT){throw 'ReSTIR PT did not enable'}
    Write-Host "PASS $name | $($m.leaves) pressure leaves, $($m.coarseLeaves) coarse, $($m.junctionFaces) T faces | projection $($m.lastMs) ms"
  }finally{$p.Dispose()}
}
