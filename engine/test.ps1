param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
foreach ($case in @(
  @('standard','dxr','float'),
  @('fixed','dxr','fixed'),
  @('no-reorder','dxr-off','float'),
  @('nvapi','nvapi','float'),
  @('fallback','off','fixed')
)) {
  & "$PSScriptRoot/check.ps1" -BuildDir $BuildDir -Name "verified-$($case[0])" -Ser $case[1] -Atomics $case[2]
}
& "$PSScriptRoot/check.ps1" -BuildDir $BuildDir -Name 'verified-moving' -Animate
& "$PSScriptRoot/check.ps1" -BuildDir $BuildDir -Name 'verified-1440p' -Width 2560 -Height 1440
& "$PSScriptRoot/check.ps1" -BuildDir $BuildDir -Name 'verified-4k' -Width 3840 -Height 2160
Write-Host 'PASS: five backend/atomic selections, moving-prism invalidation, 1440p and 4K DLSS. Run the Node raw-capture validator separately.'
