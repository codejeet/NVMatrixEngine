param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineHgi/build", [int]$Repeats = 3)
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue) { throw 'Close the lab before GPU validation.' }
if ($Repeats -lt 1 -or $Repeats -gt 10) { throw 'Use 1 to 10 paired benchmark repeats.' }
$cases = @(
  @('large-controls', '--water-lab=large --water-path=hamiltonian --wave-controls-test', 12),
  @('large-follow', '--water-lab=large --water-path=hamiltonian --wave-patch-test', 240),
  @('large-hos3', '--water-lab=large --water-path=hamiltonian --wave-order=3 --wave-epsilon=1', 600)
)
for ($repeat = 0; $repeat -lt $Repeats; ++$repeat) {
  # Alternate order to reduce clock/thermal ordering bias.
  $paths = if ($repeat % 2) { @('hamiltonian', 'baseline') } else { @('baseline', 'hamiltonian') }
  foreach ($path in $paths) {
    $cases += ,@("large-$path-$repeat", "--water-lab=large --water-path=$path", 240)
    $cases += ,@("room-$path-$repeat", "--water-path=$path", 240)
  }
}
$summary = @()
foreach ($case in $cases) {
  $name = $case[0]; $frames = $case[2]; $started = [DateTime]::UtcNow
  $argsList = "--normal-lens --boat --width=1280 --height=720 --quality=balanced --frame-gen=off --frames=$frames --capture --name=$name $($case[1])"
  $p = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $argsList -WorkingDirectory $out -PassThru
  try {
    if (!$p.WaitForExit(240000)) { $p.Kill(); throw "Timed out: $name" }
    $p.Refresh()
    if ($p.ExitCode) { Get-Content "$out/lab-error.txt"; throw "Failed: $name" }
    $file = Get-Item "$out/$name.json"
    if ($file.LastWriteTimeUtc -lt $started) { throw "Stale report: $name" }
    $r = Get-Content $file -Raw | ConvertFrom-Json
    if ($r.frames -ne $frames -or $r.dlssEvaluations -ne $frames -or !$r.fluidSurface.surfaceBricks) { throw "Missing rendered water: $name" }
    if ($name.StartsWith('large-') -and (!$r.largeWaterLab -or $r.deepPool -or $r.fluid.initialDepth -ne 1.5)) { throw 'Incorrect large-room preset.' }
    if ($r.photonCounters[5] -or $r.photonCounters[6]) { throw "Nonfinite photon transport: $name" }
    if ($r.waterPath -eq 'hamiltonian') {
      $h = $r.hamiltonian
      if ($h.nonfinite -or $h.boundaryOverflow -or !$h.boundarySeeded -or $h.steps -ne $r.fluid.steps -or $h.inverseDnoRelativeResidual -gt .02) { throw "Invalid wave/coupling state: $name" }
      if (!$h.adaptiveRegions -or $h.activeColumns -le 0) { throw 'Adaptive 3D regions were not selected.' }
      if ($name -eq 'large-follow' -and $h.regionChanges -lt 20) { throw '3D regions did not respond to moving bodies.' }
      if ($name.StartsWith('large-') -and ($h.activeColumns -gt .5 * $h.resolution * $h.resolution -or $r.fluid.particles -gt .6 * $r.fluid.initialParticles)) { throw 'Calm large-room water retained unnecessary full 3D work.' }
      if ($h.receivedParticles + $h.freeWaterParticles -ne $h.sourceParticles) { throw 'Region transitions changed basin mass.' }
    }
    $summary += [ordered]@{ name=$name; arguments=$argsList; adapter=$r.adapter; frames=$r.frames;
      capturedUtc=$file.LastWriteTimeUtc.ToString('o'); warmupFrames=$r.warmupFrames; sampleCount=$r.sampleCount;
      outputWidth=$r.outputWidth; outputHeight=$r.outputHeight; dlssMode=$r.dlssMode;
      timingColumns=$r.timingColumns; medianMs=$r.medianMs; fluid=$r.fluid; hamiltonian=$r.hamiltonian;
      surfaceBricks=$r.fluidSurface.surfaceBricks; photonCounters=$r.photonCounters }
    $frameIndex = [array]::IndexOf($r.timingColumns, 'frame')
    $simIndex = [array]::IndexOf($r.timingColumns, 'fluidSimulation')
    Write-Host "PASS: $name | frame $($r.medianMs[$frameIndex]) ms | simulation $($r.medianMs[$simIndex]) ms | particles $($r.fluid.particles)"
  } finally { $p.Dispose() }
}
$summary | ConvertTo-Json -Depth 12 | Set-Content "$out/large-water-validation.json"
