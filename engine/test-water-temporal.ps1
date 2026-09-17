param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",
      [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Name='water-temporal',
      [switch]$Whitewater,[switch]$Adaptive,[switch]$Bulk,[switch]$BulkBounded,[switch]$PressureSupport,[switch]$Resample,[switch]$Interior,[switch]$Mac,[switch]$MacMultigrid,[switch]$CutPressure,[switch]$TimeCentered,[switch]$SparseWork,[switch]$SurfaceLod,
      [ValidateSet('x','y','z','xy','xz','yz','xyz')][string]$SurfaceLodAxes='xyz',
      [ValidateSet('uniform','active','multigrid')][string]$Pressure='uniform',
      [ValidateSet('off','adaptive','uniform')][string]$Optical='off',
      [ValidateSet('dx12','cuda')][string]$Backend='dx12',[switch]$CudaGraphs,
      [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
      [ValidateSet('conditional','unrolled')][string]$CudaPressureLoop='conditional',
      [ValidateSet('primary','cig')][string]$CudaContext='primary')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
$cudaLaunch=Get-CudaTestArguments $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the game/lab before water temporal validation.'}
$extra=if($Whitewater){''}else{'--no-whitewater'}
if($Adaptive){$extra+=' --fluid-complexity-validate'}
if($Bulk){$extra+=' --fluid-bulk-validate'}
if($BulkBounded){$extra+=' --fluid-bulk-bounded'}
if($PressureSupport){$extra+=' --fluid-bulk-pressure'}
if($Resample){$extra+=' --fluid-resample'}
if($Interior){$extra+=' --fluid-interior'}
if($Mac){$extra+=' --fluid-mac'}
if($MacMultigrid){$extra+=' --fluid-mac-solver=multigrid'}
if($CutPressure){$extra+=' --fluid-cut-pressure'}
if($TimeCentered){$extra+=' --fluid-cut-time-centered'}
if($SparseWork){$extra+=' --fluid-sparse-work'}
if($SurfaceLod){$extra+=" --fluid-surface-lod-axes=$SurfaceLodAxes"}
if($Optical -eq 'adaptive'){$extra+=' --adaptive-rays'}
if($Optical -eq 'uniform'){$extra+=' --optical-reference'}
$extra+=" --fluid-pressure=$Pressure --fluid-backend=$Backend $cudaLaunch"
$started=[DateTime]::UtcNow
$p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-temporal-test --frames=320 --frame-gen=off --width=960 --height=540 --quality=balanced --capture --name=$Name $extra" -WorkingDirectory $out -PassThru
$null=$p.Handle
try{
  if(!$p.WaitForExit(120000)){$p.Kill();throw 'Water temporal test timed out'}
  $p.Refresh();if($null -eq $p.ExitCode -or $p.ExitCode -ne 0){throw 'Water temporal test failed'}
  foreach($f in 128,176,224,272,320){
    $file="$out/$Name-$f.json"
    if((Get-Item $file).LastWriteTimeUtc -lt $started){throw 'Stale temporal capture'}
    $r=Get-Content $file -Raw|ConvertFrom-Json
    Assert-CudaTestMode $r $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
    if($r.frames -ne $f -or $r.dlssEvaluations -ne $f -or $r.rrHistoryResets -ne 1){throw 'Camera movement invalidated RR'}
    if($r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid photon accumulation'}
    if(($BulkBounded -or $PressureSupport) -and (!$r.fluid.bulk.phaseLimiter -or $r.fluid.bulk.phaseLimiter.exhaustedSteps -or $r.fluid.bulk.invalid)){throw 'Invalid bounded transport in temporal sequence'}
    if($PressureSupport -and (!$r.fluid.bulkPressure -or !$r.fluid.cutCells.timeCenteredPressure -or !$r.fluid.bulkPressure.projectionCalls -or $r.fluid.bulkPressure.invalid)){throw 'Missing or invalid pressure support in temporal sequence'}
    Write-Host "PASS $Name frame $f | atlas resets $($r.historyResets) / RR resets $($r.rrHistoryResets)"
  }
}finally{$p.Dispose()}
