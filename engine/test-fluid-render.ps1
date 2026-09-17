param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
      [ValidateSet('dx12','cuda')][string]$Backend='dx12',[switch]$CudaGraphs,
      [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
      [ValidateSet('conditional','unrolled')][string]$CudaPressureLoop='conditional',
      [ValidateSet('primary','cig')][string]$CudaContext='primary')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
$cudaLaunch=Get-CudaTestArguments $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before fluid render validation.'}
$cases=@(
  @('empty',4,'--fluid-particles=0'),
  @('sphere',4,'--fluid-surface-fixture=1'),
  @('sheet',4,'--fluid-surface-fixture=2'),
  @('fall',45,''),@('splash',90,''),@('pool',240,''),
  @('colliders',180,'--fluid-collider-test'),@('isotropic',90,'--fluid-isotropic'),
  @('fixed',90,'--atomics=fixed'),@('ser',45,'--ser=dxr'),@('nvapi',45,'--ser=nvapi'),
  @('surface-controls',12,'--fluid-surface-controls-test')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  $name="fluid-render-$($case[0])";$started=[DateTime]::UtcNow
  if($Backend -eq 'cuda'){$name="cuda-$name"}
  if($CudaGraphs){$name="cuda-graphs-"+$name.Substring(5)}
  if($CudaPressure -ne 'uniform'){$name="cuda-$CudaPressure-"+$name.Substring(5)}
  if($CudaPressureLoop -eq 'unrolled'){$name="unrolled-$name"}
  if($CudaContext -eq 'cig'){$name="cig-$name"}
  $view=$cudaLaunch + $(if($case[0] -eq 'surface-controls'){''}else{' --fluid-view'})
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-validate --fluid-backend=$Backend $view --width=960 --height=540 --frames=$($case[1]) --capture --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  $null=$p.Handle
  try{
    if(!$p.WaitForExit(60000)){$p.Kill();throw "Fluid render timeout: $name"}
    $p.Refresh();if($null -eq $p.ExitCode -or $p.ExitCode -ne 0){Get-Content "$out/lab-error.txt";throw "Fluid render failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale fluid render report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    Assert-CudaTestMode $r $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
    if($r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1] -or !$r.fluid.validated){throw 'Incomplete fluid render'}
    if($r.fluidProbes.badRoots -or $r.fluidProbes.truncated -or $r.photonCounters[5] -or $r.photonCounters[6]){throw 'Invalid fluid intersection/transport'}
    if($r.fluidProbes.glassShellHits -ne 10){throw 'Missing glass shell entry/exit/corner paths'}
    if($case[0] -ne 'empty' -and (!$r.fluidProbes.hits -or !$r.fluidSurface.surfaceBricks -or !$r.waterPhotonEntries)){throw 'Missing fluid geometry/optical paths'}
    if($case[0] -ne 'surface-controls' -and $r.rrHistoryResets -ne 1){throw 'Simulation reset global RR history'}
    if($case[0] -eq 'surface-controls' -and $r.rrHistoryResets -ne 7){throw 'Surface view/reset changes did not reset RR as explicit cuts'}
    if($case[0] -eq 'sphere' -and [Math]::Abs($r.fluidSurface.tetrahedralVolumeEstimate/(4*[Math]::PI*.75*.75*.75/3)-1) -gt .02){throw 'Analytic sphere volume mismatch'}
    if($case[0] -eq 'sheet' -and [Math]::Abs($r.fluidSurface.tetrahedralVolumeEstimate/.08-1) -gt .05){throw 'Thin-sheet volume mismatch'}
    Write-Host "PASS $name | roots $($r.fluidProbes.hits) | reconstruct $($r.fluidSurface.reconstructionMs) ms | BLAS $($r.fluidSurface.blasMs) ms | median frame $($r.medianMs[5]) ms"
  }finally{$p.Dispose()}
}
