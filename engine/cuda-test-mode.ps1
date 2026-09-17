# Shared launch/report contract for full engine validation, not numerical fixtures.
function Get-CudaTestArguments($Backend, $Graphs, $Pressure, $PressureLoop='conditional', $Context='primary') {
  if (($Graphs -or $Pressure -ne 'uniform' -or $Context -ne 'primary') -and $Backend -ne 'cuda') {
    throw 'CUDA graphs/pressure require the CUDA backend'
  }
  $launch = if ($Graphs) {'on'} else {'off'}
  "--fluid-cuda-graphs=$launch --fluid-cuda-pressure=$Pressure --fluid-cuda-pressure-loop=$PressureLoop --fluid-cuda-context=$Context"
}
function Assert-CudaTestMode($Report, $Backend, $Graphs, $Pressure, $PressureLoop='conditional', $Context='primary') {
  if ($Report.fluid.backend -ne $Backend) { throw 'Wrong fluid backend' }
  if ($Backend -ne 'cuda') { return }
  $c = $Report.fluid.cuda
  $owned=[bool]$Report.fluid.ownedParticles
  if($null -eq $c.ownedParticles -or [bool]$c.ownedParticles -ne $owned -or
     $c.sharedBuffers -ne (14 + $(if($owned){4}else{0})) -or
     $c.ownershipBuffers -ne $(if($owned){4}else{0})){
    throw 'CUDA authoritative ownership binding mismatch'
  }
  if($owned -and (!$c.stagingBytes -or $c.completedFrames -ne $c.submissions -or $c.rejectedFrames)){
    throw 'CUDA ownership transaction was not completely published'
  }
  foreach($key in @('graphBuilds','directSteps','graphReplays','stagingBytes','mixedPressureBytes',
                   'completedFrames','submissions','rejectedFrames','pressureCaps','pressurePeakIterations',
                   'pressureCgBudget','pressurePageChanges','pressureChangesPerFrame','pressureDivergence',
                   'pressureCoarsePeak','graphNodes','graphBodyNodes','pressureLoopSolves','pressureLoopIterations',
                   'pressureSolves','pressureIterations','cigSharedMemoryBytes',
                   'refinementWakeCellSteps','refinementTemporalCellSteps','refinementPaddedCellSteps')) {
    if($null -eq $c.$key -or [double]::IsNaN($c.$key) -or [double]::IsInfinity($c.$key) -or $c.$key -lt 0){
      throw "Missing/invalid CUDA diagnostic: $key"
    }
  }
  if($c.contextMode -ne $Context -or ($Context -eq 'cig' -and !$c.cigSharedMemoryBytes) -or
     ($Context -eq 'primary' -and $c.cigSharedMemoryBytes)){throw 'Incorrect CUDA context selection/capability'}
  if ($c.pressureMode -ne $Pressure) { throw 'Wrong CUDA pressure mode' }
  $expectedRefinement=if($Pressure -eq 'mixed'){'advected-error-v1'}else{'none'}
  if($c.refinementPolicy -ne $expectedRefinement -or
     ($Pressure -ne 'mixed' -and ($c.refinementWakeCellSteps -or $c.refinementTemporalCellSteps -or
                                $c.refinementPaddedCellSteps))){
    throw 'Wrong CUDA refinement policy or inactive-policy work reported'
  }
  if ($Graphs -and (!$c.graphBuilds -or $c.directSteps)) { throw 'Graph replay not active' }
  if (!$Graphs -and $c.graphReplays) { throw 'Unexpected graph replay' }
  if ($Pressure -eq 'uniform') {
    if ($c.mixedPressure -or (!$owned -and $c.stagingBytes) -or $c.graphBodyNodes -or $c.pressureLoopSolves -or
        $c.pressureLoopIterations -or $c.pressureLoop -ne 'none') { throw 'Baseline unexpectedly used mixed pressure' }
    return
  }
  $expectedLoop=if($Graphs){$PressureLoop}else{'unrolled'}
  if($c.pressureLoop -ne $expectedLoop -or $c.graphBodyNodes -gt $c.graphNodes -or $c.pressureLoopSolves -gt $c.pressureSolves -or
     $c.pressureLoopIterations -gt $c.pressureIterations){throw 'Invalid pressure loop accounting'}
  if($expectedLoop -eq 'conditional'){
    if(!$c.graphBodyNodes -or ($c.graphReplays -and !$c.pressureLoopSolves)){
      throw 'Conditional pressure body was not captured/executed'
    }
  }elseif($c.graphBodyNodes -or $c.pressureLoopSolves -or $c.pressureLoopIterations){
    throw 'Unrolled pressure unexpectedly executed a conditional body'
  }
  if (!$c.mixedPressure -or !$c.stagingBytes -or !$c.mixedPressureBytes -or
      $c.completedFrames -ne $c.submissions -or $c.rejectedFrames -or $c.pressureCaps -or
      $c.pressurePeakIterations -gt $c.pressureCgBudget -or
      $c.pressurePageChanges -gt $c.pressureChangesPerFrame -or
      $c.pressureDivergence -gt .000101 -or ($Pressure -eq 'fine' -and $c.pressureCoarsePeak)) {
    throw 'Incomplete/unqualified CUDA pressure publication'
  }
  if($Report.fluid.validated -and ($Report.fluid.pressureAudit -ne 'published-fine-face-flux' -or
     $null -eq $Report.fluid.maxPublishedFluxDivergence -or $null -eq $Report.fluid.maxPublishedFluxMismatch -or
     $Report.fluid.maxPublishedFluxDivergence -gt .000101 -or $Report.fluid.maxPublishedFluxMismatch -gt .00001)){
    throw 'Missing independent published-face audit'
  }
  if($Report.fluid.validated){
    foreach($key in @('maxPublishedFluxDivergence','maxPublishedFluxMismatch')){
      if([double]::IsNaN($Report.fluid.$key) -or [double]::IsInfinity($Report.fluid.$key) -or $Report.fluid.$key -lt 0){
        throw 'Nonfinite/negative published-face audit'
      }
    }
  }
}
