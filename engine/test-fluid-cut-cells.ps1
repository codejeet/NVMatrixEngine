param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*')
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$cases=@(
  @('empty',4,'--fluid-pit --fluid-solver-only --fluid-particles=0'),
  @('plane',8,'--fluid-pit --fluid-particles=257 --fluid-cut-fixture=1'),
  @('oblique',8,'--fluid-room --fluid-particles=257 --fluid-cut-fixture=2'),
  @('moving-plane',60,'--fluid-pit --fluid-particles=257 --fluid-cut-fixture=3'),
  @('sphere',8,'--fluid-pit --fluid-particles=257 --fluid-cut-fixture=4'),
  @('pit',48,'--fluid-pit'),
  @('room',48,'--fluid-room --fluid-emitter --fluid-bulk-validate'),
  @('wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-wake-test'),
  @('controls',12,'--fluid-pit --fluid-particles=257 --fluid-controls-test'),
  @('view',16,'--fluid-room --fluid-cut-view'),
  @('fg-view',16,'--fluid-room --fluid-cut-view --frame-gen=2'),
  @('adaptive',48,'--fluid-room --fluid-emitter --fluid-resample --fluid-sparse-work --fluid-mac-solver=multigrid --adaptive-rays --restir-pt')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before cut-cell tests'}
  $name="cut-cells-$($case[0])";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-cut-validate --fluid-validate --no-whitewater --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned cut-cell test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Cut-cell test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale cut-cell report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$c=$r.fluid.cutCells
    if(!$c.validated -or $c.auditedFrames -ne $case[1] -or $c.invalid -or !$r.fluid.validated -or $c.maxHistoryError){throw 'Cut-cell audit failed'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated){throw 'Optical geometry failed'}
    if($case[0] -in @('plane','oblique','sphere','empty') -and $c.updates -ne 1){throw 'Static geometry was not cached'}
    if($case[0] -in @('plane','oblique','sphere','view') -and !$c.fineCutCells){throw 'No partial cells exercised'}
    if($case[0] -eq 'moving-plane' -and ($c.updates -ne $r.fluid.steps -or !$c.absoluteChangedVolumeM3)){throw 'Substep moving capacity failed'}
    if($case[0] -eq 'room' -and [Math]::Abs($c.bulkInventoryM3-$r.fluid.bulk.volumeM3) -gt .0001){throw 'Capacity observer changed inventory'}
    if($case[0] -eq 'fg-view' -and $r.frameGeneration.enabled){throw 'Untracked cut-cell overlay was sent to Frame Generation'}
    Write-Host "PASS $name | $($c.fineCutCells) partial cells | $($c.updates) geometry updates | $($c.maxGeometryError) geometry error"
  }finally{$p.Dispose()}
}
