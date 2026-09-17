param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop'
$dxc="$PSScriptRoot/../shared/.deps/dxc-1.9.2607/bin/x64/dxc.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
foreach($entry in @('FlowingBandMass','FlowingBandMeasure','FlowingBandDecide','FlowingBandSites','FlowingBandRestoreRequests')){
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/flowing-band.hlsl" -Fo "$OutputDir/$entry.dxil"
  if($LASTEXITCODE){throw "Flowing ownership band compilation failed: $entry"}
}
foreach($entry in @('ExchangeReset','ExchangeClear','ExchangeSeed','ExchangeVelocityDelta','ExchangeDeposit','ExchangeFree','ExchangeRestore')){
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/particle-grid-exchange.hlsl" -Fo "$OutputDir/$entry.dxil"
  if($LASTEXITCODE){throw "Fluid ownership exchange compilation failed: $entry"}
}
foreach($entry in @('GatherMass','P2G','P2GSparse')){
  $source=if($entry -eq 'GatherMass'){'binning'}else{'transfer'}
  & $dxc -T cs_6_6 -E $entry -D FLUID_PARTICLE_AUTHORITY=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$source.hlsl" -Fo "$OutputDir/Fluid$($entry)Authority.dxil"
  if($LASTEXITCODE){throw "Authoritative fluid consumer compilation failed: $entry"}
}
