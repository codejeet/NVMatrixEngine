param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build",
  [ValidateSet('auto','dxr','nvapi','off','dxr-off')][string]$Ser = 'auto',
  [ValidateSet('auto','float','fixed')][string]$Atomics = 'auto',
  [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Name = 'lab',
  [int]$Width = 960, [int]$Height = 540, [int]$Frames = 96,
  [int]$Photons = 65536, [switch]$Animate, [switch]$DebugLayer
)
$ErrorActionPreference = 'Stop'
$out = "$BuildDir/bin/Release"
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab before a GPU test.' }
$argsList = @("--width=$Width","--height=$Height","--frames=$Frames","--photons=$Photons","--ser=$Ser","--atomics=$Atomics","--name=$Name",'--capture','--fixture')
if ($Animate) { $argsList += '--animate' }
if ($DebugLayer) { $argsList += '--debug' }
$process = Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $argsList -WorkingDirectory $out -PassThru
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Lab test exceeded its 60-second safety cap.' }
$process.Refresh()
if ($process.ExitCode -ne 0) { if (Test-Path "$out/lab-error.txt") { Get-Content "$out/lab-error.txt" }; throw "Lab failed: $($process.ExitCode)" }
$report = Get-Content "$out/$Name.json" -Raw | ConvertFrom-Json
if ($report.frames -ne $Frames -or $report.dlssEvaluations -ne $Frames) { throw 'Incomplete rendering/DLSS test.' }
if ($report.rrHistoryResets -ne 1) { throw 'Continuous fixture motion reset global RR history.' }
if ($report.photonCounters[0] -ne $Photons -or $report.photonCounters[3] -le 0) { throw 'Photon pass did not produce caustics.' }
if ($report.photonCounters[5] -ne 0 -or $report.photonCounters[6] -ne 0) { throw 'Photon overflow/invalid state.' }
if (!$Animate -and $report.historyResets -ne 1) { throw 'Static caustics history unexpectedly reset.' }
if (!$Animate -and [Math]::Abs($report.prismAngle - [Math]::PI/6) -gt 0.00001) { throw 'Automated prism angle changed.' }
if ($Animate -and $report.historyResets -ne $Frames) { throw 'Moving-prism history did not invalidate every frame.' }
$report | Select-Object adapter,shaderModel,raytracingTier,hitMode,serActive,floatAtomics,frames,historyResets,medianMs,photonCounters | ConvertTo-Json
Write-Host "PASS: $Name"
