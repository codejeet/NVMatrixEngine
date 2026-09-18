param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue) { throw 'Close the lab before serial GPU validation.' }
& "$out/NVMatrixEngineHamiltonianGpuTest.exe" $out
if ($LASTEXITCODE) { throw 'Hamiltonian numerical GPU fixture failed.' }
$cases = @(
  @('ham-baseline', '--water-path=baseline', 96),
  @('ham-linear', '--water-path=hamiltonian --wave-epsilon=0', 96),
  @('ham-hos2', '--water-path=hamiltonian --wave-order=2', 96),
  @('ham-hos3', '--water-path=hamiltonian --wave-order=3', 96),
  @('ham-controls', '--water-path=hamiltonian --wave-controls-test', 12)
)
foreach ($case in $cases) {
  $name = $case[0]
  $frames = $case[2]
  $started = [DateTime]::UtcNow
  $arguments = "--normal-lens --boat --frames=$frames --width=960 --height=540 --quality=balanced --frame-gen=off --capture --name=$name $($case[1])"
  $p = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
  try {
    if (!$p.WaitForExit(180000)) { $p.Kill(); throw "Hamiltonian smoke timed out: $name" }
    $p.Refresh()
    if ($p.ExitCode) { Get-Content "$out/lab-error.txt"; throw "Hamiltonian smoke failed: $name" }
    $path = "$out/$name.json"
    if ((Get-Item $path).LastWriteTimeUtc -lt $started) { throw "Stale report: $name" }
    $r = Get-Content $path -Raw | ConvertFrom-Json
    if ($r.frames -ne $frames -or $r.dlssEvaluations -ne $frames -or $r.fluidSurface.surfaceBricks -le 0) { throw "Missing rendered water: $name" }
    if ($name -eq 'ham-baseline') {
      if ($r.waterPath -ne 'baseline') { throw 'Baseline unexpectedly selected Hamiltonian water.' }
    } else {
      $h = $r.hamiltonian
      if ($r.waterPath -ne 'hamiltonian' -or !$h.enabled -or $h.steps -ne $r.fluid.steps -or $h.steps -le 0) { throw 'Wave and fluid clocks diverged.' }
      if ($h.nonfinite -or $h.boundaryOverflow -or $h.boundarySeeded -le 0 -or $h.boundaryRetired -le 0) { throw 'Invalid wave state or missing boundary exchange.' }
      if ($h.maxElevation -le 0 -or $h.maxVelocity -le 0 -or $h.inverseDnoRelativeResidual -gt .02) { throw 'Wave dynamics or inverse DNO check failed.' }
      if ($name -eq 'ham-linear' -and $h.epsilon -ne 0) { throw 'Linear ablation ignored epsilon.' }
      if ($name -eq 'ham-hos3' -and $h.hosOrder -ne 3) { throw 'HOS-3 selection ignored.' }
    }
    if ($r.photonCounters[5] -or $r.photonCounters[6]) { throw 'Nonfinite photon transport.' }
    Write-Host "PASS: $name | $($r.fluidSurface.surfaceBricks) surface bricks | $($r.fluid.steps) liquid substeps"
  } finally { $p.Dispose() }
}
foreach ($invalid in @(
  @('--water-path=unknown', 'Water path must'),
  @('--water-path=hamiltonian --wave-order=4', 'Wave order must'),
  @('--water-path=hamiltonian --wave-amplitude=nan', 'finite'),
  @('--water-path=hamiltonian --wave-amplitude=1', 'Hamiltonian waves need'),
  @('--water-path=baseline --wave-epsilon=1', 'Wave controls require')
)) {
  $p = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--frames=1 $($invalid[0])" -WorkingDirectory $out -PassThru
  try {
    if (!$p.WaitForExit(15000)) { $p.Kill(); throw 'Invalid wave settings did not fail promptly.' }
    $p.Refresh()
    if (!$p.ExitCode -or (Get-Content "$out/lab-error.txt" -Raw) -notmatch $invalid[1]) { throw "Incorrect option validation: $($invalid[0])" }
  } finally { $p.Dispose() }
}
Write-Host 'PASS: invalid path, order, nonfinite/range checks and inactive wave controls'
