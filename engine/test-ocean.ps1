param([string]$RuntimeDir = "$env:LOCALAPPDATA/NVMatrixEngineHgi/build/bin/Release")
$ErrorActionPreference = 'Stop'
$out = $RuntimeDir
if (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue) { throw 'Close the lab before serial GPU validation.' }
& "$out/NVMatrixEngineExperienceTest.exe"
if ($LASTEXITCODE) { throw 'Island collision, camera, buoyancy or HDRI calibration test failed.' }
& "$out/NVMatrixEngineHamiltonianGpuTest.exe" $out
if ($LASTEXITCODE) { throw 'Indoor wave-grid numerical test failed.' }
& "$out/NVMatrixEngineHamiltonianGpuTest.exe" $out --ocean-grid
if ($LASTEXITCODE) { throw 'Ocean wave-grid numerical test failed.' }
& "$out/NVMatrixEngineHamiltonianGpuTest.exe" $out --grid64
if ($LASTEXITCODE) { throw 'Intermediate wave-grid numerical test failed.' }
$cases = @(
  @('ocean-day', '--water-lab=extra-large', 180),
  @('ocean-overview', '--water-lab=ocean --fluid-view', 600),
  @('ocean-night', '--water-lab=ocean --fluid-view --time-of-day=night', 180),
  @('ocean-controls', '--water-lab=ocean --ocean-controls-test', 120),
  @('ocean-swim', '--water-lab=ocean --ocean-swim-test', 600),
  @('ocean-restir', '--water-lab=ocean --restir-pt --time-of-day=night', 96),
  @('ocean-regression-small', '--fluid-room', 96),
  @('ocean-regression-large', '--water-lab=large', 96),
  @('ocean-regression-inlet', '--fluid-room --fluid-emitter --fluid-validate', 240)
)
$summary = @()
foreach ($case in $cases) {
  $name = $case[0]; $frames = $case[2]; $started = [DateTime]::UtcNow
  $arguments = "--normal-lens --boat --quality=balanced --frame-gen=off --width=1280 --height=720 --frames=$frames --capture --name=$name $($case[1])"
  $p = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
  try {
    if (!$p.WaitForExit(240000)) { $p.Kill(); throw "Timed out: $name" }
    $p.Refresh()
    if ($p.ExitCode) { Get-Content "$out/lab-error.txt"; throw "Failed: $name" }
    $file = Get-Item "$out/$name.json"
    if ($file.LastWriteTimeUtc -lt $started) { throw "Stale report: $name" }
    $r = Get-Content $file -Raw | ConvertFrom-Json
    if ($r.frames -ne $frames -or $r.dlssEvaluations -ne $frames -or !$r.fluidSurface.surfaceBricks) { throw "Missing rendered water: $name" }
    if ($r.photonCounters[5] -or $r.photonCounters[6] -or $r.restirPT.invalidLastFrame) { throw "Invalid light transport: $name" }
    if ($r.waterPath -eq 'hamiltonian') {
      $h = $r.hamiltonian
      if ($h.nonfinite -or $h.boundaryOverflow -or !$h.boundarySeeded -or $h.steps -ne $r.fluid.steps -or $h.inverseDnoRelativeResidual -gt .02) { throw "Invalid coupled water: $name" }
      if ($h.receivedParticles + $h.freeWaterParticles -ne $h.sourceParticles) { throw "Lost source volume: $name" }
    }
    if (!$name.StartsWith('ocean-regression-')) {
      if (!$r.oceanLab -or $r.largeWaterLab -or $r.deepPool -or $r.waterPath -ne 'hamiltonian' -or $r.fluid.initialDepth -ne 6 -or $r.fluid.density -ne 1025) { throw "Incorrect ocean preset: $name" }
      if ($h.resolution -ne 128 -or $h.fftResolution -ne 256 -or !$h.activeColumns -or $h.activeColumns -gt 8192) { throw "Ocean did not retain calm wave regions: $name" }
      if ($r.fluid.domainMaximum[0] - $r.fluid.domainMinimum[0] -ne 256) { throw 'Ocean size changed.' }
      if ($r.fluidSurface.allocatedBytes -gt 50000000 -or $r.fluidSurface.anisotropic) { throw 'Unused Hamiltonian shape storage returned.' }
      $expectedSky = if ($name -in @('ocean-night','ocean-restir')) { 'night' } else { 'day' }
      if ($r.oceanEnvironment.timeOfDay -ne $expectedSky) { throw "Wrong environment: $name" }
      $expectedLights = if ($expectedSky -eq 'night') { 5 } else { 0 }
      if ($r.oceanEnvironment.activeLanterns -ne $expectedLights) { throw "Wrong lantern state: $name" }
      if ($name -eq 'ocean-controls' -and !$r.fluid.emittedParticles) { throw 'The pier outlet did not emit.' }
      if ($name -eq 'ocean-swim' -and (!$r.whitewater.validated -or $r.whitewater.invalid -or
          !$r.whitewater.surfaceSourceParticles -or !$r.whitewater.foam -or !$r.whitewater.bubbles -or
          !$r.whitewater.spray -or !$r.whitewater.foamActiveNodes)) { throw 'Ocean voyage lost its simulated whitewater.' }
    } elseif ($name.EndsWith('inlet')) {
      if ($r.waterPath -ne 'baseline' -or !$r.fluid.validated -or !$r.whitewater.validated -or
          $r.whitewater.invalid -or !$r.whitewater.foam -or !$r.whitewater.bubbles -or !$r.whitewater.spray -or
          !$r.whitewater.foamActiveNodes -or $r.whitewaterProbes.bad -or !$r.whitewaterProbes.foam -or
          !$r.whitewaterProbes.bubbles -or !$r.whitewaterProbes.spray) { throw 'Inlet whitewater state or DXR optics failed.' }
    } elseif ($name.EndsWith('small')) {
      if ($r.oceanLab -or $r.largeWaterLab -or $r.waterPath -ne 'baseline') { throw 'Small Water Lab default changed.' }
    } elseif (!$r.largeWaterLab -or $r.oceanLab -or $r.waterPath -ne 'hamiltonian') { throw 'Large Water Lab default changed.' }
    $summary += [ordered]@{ name=$name; arguments=$arguments; adapter=$r.adapter; frames=$r.frames;
      capturedUtc=$file.LastWriteTimeUtc.ToString('o'); outputWidth=$r.outputWidth; outputHeight=$r.outputHeight;
      dlssMode=$r.dlssMode; oceanEnvironment=$r.oceanEnvironment; waterPath=$r.waterPath;
      fluid=$r.fluid; hamiltonian=$r.hamiltonian; surfaceBricks=$r.fluidSurface.surfaceBricks;
      surfaceAllocatedBytes=$r.fluidSurface.allocatedBytes; whitewater=$r.whitewater;
      medianMs=$r.medianMs; whitewaterProbes=$r.whitewaterProbes;
      photonCounters=$r.photonCounters; restirPT=$r.restirPT }
    Write-Host "PASS: $name | $($r.fluid.steps) substeps | $($r.fluidSurface.surfaceBricks) surface bricks"
  } finally { $p.Dispose() }
}
$summary | ConvertTo-Json -Depth 12 | Set-Content "$out/ocean-validation.json"
