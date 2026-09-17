param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before rolling validation.' }
$started = [DateTime]::UtcNow
$process = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList '--rolling-test --frames=300 --width=1920 --height=1080 --capture --name=rolling' -WorkingDirectory $out -PassThru
try {
  if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Rolling validation timed out.' }
  $process.Refresh()
  if ($process.ExitCode -ne 0) { Get-Content "$out/lab-error.txt"; throw 'Rolling validation failed.' }
  $path = "$out/rolling.json"
  if ((Get-Item $path).LastWriteTimeUtc -lt $started) { throw 'Stale rolling report.' }
  $r = Get-Content $path -Raw | ConvertFrom-Json
  $g = Get-Content "$out/rolling.game.json" -Raw | ConvertFrom-Json
  if (!$r.rollingTest -or $r.frames -ne 300 -or $r.dlssEvaluations -ne 300 -or $r.rrHistoryResets -ne 1) { throw 'Rolling disrupted rendering/history.' }
  if ($g.jumps -ne 1) { throw 'Rolling validation did not exercise jumping.' }
  if ($r.photonCounters[5] -or $r.photonCounters[6]) { throw 'Invalid photon accumulation.' }
  Write-Host 'PASS: 300 rolling/reversal/jump frames | 1 RR reset | zero photon errors'
} finally { $process.Dispose() }
