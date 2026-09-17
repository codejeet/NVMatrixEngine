param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build",
  [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Name = 'orbit',
  [ValidateSet('auto','dxr','nvapi','off','dxr-off')][string]$Ser = 'auto'
)
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before orbit validation.' }
$started = [DateTime]::UtcNow
$arguments = "--orbit-test --frames=300 --width=1920 --height=1080 --capture --name=$Name --ser=$Ser"
$process = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $arguments -WorkingDirectory $out -PassThru
try {
  if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Orbit validation timed out.' }
  $process.Refresh()
  if ($process.ExitCode -ne 0) { Get-Content "$out/lab-error.txt"; throw 'Orbit validation failed.' }
  $path = "$out/$Name.json"
  if ((Get-Item $path).LastWriteTimeUtc -lt $started) { throw 'Stale orbit report.' }
  $r = Get-Content $path -Raw | ConvertFrom-Json
  if (!$r.orbitTest -or $r.frames -ne 300 -or $r.dlssEvaluations -ne 300 -or $r.rrHistoryResets -ne 1) { throw 'Orbit disrupted rendering/history.' }
  if ($r.photonCounters[5] -or $r.photonCounters[6]) { throw 'Invalid photon accumulation.' }
  Write-Host "PASS: $Name | 300 orbit frames | 1 RR reset | zero photon errors"
} finally { $process.Dispose() }
