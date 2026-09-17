param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop'
$dxc="$PSScriptRoot/../shared/.deps/dxc-1.9.2607/bin/x64/dxc.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
foreach($entry in 'SpacetimeReset','SpacetimeSeed','SpacetimeAdvect','SpacetimeSynchronize','SpacetimePhaseFaces') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/spacetime.hlsl" -Fo "$OutputDir/$entry.dxil"
  if($LASTEXITCODE){throw "Spacetime compilation failed: $entry"}
}
& $dxc -T cs_6_6 -E P2G -D FLUID_SPACETIME=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/transfer.hlsl" -Fo "$OutputDir/FluidP2GSpacetime.dxil"
if($LASTEXITCODE){throw 'Spacetime P2G compilation failed'}
