param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('empty',4,'--fluid-room --fluid-particles=0 --no-whitewater'),
  @('fall',90,'--fluid-pit --fluid-validate'),
  @('room',300,'--fluid-room --fluid-emitter --fluid-adaptive --fluid-validate'),
  @('translation',120,'--fluid-pit --fluid-bulk-fixture=1'),
  @('high-cfl',120,'--fluid-pit --fluid-bulk-fixture=2'),
  @('persistent',120,'--fluid-pit --fluid-bulk-fixture=3'),
  @('controls',12,'--fluid-pit --fluid-particles=257 --fluid-controls-test'),
  @('reset-inlet',180,'--fluid-room-test'),
  @('active',90,'--fluid-room --fluid-emitter --fluid-pressure=active --fluid-pressure-validate'),
  @('multigrid',90,'--fluid-room --fluid-emitter --fluid-pressure=multigrid --fluid-pressure-validate'),
  @('overlay',32,'--fluid-room --fluid-emitter --fluid-bulk-view=1'),
  @('frame-gen',90,'--fluid-room --fluid-emitter --frame-gen=2'),
  @('pt',90,'--fluid-room --fluid-emitter --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab,NVMatrixEngine,NVMatrixFluidLab-bulk-baseline -ErrorAction SilentlyContinue){throw 'Close games before bulk tests'}
  $name="bulk-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-bulk-validate --frame-gen=off --width=1280 --height=720 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(120000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned bulk test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Bulk test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale bulk report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$b=$r.fluid.bulk
    if(!$b.validated -or $b.invalid -or $b.relativeVolumeError -gt .0002 -or $b.steps -ne $r.fluid.steps){throw 'Bulk conservation or step count failed'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.whitewaterProbes.bad){throw 'Optical validation failed'}
    if($case[0] -eq 'empty' -and ($b.volumeM3 -or $b.activeCells)){throw 'Empty inventory invented mass'}
    if($case[0] -eq 'high-cfl' -and !$b.limitedDonorUpdates){throw 'CFL stress did not exercise donor limit'}
    if($case[0] -eq 'persistent' -and ($b.sourceCalls -ne 1 -or $b.steps -ne 240)){throw 'Persistence fixture did not advance independently'}
    if($case[0] -eq 'frame-gen' -and (!$r.frameGeneration.enabled -or $r.frameGeneration.status)){throw 'FG handoff not healthy'}
    if($case[0] -eq 'pt' -and (!$r.restirPT.enabled -or $r.restirPT.invalidLastFrame)){throw 'PT reuse not healthy'}
    Write-Host "PASS $name | $($b.volumeM3) m3 | relative mass error $($b.relativeVolumeError) | $($b.lastFrameMs) ms bulk | max fraction $($b.maxVolumeFraction)"
  }finally{$p.Dispose()}
}
