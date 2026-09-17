param([Parameter(Mandatory)][string]$OutputDir, [switch]$SpacetimeExperiment)
$ErrorActionPreference = 'Stop'
$deps = "$PSScriptRoot/../shared/.deps"
$dxc = "$deps/dxc-1.9.2607/bin/x64/dxc.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
& $dxc -T cs_6_6 -E BuoyancySample -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/buoyancy.hlsl" -Fo "$OutputDir/BuoyancySample.dxil"
if ($LASTEXITCODE) { throw 'Buoyancy sample compilation failed' }
& "$PSScriptRoot/compile-fluid-precision.ps1" -OutputDir $OutputDir
& "$PSScriptRoot/compile-fluid-exchange.ps1" -OutputDir $OutputDir
if ($SpacetimeExperiment) {
  & "$PSScriptRoot/compile-fluid-spacetime.ps1" -OutputDir $OutputDir
}
& $dxc -T cs_6_6 -E MacApplyCapacity -D CUT_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac.hlsl" -Fo "$OutputDir/MacApplyCapacityCut.dxil"
if ($LASTEXITCODE) { throw 'Capacity-to-MAC compilation failed' }
foreach ($entry in 'CarrierClear','CarrierFaces','CarrierRows','CarrierSweep','CarrierResidual','CarrierPrepare','CarrierExport','CarrierFinish','CarrierMacInitialize','CarrierLiquidSweep','CarrierPressureFaces','CarrierLiquidResidual','CarrierLiquidRhs','CarrierApplyMultigrid') {
  & $dxc -T cs_6_6 -E $entry -D CARRIER_COUPLED=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/carrier-projection.hlsl" -Fo "$OutputDir/$entry-coupled.dxil"
  if ($LASTEXITCODE) { throw "Coupled carrier compilation failed: $entry" }
}
foreach ($entry in 'CarrierClear','CarrierFaces','CarrierRows','CarrierSweep','CarrierResidual','CarrierPrepare','CarrierExport','CarrierFinish') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/carrier-projection.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Air carrier compilation failed: $entry" }
}
foreach ($entry in 'ImplicitClear','ImplicitInitialize','ImplicitIterate','ImplicitResidual','ImplicitPrepare','ImplicitExport','ImplicitFinish') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/bulk-implicit.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Implicit transport compilation failed: $entry" }
}
foreach ($entry in 'BulkPressureClear','BulkPressureCells','BulkPressureFaces') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/bulk-pressure.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Bulk pressure coverage compilation failed: $entry" }
}
& $dxc -T cs_6_6 -E SurfaceColor -D BULK_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/material.hlsl" -Fo "$OutputDir/FluidSurfaceColorBulk.dxil"
if ($LASTEXITCODE) { throw 'Bulk capillary occupancy compilation failed' }
foreach ($entry in 'DensityGather','DensityGatherAdaptive') {
  & $dxc -T cs_6_6 -E $entry -D BULK_PRESSURE=1 -D CUT_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/density.hlsl" -Fo "$OutputDir/Fluid$($entry)CutBulk.dxil"
  if ($LASTEXITCODE) { throw "Bulk density support compilation failed: $entry" }
}
foreach ($entry in 'CutCorners','CutVolumes','CutAreas','CutRestrictVolumes','CutRestrictAreas','CutReduce','CutTotals','CutSolidKernel','CutKernelClear','CutKernelClassify','CutKernelPrepare') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/cut-cells.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Cut-cell geometry compilation failed: $entry" }
}
foreach ($stage in @(@('CutVS','vs_6_6'),@('CutPS','ps_6_6'))) {
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/cut-cells.hlsl" -Fo "$OutputDir/$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw "Cut-cell debug compilation failed: $($stage[0])" }
}
foreach ($entry in 'MgClear','MgAssemble','MgCanonicalFaces','MgPrepare','MgFactor','MgInitialize','MgResetCorrection','MgFineSmooth','MgRestrictBase','MgCoarseSmooth','MgRestrictCoarse','MgBottom','MgProlongCoarse','MgProlongFine','MgDotPreconditioned','MgDirection','MgApply','MgUpdate','MgResidual','MgReduce','MgCoarseCycle','MgImportCorrection','MgExportCorrection') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac-pressure.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Mixed MAC MGPCG compilation failed: $entry" }
  & $dxc -T cs_6_6 -E $entry -D CUT_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac-pressure.hlsl" -Fo "$OutputDir/$($entry)Cut.dxil"
  if ($LASTEXITCODE) { throw "Cut-cell MGPCG compilation failed: $entry" }
}
foreach ($entry in 'MacClear','MacClassify','MacLeaves','MacPrepare','MacRestrict','MacAssemble','MacSmooth','MacProject','MacProlongate','MacMeasure') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Adaptive MAC compilation failed: $entry" }
  & $dxc -T cs_6_6 -E $entry -D CUT_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac.hlsl" -Fo "$OutputDir/$($entry)Cut.dxil"
  if ($LASTEXITCODE) { throw "Cut-cell MAC compilation failed: $entry" }
}
foreach ($stage in @(@('MacVS','vs_6_6'),@('MacPS','ps_6_6'))) {
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac.hlsl" -Fo "$OutputDir/$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw "Adaptive MAC debug compilation failed: $($stage[0])" }
}
foreach ($entry in 'MacAssemble','MacProject') {
  & $dxc -T cs_6_6 -E $entry -D PRECISE_MAC=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac.hlsl" -Fo "$OutputDir/$($entry)Precise.dxil"
  if ($LASTEXITCODE) { throw "Precise mixed MAC compilation failed: $entry" }
  & $dxc -T cs_6_6 -E $entry -D PRECISE_MAC=1 -D CUT_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/mac.hlsl" -Fo "$OutputDir/$($entry)CutPrecise.dxil"
  if ($LASTEXITCODE) { throw "Precise cut-cell MAC compilation failed: $entry" }
}
foreach ($entry in 'Classify','Forces','Divergence','Viscosity','SurfaceCurvature','DensityGather','DensityGatherAdaptive','DensityDisplace') {
  $source=if($entry.StartsWith('Density')){'density'}elseif($entry -in @('Viscosity','SurfaceCurvature')){'material'}else{'pressure'}
  & $dxc -T cs_6_6 -E $entry -D CUT_PRESSURE=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$source.hlsl" -Fo "$OutputDir/Fluid$($entry)Cut.dxil"
  if ($LASTEXITCODE) { throw "Cut-cell fluid compilation failed: $entry" }
}
foreach ($entry in 'InteriorBegin','InteriorClearExchange','InteriorFreeSlots','InteriorFreeSlotsOrdered','InteriorRestore','InteriorRestoreOrdered','InteriorDeposit','InteriorAdvect','InteriorCount','InteriorPrepare') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/interior.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Interior ownership compilation failed: $entry" }
}
foreach ($entry in 'ResampleClear','ResampleMerge','ResampleSelect','ResampleFree','ResampleFreeOrdered','ResampleSplit','ResampleSplitOrdered','ResampleCount','ResampleRebin','ResamplePlan') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/resampling.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Particle resampling compilation failed: $entry" }
}
foreach ($entry in 'BulkClearFrame','BulkSources','BulkRestrictFaces','BulkForces','BulkLimit','BulkFlux','BulkUpdate','BulkReduce','BulkTotals') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/bulk.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Bulk inventory compilation failed: $entry" }
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 -D BULK_PROJECTED=1 "$PSScriptRoot/shaders/fluid/bulk.hlsl" -Fo "$OutputDir/$entry-projected.dxil"
  if ($LASTEXITCODE) { throw "Projected bulk compilation failed: $entry" }
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 -D BULK_PROJECTED=1 -D BULK_CAPACITY=1 "$PSScriptRoot/shaders/fluid/bulk.hlsl" -Fo "$OutputDir/$entry-capacity.dxil"
  if ($LASTEXITCODE) { throw "Capacity bulk compilation failed: $entry" }
}
foreach ($entry in 'AllocationClear','AllocationAccept','AllocationPrepare','AllocationFlux','AllocationUpdate','AllocationReduce','AllocationTotals') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/volume-allocation.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Source allocation compilation failed: $entry" }
}
foreach ($entry in 'FluxLimitClear','FluxLimitBegin','FluxLimitEvaluate','FluxLimitPrepare','FluxLimitFaces','FluxLimitAudit','FluxLimitFinish') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/flux-limiter.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Phase flux limiter compilation failed: $entry" }
}
foreach ($stage in @(@('BulkVS','vs_6_6'),@('BulkPS','ps_6_6'))) {
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/bulk.hlsl" -Fo "$OutputDir/$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw "Bulk debug compilation failed: $($stage[0])" }
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 -D BULK_PROJECTED=1 "$PSScriptRoot/shaders/fluid/bulk.hlsl" -Fo "$OutputDir/$($stage[0])-projected.dxil"
  if ($LASTEXITCODE) { throw "Projected bulk debug compilation failed: $($stage[0])" }
}
foreach ($mode in 0,1,2) {
  foreach ($atomic in 0,1) {
    $target = if ($mode -eq 2) { 'lib_6_9' } else { 'lib_6_6' }
    & $dxc -T $target -HV 2021 -enable-16bit-types -O3 -I "$deps/nvapi" -I "$deps/rtxdi-f12037fa/Include" -D "HIT_MODE=$mode" -D "FLOAT_ATOMICS=$atomic" "$PSScriptRoot/shaders/transport.hlsl" -Fo "$OutputDir/Transport-$mode-$atomic.dxil"
    if ($LASTEXITCODE) { throw "Raygen compilation failed: $mode/$atomic" }
  }
}
& $dxc -T cs_6_6 -E OpticalFinalize -HV 2021 -O3 "$PSScriptRoot/shaders/optical-importance.hlsl" -Fo "$OutputDir/OpticalFinalize.dxil"
if ($LASTEXITCODE) { throw 'Optical importance compilation failed' }
foreach ($entry in 'Clear','Accumulate','Composite') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/resolve.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Resolve compilation failed: $entry" }
}
foreach ($stage in @(@('VS','vs_6_0'),@('PS','ps_6_0'))) {
  & $dxc -T $stage[1] -E $stage[0] -O3 "$PSScriptRoot/shaders/present.hlsl" -Fo "$OutputDir/$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw 'Presentation compilation failed.' }
}
foreach ($stage in @(@('FGComposite','ps_6_6'),@('FGDepth','cs_6_6'))) {
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/present.hlsl" -Fo "$OutputDir/$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw 'Frame generation handoff compilation failed.' }
}
& $dxc -T cs_6_6 -E FGDistortion -HV 2021 -O3 "$PSScriptRoot/shaders/lens-distortion.hlsl" -Fo "$OutputDir/FGDistortion.dxil"
if ($LASTEXITCODE) { throw 'Frame generation lens distortion compilation failed.' }
foreach ($entry in 'WhitewaterClear','WhitewaterUpdate','FoamClear','FoamSplat','FoamTransport') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/whitewater.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Whitewater compilation failed: $entry" }
}
foreach ($entry in 'Initialize','Integrate','Emit') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/particles.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid compilation failed: $entry" }
}
foreach ($entry in 'Snapshot','BakeSolids','Collide') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/collision.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid collision compilation failed: $entry" }
}
foreach ($entry in 'SurfaceClear','SurfaceMark','SurfacePrepare','SurfaceShape','SurfaceReconstruct','SurfaceBounds','SurfaceLodBegin','SurfaceLodFingerprint','SurfaceLodPlan','SurfaceLodFine','SurfaceLodCoarse','SurfaceLodMasks','SurfaceLodAnalyze','SurfaceLodGuard','SurfaceLodRepair','SurfaceLodFinalize','SurfaceLodCommit','SurfaceLodReference') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/reconstruction.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid surface compilation failed: $entry" }
}
foreach ($entry in 'SurfaceMark','SurfaceReconstruct','SurfaceLodBegin','SurfaceLodFingerprint','SurfaceLodPlan','SurfaceLodFine','SurfaceLodCoarse','SurfaceLodMasks','SurfaceLodAnalyze','SurfaceLodGuard','SurfaceLodRepair','SurfaceLodFinalize','SurfaceLodCommit','SurfaceLodReference') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 -D FLUID_NARROW_SURFACE=1 "$PSScriptRoot/shaders/fluid/reconstruction.hlsl" -Fo "$OutputDir/Fluid$($entry)Narrow.dxil"
  if ($LASTEXITCODE) { throw "Fluid narrow-band surface shader failed: $entry" }
}
foreach ($entry in 'Mark','Reconstruct','Heights') {
  & $dxc -T cs_6_6 -E "Surface$entry" -HV 2021 -O3 -D FLUID_PHASE_SURFACE=1 "$PSScriptRoot/shaders/fluid/reconstruction.hlsl" -Fo "$OutputDir/FluidSurfacePhase$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid phase surface shader failed: $entry" }
}
foreach ($stage in @(@('ComplexityClear','cs_6_6'),@('ComplexityClassify','cs_6_6'),@('ComplexitySchedule','cs_6_6'),@('ComplexityPrepare','cs_6_6'),@('ComplexityVS','vs_6_6'),@('ComplexityPS','ps_6_6'))) {
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/complexity.hlsl" -Fo "$OutputDir/$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw "Fluid complexity compilation failed: $($stage[0])" }
}
foreach ($entry in 'ClearBins','CountBins','ScanCells','ScanSums','FinishScan','Scatter','SortBins') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/binning.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid binning compilation failed: $entry" }
}
foreach ($entry in 'P2G','P2GSparse','G2P','Extrapolate') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/transfer.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid transfer compilation failed: $entry" }
}
foreach ($entry in 'Classify','Forces','Divergence','Jacobi','Project','Measure') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/pressure.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid projection compilation failed: $entry" }
}
& $dxc -T cs_6_6 -E DensityMeasure -HV 2021 -O3 -D FLUID_NARROW_DENSITY=1 "$PSScriptRoot/shaders/fluid/density.hlsl" -Fo "$OutputDir/FluidDensityMeasureNarrow.dxil"
if ($LASTEXITCODE) { throw 'Fluid narrow-band density shader failed' }
foreach ($entry in 'DensityGather','DensityJacobi','DensityTileJacobi','DensityTileSparse','DensityDisplace','DensityMeasure','DensityGatherAdaptive','DensityClearArguments','DensityContinueArguments','DensityPrepareArguments') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/density.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid density compilation failed: $entry" }
}
foreach ($entry in 'WorkReset','WorkBegin','WorkClearFaces','WorkMarkFaces','WorkCompactFaces','WorkPrepareFaces','WorkClearDensity','WorkDensity','WorkDensityRepair','WorkPrepareDensity','WorkZeroPressure','WorkCompareFaces','WorkCompareDensity') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/work.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid work compilation failed: $entry" }
}
foreach ($entry in 'PressureClear','PressureActive','PressureAssemble','PressurePrepare','PressureSmooth','PressureTileSmooth','PressureRestrict','PressureCoarseSmooth','PressureProlongate','PressureResidual') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/pressure-hierarchy.hlsl" -Fo "$OutputDir/$entry.dxil"
  if ($LASTEXITCODE) { throw "Pressure hierarchy compilation failed: $entry" }
}
foreach ($entry in 'Viscosity','SurfaceColor','SurfaceCurvature','MaterialFixture') {
  & $dxc -T cs_6_6 -E $entry -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/material.hlsl" -Fo "$OutputDir/Fluid$entry.dxil"
  if ($LASTEXITCODE) { throw "Fluid material compilation failed: $entry" }
}
foreach ($stage in @(@('DebugVS','vs_6_6'),@('DebugPS','ps_6_6'))) {
  & $dxc -T $stage[1] -E $stage[0] -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/debug.hlsl" -Fo "$OutputDir/Fluid$($stage[0]).dxil"
  if ($LASTEXITCODE) { throw 'Fluid debug compilation failed.' }
}
