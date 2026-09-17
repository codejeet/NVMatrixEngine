param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop'
$out="$BuildDir/bin/Release"
$cases=@(
  @('empty',4,'--fluid-pit --fluid-particles=0 --no-whitewater'),
  @('calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --no-whitewater'),
  @('fall',120,'--fluid-pit --no-whitewater'),
  @('long',600,'--fluid-pit --no-whitewater'),
  @('room',300,'--fluid-room --fluid-emitter --rolling-test'),
  @('freeze',90,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-complexity-freeze --no-whitewater'),
  @('reset',16,'--fluid-room --fluid-complexity-controls-test'),
  @('flip',120,'--fluid-pit --fluid-flip --no-whitewater'),
  @('multigrid',90,'--fluid-room --fluid-pressure=multigrid --fluid-emitter'),
  @('bulk',90,'--fluid-pit --fluid-bulk-validate --no-whitewater'),
  @('frame-gen',60,'--fluid-room --frame-gen=2 --fluid-emitter'),
  @('pt',60,'--fluid-room --restir-pt --fluid-emitter')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before resampling GPU validation.'}
  $name="resampling-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name --fluid-resample-validate --fluid-validate --fluid-complexity-validate $($case[2])" -WorkingDirectory $out -PassThru
  try {
    if(!$p.WaitForExit(90000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Resampling test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale resampling report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$f=$r.fluid;$s=$f.resampling
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1] -or !$f.validated -or !$s.validated){throw 'Incomplete validation'}
    if($s.massError -ne 0 -or $s.linearErrorPerMass -gt 2e-6 -or $s.angularErrorPerMass -gt 2e-6 -or $s.relativeEnergyIncrease -gt 5e-6){throw 'Conservation failure'}
    if($f.restMassUnits -ne $f.particles -or $s.massUnits -ne $f.particles -or $f.active -ne $s.activeSamples){throw 'Lost rest mass / sample accounting'}
    if(($s.samplesByRequestedLod|Measure-Object -Sum).Sum -ne $f.active){throw 'Lost sample partition'}
    if($s.binRebuilds -gt $s.frames -or (!$s.merged -and !$s.split -and $s.binRebuilds)){throw 'Spurious conditional rebin work'}
    if(!$r.complexity.validated -or $r.complexity.invalidMetrics -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated){throw 'Invalid importance / fluid intersection'}
    if($r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid photon transport'}
    if($case[0] -eq 'calm' -and (!$s.merged -or $s.activeSamples -ge $f.particles)){throw 'Calm interior did not reduce actual work'}
    if($case[0] -eq 'fall' -and (!$s.merged -or !$s.split)){throw 'Impact did not restore particle detail'}
    if($case[0] -eq 'empty' -and ($s.activeSamples -or $s.massUnits)){throw 'Empty domain gained water'}
    if($case[0] -eq 'frame-gen' -and !$r.frameGeneration.enabled){throw 'FG disabled'}
    Write-Host "PASS $name | samples $($s.activeSamples) / mass $($s.massUnits), merged $($s.merged), split $($s.split), $($s.meanMs) ms maintenance"
  } finally {$p.Dispose()}
}
