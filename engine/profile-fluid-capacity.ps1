param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidatePattern('^[a-z0-9-]+$')][string]$Label='current',
      [ValidateRange(1,5)][int]$Repeats=3,
      [ValidateRange(1,5)][int]$FirstRepeat=1,
      [ValidateRange(1,4)][int]$Cycles=1)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
# Real render workload: no immutable snapshots or CPU simulation audits. Keep
# scene, frame count, warm-up, resolution and FG fixed across solver changes.
for($repeat=$FirstRepeat;$repeat -lt $FirstRepeat+$Repeats;$repeat++){
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before capacity profiling'}
  $name="capacity-cost-$Label-$repeat";$started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-bulk-coupled --fluid-capacity-cycles=$Cycles --fluid-room --fluid-emitter --orbit-test --frame-gen=off --width=1920 --height=1080 --quality=balanced --frames=120 --capture --name=$name" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned capacity profile timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Capacity profile failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale capacity profile'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$f=$r.fluid;$b=$f.bulk;$i=$b.implicitTransport;$p0=$b.carrierProjection;$m=$f.adaptiveMac;$c=$f.cutCells;$s=$f.bulkPressure
    if($r.frames -ne 120 -or $r.sampleCount -ne 88 -or $f.steps -ne 240 -or $f.droppedSeconds -or $r.frameGeneration.enabled -or $f.validated -or $m.auditedFrames -or $c.auditedFrames -or $b.validated -or $s.auditedFrames -or $i.auditedFrames -or $p0.auditedFrames){throw 'Invalid capacity profile workload'}
    if(!$p0.mixedPressureCoupled -or $p0.pressureCyclesPerIteration -ne $Cycles -or !$c.timeCenteredPressure -or !$m.cutPressure -or $b.inventoryBits -ne 64 -or $c.coarseCapacityBits -ne 64 -or $i.inventoryBits -ne 64){throw 'Incorrect capacity profile mode'}
    if($m.multigrid.exhaustedSolves -or $m.multigrid.peakFinalDivergence -gt .000101 -or $b.invalid -or $s.invalid -or $p0.invalid -or $p0.exhaustedSteps -or $p0.isolatedRows -or $p0.steps -ne 240 -or $p0.peakResidual -gt 5.01e-7){throw 'Invalid capacity profile pressure'}
    if($i.steps -ne 240 -or $i.invalid -or $i.exhaustedSteps -or $i.peakResidual -gt 2.01e-13 -or $i.peakExcessM3 -gt 1e-11 -or $c.bulkInClosedCellsM3 -or $b.relativeVolumeError -gt .0002 -or $b.maxVolumeFraction -gt 1.00001){throw 'Invalid capacity profile inventory'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.dlssEvaluations -ne 120){throw 'Invalid capacity profile optics'}
    Write-Host "$name | raw median $($r.medianMs[5]) ms | fluid median $($r.medianMs[6]) ms | capacity mean $($p0.meanFrameMs) ms | outer iterations $($p0.iterations)"
  }finally{$p.Dispose()}
}
