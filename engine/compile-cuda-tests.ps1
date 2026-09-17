param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference='Stop'
$dxc="$PSScriptRoot/../shared/.deps/dxc-1.9.2607/bin/x64/dxc.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$stages=@(
  @('InteropTest','cuda-interop-test','CudaInteropTest'),
  @('PerturbFaces','cuda-transfer-test','CudaPerturbFaces'),
  @('SurfaceSeed','cuda-surface-test','CudaSurfaceSeed'),
  @('SurfaceProbe','cuda-surface-test','CudaSurfaceProbe'),
  @('SurfaceBoxProbe','cuda-surface-test','CudaSurfaceBoxProbe'),
  @('SurfaceGeometrySeed','cuda-surface-test','CudaSurfaceGeometrySeed'),
  @('SurfaceCurveSeed','cuda-surface-test','CudaSurfaceCurveSeed')
)
foreach($entry in 'ClearBins','CountBins','ScanCells','ScanSums','FinishScan','Scatter','SortBins') {
  $stages+=,@($entry,'binning',"Fluid$entry")
}
foreach($entry in 'P2G','G2P','Extrapolate') { $stages+=,@($entry,'transfer',"Fluid$entry") }
foreach($entry in 'Classify','Forces','Divergence','Jacobi','Project','Measure') { $stages+=,@($entry,'pressure',"Fluid$entry") }
foreach($entry in 'Viscosity','SurfaceColor','SurfaceCurvature','MaterialFixture') { $stages+=,@($entry,'material',"Fluid$entry") }
foreach($entry in 'DensityGather','DensityGatherAdaptive','DensityDisplace','DensityMeasure','DensityClearArguments','DensityContinueArguments','DensityPrepareArguments','DensityJacobi') { $stages+=,@($entry,'density',"Fluid$entry") }
foreach($entry in 'BakeSolids','Collide') { $stages+=,@($entry,'collision',"Fluid$entry") }
foreach($entry in 'SurfaceClear','SurfaceMark','SurfacePrepare','SurfaceShape','SurfaceReconstruct','SurfaceBounds','SurfaceLodBegin','SurfaceLodFingerprint','SurfaceLodPlan','SurfaceLodFine','SurfaceLodCoarse','SurfaceLodMasks','SurfaceLodAnalyze','SurfaceLodGuard','SurfaceLodRepair','SurfaceLodFinalize','SurfaceLodCommit','SurfaceLodReference') {
  $stages+=,@($entry,'reconstruction',"Fluid$entry")
}
foreach($stage in $stages) {
  & $dxc -T cs_6_6 -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$($stage[1]).hlsl" -Fo "$OutputDir/$($stage[2]).dxil"
  if($LASTEXITCODE){throw "CUDA reference shader compilation failed: $($stage[0])"}
}
foreach($entry in 'Mark','Reconstruct','Heights') {
  & $dxc -T cs_6_6 -E "Surface$entry" -HV 2021 -O3 -D FLUID_PHASE_SURFACE=1 "$PSScriptRoot/shaders/fluid/reconstruction.hlsl" -Fo "$OutputDir/FluidSurfacePhase$entry.dxil"
  if($LASTEXITCODE){throw "CUDA phase surface shader failed: $entry"}
}
& $dxc -T lib_6_6 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/cuda-surface-rays.hlsl" -Fo "$OutputDir/CudaSurfaceRays.dxil"
if($LASTEXITCODE){throw 'CUDA phase surface DXR shader failed'}
