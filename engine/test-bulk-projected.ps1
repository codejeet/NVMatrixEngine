param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
  [ValidateSet('projected','legacy','capacity','bounded','implicit','air','coupled')][string]$Mode='projected',[switch]$TimeCentered,[switch]$PressureSupport,
  [ValidateRange(1,4)][int]$CapacityCycles=1)
$ErrorActionPreference='Stop';$out="$BuildDir/bin/Release"
$implicit=$Mode -in @('implicit','air','coupled')
if($PressureSupport){if($Mode -ne 'bounded'){throw 'Bulk pressure requires bounded transport'};$TimeCentered=$true}
if($implicit){$TimeCentered=$true}
$cases=@(
  @('initial-calm',1,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0'),
  @('initial-room',1,'--fluid-room --fluid-gravity=0 --fluid-surface-tension=0'),
  @('empty',4,'--fluid-room --fluid-particles=0'),
  @('gravity-start',1,'--fluid-room'),
  @('short',8,'--fluid-room'),
  @('calm',120,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0'),
  @('fall',90,'--fluid-pit'),
  @('room',120,'--fluid-room --fluid-emitter'),
  @('room-no-tension',120,'--fluid-room --fluid-emitter --fluid-surface-tension=0'),
  @('wake',180,'--fluid-pit --fluid-gravity=0 --fluid-surface-tension=0 --fluid-wake-test'),
  @('controls',12,'--fluid-pit --fluid-particles=257 --fluid-surface-controls-test'),
  @('reset',180,'--fluid-room-test'),
  @('adaptive',64,'--fluid-room --fluid-emitter --fluid-resample --fluid-sparse-work --adaptive-rays --restir-pt'),
  @('overlay',32,'--fluid-room --fluid-emitter --fluid-bulk-view=1')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close games before projected bulk tests'}
  $name="bulk-$Mode-$($case[0])";$started=[DateTime]::UtcNow
  $solver=if($Mode -eq 'coupled'){'--fluid-bulk-coupled'}elseif($Mode -eq 'air'){'--fluid-bulk-air'}elseif($Mode -eq 'implicit'){'--fluid-bulk-implicit'}elseif($Mode -eq 'bounded'){'--fluid-bulk-bounded'}elseif($Mode -eq 'capacity'){'--fluid-bulk-capacity'}elseif($Mode -eq 'projected'){'--fluid-bulk-projected'}else{'--fluid-cut-pressure --fluid-bulk'}
  if($TimeCentered){$solver+=' --fluid-cut-time-centered';$name+="-time"}
  if($PressureSupport){$solver+=' --fluid-bulk-pressure';$name+='-support'}
  if($Mode -eq 'coupled'){$solver+=" --fluid-capacity-cycles=$CapacityCycles"}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "$solver --fluid-bulk-validate --fluid-cut-validate --fluid-mac-validate --fluid-validate --fluid-deterministic-bins --atomics=fixed --no-whitewater --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  try{
    if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned projected bulk test timed out: $name"}
    $p.Refresh();if($p.ExitCode){Get-Content "$out/lab-error.txt";throw "Projected bulk test failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale projected bulk report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json;$b=$r.fluid.bulk;$m=$r.fluid.adaptiveMac;$c=$r.fluid.cutCells
    if($b.projectedFlux -ne ($Mode -ne 'legacy') -or !$b.validated -or $b.invalid -or $b.relativeVolumeError -gt .0002 -or $b.steps -ne $r.fluid.steps){throw 'Bulk conservation or binding audit failed'}
    if($Mode -in @('capacity','bounded','implicit','air','coupled') -and (!$b.sourceAllocation.validated -or $b.sourceAllocation.invalid -or $b.sourceAllocation.newExcessM3 -gt 1e-7)){throw 'Source admission audit failed'}
    if($Mode -eq 'bounded'){
      $f=$b.phaseLimiter
      if(!$f -or ($f.steps -and !$f.validated) -or $f.invalid -or $f.exhaustedSteps -or $f.steps -ne $b.steps -or $f.peakNewExcessM3 -gt 1e-7){throw 'Every-substep phase bounds/convergence audit failed'}
    }
    if(!$m.validated -or $m.invalid -or !$c.validated -or $c.invalid -or !$r.fluid.validated){throw 'Missing solver/geometry/particle audits'}
    if($c.timeCenteredPressure -ne [bool]$TimeCentered -or $c.maxTimeAreaError -gt .0001){throw 'Temporal geometry mode or audit mismatch'}
    if($implicit){
      $i=$b.implicitTransport
      if(!$i -or $b.phaseLimiter -or ($i.steps -and !$i.validated) -or $i.invalid -or $i.exhaustedSteps -or $i.steps -ne $b.steps -or $i.peakResidual -gt 2.01e-10 -or ($i.auditedFrames+$i.auditedIdleFrames) -ne $r.frames){throw 'Implicit transport audit failed'}
      if($c.bulkInClosedCellsM3){throw 'Implicit transport left inventory in closed cells'}
      Write-Host "IMPLICIT $name | iterations $($i.iterations) | closing water updates $($i.closingWaterUpdates) | residual $($i.peakResidual)"
    }
    if($Mode -in @('air','coupled')){
      $e=$b.carrierProjection
      if(!$e -or ($e.steps -and !$e.validated) -or $e.invalid -or $e.exhaustedSteps -or $e.isolatedRows -or $e.steps -ne $b.steps -or $e.auditedSubsteps -ne $e.totalSubsteps -or ($e.auditedFrames+$e.auditedIdleFrames) -ne $r.frames -or $e.peakResidual -gt 5.01e-7){throw 'Air carrier projection audit failed'}
      if($b.maxVolumeFraction -gt 1.00001 -or $b.implicitTransport.peakExcessM3 -gt 1e-7){throw 'Air-extended implicit transport exceeded capacity tolerance'}
      if($e.mixedPressureCoupled -ne ($Mode -eq 'coupled') -or $e.appliedVelocityError -gt 2e-6){throw 'Capacity/MAC application audit failed'}
      if($Mode -eq 'coupled' -and ($null -eq $e.appliedPhysicalDivergence -or $e.originalPhysicalDivergence -gt .000101 -or $e.proposedPhysicalDivergence -gt 1.001e-6 -or $e.appliedPhysicalDivergence -gt 1.01e-6)){throw 'Applied physical divergence audit failed'}
      if($Mode -eq 'coupled'){
        if($e.pressureCyclesPerIteration -ne $CapacityCycles){throw 'Incorrect capacity inner-work budget'}
        if($b.inventoryBits -ne 64 -or $c.coarseCapacityBits -ne 64 -or $i.inventoryBits -ne 64 -or $i.tolerance -gt 2.01e-13){throw 'Coupled inventory/geometry precision contract failed'}
        if($null -eq $e.auditedPhaseDeficit -or $e.auditedPhaseDeficit -gt 1.001e-12 -or $e.auditedPhaseResidual -gt 5.01e-7){throw 'Signed phase supersolution audit failed'}
        if($b.capacityRestrictionMaxError -gt 1e-14 -or $i.peakExcessM3 -gt 1e-11){throw 'Coupled precision/capacity bound failed'}
      }
      if($Mode -eq 'coupled' -and $case[0] -eq 'gravity-start' -and !$e.changedFaceUpdates){throw 'Gravity start did not exercise the coupled correction'}
      Write-Host "CARRIER $name | iterations $($e.iterations) | changed faces $($e.changedFaceUpdates) | residual $($e.peakResidual)"
    }
    if($PressureSupport -or $implicit){
      $s=$r.fluid.bulkPressure
      if(!$s -or ($s.projectionCalls -and !$s.validated) -or $s.invalid -or $s.velocityReferenceError -gt .000002 -or $s.weightReferenceError -gt .000002 -or ($s.auditedFrames+$s.auditedIdleFrames) -ne $r.frames){throw 'Bulk pressure coverage audit failed'}
      if($s.partialClosingSupport -ne $implicit){throw 'Partial closing pressure mode mismatch'}
      if($implicit -and $case[0] -eq 'reset' -and !$s.partialClosingCellUpdates){throw 'No partially filled closing pressure cells exercised'}
      if(!$s.lastFrameProjectionCalls -and ($s.lastAddedCells -or $s.lastFilledFaces -or $s.lastFilledCellVisits)){throw 'Idle frame recorded bulk pressure work'}
      Write-Host "COVERAGE $name | added cells $($s.addedCellUpdates) | missing faces $($s.filledFaceUpdates) | closing cells $($s.closingCellUpdates)"
    }
    if(!$m.multigrid.validated -or $m.multigrid.exhaustedSolves -or $m.multigrid.peakFinalDivergence -gt .000101){throw 'Pressure convergence failed'}
    if($Mode -ne 'legacy' -and ($b.fluxRestrictionMaxError -gt 1e-10 -or $b.capacityRestrictionMaxError -gt 1e-7)){throw 'Canonical restriction mismatch'}
    if($r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated){throw 'Optical validation failed'}
    if($case[0] -eq 'empty' -and ($b.volumeM3 -or $b.activeCells)){throw 'Empty inventory invented mass'}
    if($case[0] -eq 'calm' -and !$m.coarseLeaves){throw 'No actual coarse pressure leaves exercised'}
    if($TimeCentered -and $case[0] -eq 'reset' -and (!$m.auditedClosingLiquidSamples -or $m.auditedClosingConnectedSamples -ne $m.auditedClosingLiquidSamples)){throw 'No connected disappearing liquid pressure cells exercised'}
    # Older modes report excess without asserting bounded VOF. Air mode has the
    # explicit actual-transport bound above; its failures are not waived.
    Write-Host "PASS $name | volume $($b.volumeM3) | mass error $($b.relativeVolumeError) | restriction $($b.fluxRestrictionMaxError) | excess $($b.excessVolumeM3) | fraction $($b.maxVolumeFraction)"
  }finally{$p.Dispose()}
}
