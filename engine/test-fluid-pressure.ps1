param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',[string]$ModeFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('compression',1,'--fluid-pit --fluid-solver-only --fluid-transfer-test=3 --fluid-validate'),
  @('empty',4,'--fluid-room --fluid-particles=0 --no-whitewater'),
  @('fall',90,'--fluid-pit --fluid-validate'),
  @('room',240,'--fluid-room --fluid-emitter --fluid-validate --fluid-adaptive'),
  @('odd-sweeps',90,'--fluid-room --fluid-emitter --fluid-pressure-iterations=119 --fluid-validate'),
  @('flip',90,'--fluid-pit --fluid-flip --fluid-validate'),
  @('no-capillary',90,'--fluid-room --fluid-surface-tension=0 --fluid-validate'),
  @('frame-gen',90,'--fluid-room --fluid-emitter --frame-gen=2'),
  @('pt',90,'--fluid-room --fluid-emitter --restir-pt')
)
foreach($mode in 'active','multigrid'){
  if($mode -notmatch $ModeFilter){continue}
  foreach($case in $cases){
    if($case[0] -notmatch $CaseFilter){continue}
    if(Get-Process NVMatrixFluidLab,NVMatrixEngine,NVMatrixFluidLab-pressure-baseline -ErrorAction SilentlyContinue){throw 'Close game/lab before pressure tests'}
    $name="pressure-$mode-$($case[0])";$started=[DateTime]::UtcNow
    $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-pressure=$mode --fluid-pressure-validate --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
    try{
      if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned pressure test timed out: $name"}
      $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Pressure failed: $name"}
      if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale pressure report'}
      $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$s=$r.fluid.pressureSolver
      if(!$s.validated -or $r.frames -ne $case[1] -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid pressure or renderer validation'}
      if($mode -eq 'active' -and $s.activeCoarseCells){throw 'Active Jacobi unexpectedly assembled coarse operator'}
      if($case[0] -eq 'empty' -and $s.activeFineCells){throw 'Empty domain scheduled pressure work'}
      if($case[2] -match 'fluid-validate' -and !$r.fluid.validated){throw 'Full fluid validation missing'}
      if($case[0] -eq 'frame-gen' -and (!$r.frameGeneration.enabled -or $r.frameGeneration.status -or !$r.frameGeneration.reflex)){throw 'Frame generation handoff not active/healthy'}
      if($case[0] -eq 'pt' -and (!$r.restirPT.enabled -or $r.restirPT.invalidLastFrame -or !$r.restirPT.reusedLastFrame)){throw 'Path reuse not active/healthy'}
      if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.whitewaterProbes.bad){throw 'Fluid optical validation failed'}
      Write-Host "PASS $name | $($s.activeFineCells) fine / $($s.activeCoarseCells) coarse | $($s.lastFrameMs) ms pressure | divergence $($r.fluid.divergenceRmsAfter)"
    }finally{$p.Dispose()}
  }
}
