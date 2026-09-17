param([Parameter(Mandatory)][string]$OutputDir)
$ErrorActionPreference = 'Stop'
$dxc = "$PSScriptRoot/../shared/.deps/dxc-1.9.2607/bin/x64/dxc.exe"
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$groups = @(
  @{Source='cut-cells'; Entries=@('CutCorners','CutVolumes','CutAreas','CutRestrictVolumes','CutRestrictAreas','CutReduce','CutTotals','CutSolidKernel','CutKernelClear','CutKernelClassify','CutKernelPrepare')},
  @{Source='bulk'; Entries=@('BulkClearFrame','BulkSources','BulkRestrictFaces','BulkForces','BulkLimit','BulkFlux','BulkUpdate','BulkReduce','BulkTotals')},
  @{Source='bulk-pressure'; Entries=@('BulkPressureClear','BulkPressureCells','BulkPressureFaces')},
  @{Source='bulk-implicit'; Entries=@('ImplicitClear','ImplicitInitialize','ImplicitIterate','ImplicitResidual','ImplicitPrepare','ImplicitExport','ImplicitFinish')},
  @{Source='volume-allocation'; Entries=@('AllocationClear','AllocationAccept','AllocationPrepare','AllocationFlux','AllocationUpdate','AllocationReduce','AllocationTotals')}
)
foreach ($group in $groups) {
  foreach ($entry in $group.Entries) {
    & $dxc -T cs_6_6 -E $entry -D BULK_PRECISE=1 -D BULK_PROJECTED=1 -D BULK_CAPACITY=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$($group.Source).hlsl" -Fo "$OutputDir/$entry-precise.dxil"
    if ($LASTEXITCODE) { throw "Precise fluid compilation failed: $entry" }
  }
}
foreach ($source in 'bulk','cut-cells') {
  $prefix = if ($source -eq 'bulk') { 'Bulk' } else { 'Cut' }
  foreach ($stage in @(@('VS','vs_6_6'),@('PS','ps_6_6'))) {
    $entry = "$prefix$($stage[0])"
    & $dxc -T $stage[1] -E $entry -D BULK_PRECISE=1 -D BULK_PROJECTED=1 -D BULK_CAPACITY=1 -HV 2021 -O3 "$PSScriptRoot/shaders/fluid/$source.hlsl" -Fo "$OutputDir/$entry-precise.dxil"
    if ($LASTEXITCODE) { throw "Precise fluid debug compilation failed: $entry" }
  }
}
