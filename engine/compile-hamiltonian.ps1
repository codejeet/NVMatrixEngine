param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference = 'Stop'
$dxc = "$PSScriptRoot/../shared/.deps/dxc-1.9.2607/bin/x64/dxc.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
foreach ($entry in 'Init','FFT','Operator','Product','Bernoulli','Integrate','Publish','Snapshot','Smooth','Relax','ClearFree','Validate','TargetHeight','Cull','Seed','Emit','FreeWater','Transfers','Activity','Regions') {
  $source = if ($entry -in 'TargetHeight','Cull','Seed','Emit','FreeWater','Activity','Regions') { 'hamiltonian-coupling' } else { 'hamiltonian' }
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$source.hlsl" -Fo "$OutputDir/Hamiltonian$entry.dxil"
  if ($LASTEXITCODE) { throw "Hamiltonian compilation failed: $entry" }
  if ($source -eq 'hamiltonian') {
    foreach ($size in 128,256) {
      $log2 = if ($size -eq 128) { 7 } else { 8 }
      & $dxc -T cs_6_6 -E $entry -D WAVE_FFT_SIZE=$size -D WAVE_FFT_LOG2=$log2 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$source.hlsl" -Fo "$OutputDir/Hamiltonian$($entry)$size.dxil"
      if ($LASTEXITCODE) { throw "Hamiltonian compilation failed: $entry / $size" }
    }
  }
}
foreach ($entry in 'Classify','Forces','Divergence','Project','P2G','Initialize','DensityGather','DensityGatherAdaptive','DensityDisplace','SurfaceMark','SurfaceReconstruct','SurfaceFreeClear','SurfaceFreeBin') {
  $source = if ($entry.StartsWith('Surface')) { 'reconstruction' } elseif ($entry.StartsWith('Density')) { 'density' } elseif ($entry -eq 'P2G') { 'transfer' } elseif ($entry -eq 'Initialize') { 'particles' } else { 'pressure' }
  & $dxc -T cs_6_6 -E $entry -D FLUID_HAMILTONIAN=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$source.hlsl" -Fo "$OutputDir/Fluid$($entry)Hamiltonian.dxil"
  if ($LASTEXITCODE) { throw "Hamiltonian fluid compilation failed: $entry" }
}

& $dxc -T cs_6_6 -E BuoyancySample -D FLUID_HAMILTONIAN=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/buoyancy.hlsl" -Fo "$OutputDir/BuoyancySampleHamiltonian.dxil"
if ($LASTEXITCODE) { throw 'Hamiltonian buoyancy compilation failed.' }
