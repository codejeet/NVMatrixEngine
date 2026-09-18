param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineHgi/build")
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue) { throw 'Close the lab before GPU validation.' }
& "$out/NVMatrixEngineHamiltonianGpuTest.exe" $out
if ($LASTEXITCODE) { throw 'Hamiltonian numerical / inlet fixture failed.' }
$cases = @(
  @('ham-fill-small', '--wave-fill-test', 360),
  @('ham-fill-large', '--wave-fill-test --water-lab=large', 600),
  @('ham-fill-hos3', '--wave-fill-test --water-lab=large --wave-order=3 --wave-epsilon=1', 360),
  @('ham-fill-cli', '--fluid-emitter --water-lab=large --inlet-view', 120),
  @('ham-jet-small', '--fluid-emitter --inlet-view', 180)
)
$results = @()
foreach ($case in $cases) {
  $name = $case[0]; $frames = $case[2]; $started = [DateTime]::UtcNow
  $arguments = "--water-path=hamiltonian --normal-lens --boat --width=960 --height=540 --quality=balanced --frame-gen=off --frames=$frames --capture --name=$name $($case[1])"
  $p = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
  try {
    if (!$p.WaitForExit(300000)) { $p.Kill(); throw "Timed out: $name" }
    $p.Refresh()
    if ($p.ExitCode) { Get-Content "$out/lab-error.txt"; throw "Failed: $name" }
    $file = Get-Item "$out/$name.json"
    if ($file.LastWriteTimeUtc -lt $started) { throw "Stale report: $name" }
    $r = Get-Content $file -Raw | ConvertFrom-Json
    $h = $r.hamiltonian
    if ($r.frames -ne $frames -or $r.dlssEvaluations -ne $frames -or !$r.fluidSurface.surfaceBricks) { throw "Missing rendered water: $name" }
    if ($h.addedVolumeM3 -le 0 -or $h.meanDepthMetres -le $r.fluid.initialDepth) { throw "Inlet did not fill: $name" }
    if ($h.nonfinite -or $h.boundaryOverflow -or $h.steps -ne $r.fluid.steps -or $h.inverseDnoRelativeResidual -gt .02) { throw "Invalid wave state: $name" }
    if ([Math]::Abs($h.publishedMeanHeightMetres - $h.meanDepthMetres - $r.fluid.domainMinimum[1]) -gt .00003) { throw "Rendered height lost inlet volume: $name" }
    if ($h.sourceParticles -ne $r.fluid.emittedParticles -or $h.receivedParticles + $h.freeWaterParticles -ne $h.sourceParticles) { throw "Particle/wave mass ledger diverged: $name" }
    if ($h.freeWaterParticles -le 0) { throw "No visible free-water stream: $name" }
    if ($name -eq 'ham-fill-cli') {
      $expected = [Math]::PI * .2 * .2 * [Math]::Sqrt(4.5 * 4.5 + .4 * .4) * $frames / 60
      if ([Math]::Abs($h.sourceParticles * $r.fluid.particleVolume - $expected) -gt 1.1 * $r.fluid.particleVolume) { throw 'CLI inlet did not preserve the default physical flow rate.' }
    }
    if ($r.photonCounters[5] -or $r.photonCounters[6]) { throw "Nonfinite transport: $name" }
    $results += [ordered]@{ name=$name; arguments=$arguments; frames=$r.frames; adapter=$r.adapter;
      fluid=$r.fluid; hamiltonian=$h; surfaceBricks=$r.fluidSurface.surfaceBricks; photonCounters=$r.photonCounters }
    Write-Host "PASS: $name | depth $($h.meanDepthMetres) m | added $($h.addedVolumeM3) m3 | $($h.steps) substeps"
  } finally { $p.Dispose() }
}
$results | ConvertTo-Json -Depth 12 | Set-Content "$out/hamiltonian-fill-validation.json"
