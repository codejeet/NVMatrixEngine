param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('observe',48,'--fluid-room --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('adaptive',48,'--fluid-room --adaptive-rays --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('uniform',48,'--fluid-room --optical-reference --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('opaque',48,'--no-water --adaptive-rays'),
  @('pt',48,'--fluid-room --adaptive-rays --restir-pt'),
  @('orbit',96,'--fluid-room --adaptive-rays --orbit-test'),
  @('rolling',300,'--fluid-room --adaptive-rays --rolling-test'),
  @('inlet',64,'--fluid-room --adaptive-rays --fluid-emitter'),
  @('view',48,'--fluid-room --adaptive-rays --optical-view=importance --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('receivers',48,'--fluid-room --adaptive-rays --optical-view=caustics'),
  @('feedback',48,'--fluid-room --adaptive-rays --fluid-complexity-validate --fluid-surface-lod-axes=xz'),
  @('controls',48,'--fluid-room --optical-controls-test'),
  @('freeze',48,'--fluid-room --adaptive-rays --optical-freeze --orbit-test'),
  @('cap1',48,'--fluid-room --adaptive-rays --optical-samples=1'),
  @('cap8',16,'--fluid-room --adaptive-rays --optical-samples=8'),
  @('fg',48,'--fluid-room --adaptive-rays --frame-gen=2'),
  @('resize',180,'--fluid-room --adaptive-rays --frame-gen=2 --frame-gen-test'),
  @('nvapi',48,'--fixture --adaptive-rays --ser=nvapi'),
  @('dxr',48,'--fixture --adaptive-rays --ser=dxr'),
  @('fixed',48,'--fluid-room --adaptive-rays --ser=off --atomics=fixed')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before optical validation'}
  $name="optical-$($case[0])";$started=[DateTime]::UtcNow
  # Heavy independent CPU readback can distort presentation pacing. Exercise
  # FG timing separately; the controls case audits both internal-resolution resizes.
  $validation=if($case[0] -eq 'resize'){''}else{'--optical-validate'}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$validation --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned optical test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Optical validation failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale optical report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$o=$r.opticalImportance
    $audits=if($case[0] -eq 'resize'){0}elseif($case[0] -eq 'controls'){12}else{$case[1]}
    if($r.frames -ne $case[1] -or $o.audits -ne $audits -or $o.last[12]){throw 'Missing/invalid optical audit'}
    if($o.last[0] -lt $r.internalWidth*$r.internalHeight -or $o.last[4] -gt $o.last[5] -or $o.last[15] -gt $o.last[14]){throw 'Invalid reduced optical work counters'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.restirPT.invalidLastFrame){throw 'Invalid optical transport'}
    if($case[0] -eq 'fg' -and !$r.frameGeneration.enabled){throw 'FG disabled'}
    if($case[0] -eq 'pt' -and !$r.restirPT.enabled){throw 'PT disabled'}
    if($case[0] -eq 'feedback' -and (!$o.feedbackAudits -or !$o.last[14] -or !$r.complexity.opticalFeedbackBricks -or !$r.complexity.opticalRaisedBricks -or !$r.complexity.validated)){throw 'Optical/fluid feedback disconnected'}
    Write-Host "PASS $name | actual camera rays $($o.last[0]); budget histogram $($o.last[7..10] -join ','); caustic pixels $($o.last[11])"
  }finally{$p.Dispose()}
}
