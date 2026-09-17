param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
      [ValidateSet('dx12','cuda')][string]$Backend='dx12',[switch]$CudaGraphs,
      [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
      [ValidateSet('conditional','unrolled')][string]$CudaPressureLoop='conditional',
      [ValidateSet('primary','cig')][string]$CudaContext='primary')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
$cudaLaunch=Get-CudaTestArguments $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before fluid material validation.'}
$cases=@(
  @('diffusion-zero',4,'--fluid-material-test=1 --fluid-viscosity=0'),
  @('diffusion',4,'--fluid-material-test=1 --fluid-viscosity=.05'),
  @('diffusion-subcycle',4,'--fluid-material-test=1 --fluid-viscosity=.5'),
  @('curvature',4,'--fluid-material-test=2'),
  @('inviscid',180,'--fluid-viscosity=0 --fluid-surface-tension=0'),
  @('water',180,''),
  @('viscous',180,'--fluid-viscosity=.05'),
  @('capillary',180,'--fluid-surface-tension=2')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if($CudaPressure -ne 'uniform' -and $case[2] -match 'fluid-material-test'){
    Write-Host "SKIP $($case[0]): partial fixture does not run a complete MGPCG substep";continue
  }
  $name="fluid-material-$($case[0])";$started=[DateTime]::UtcNow
  if($Backend -eq 'cuda'){$name="cuda-$name"}
  if($CudaGraphs){$name="cuda-graphs-"+$name.Substring(5)}
  if($CudaPressure -ne 'uniform'){$name="cuda-$CudaPressure-"+$name.Substring(5)}
  if($CudaPressureLoop -eq 'unrolled'){$name="unrolled-$name"}
  if($CudaContext -eq 'cig'){$name="cig-$name"}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-validate --fluid-backend=$Backend $cudaLaunch --fluid-solver-only --width=640 --height=360 --frames=$($case[1]) --name=$name $($case[2])" -WorkingDirectory $out -PassThru
  $null=$p.Handle
  try {
    if(!$p.WaitForExit(60000)){$p.Kill();throw "Fluid material timeout: $name"}
    $p.Refresh();if($null -eq $p.ExitCode -or $p.ExitCode -ne 0){Get-Content "$out/lab-error.txt";throw "Fluid material failed: $name"}
    if((Get-Item "$out/$name.json").LastWriteTimeUtc -lt $started){throw 'Stale material report'}
    $r=Get-Content "$out/$name.json" -Raw|ConvertFrom-Json
    Assert-CudaTestMode $r $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
    if(!$r.fluid.validated -or $r.frames -ne $case[1] -or $r.dlssEvaluations -ne $case[1]){throw 'Incomplete material test'}
    if($case[0] -match '^diffusion' -and !$r.fluid.viscosityTestSamples){throw 'Missing analytic diffusion checks'}
    if($case[0] -eq 'diffusion-subcycle' -and $r.fluid.viscositySubcycles -le 1){throw 'Diffusion did not subcycle'}
    if($case[0] -eq 'curvature' -and !$r.fluid.curvatureTestSamples){throw 'Missing sphere curvature checks'}
    Write-Host "PASS $name | viscosity error $($r.fluid.maxViscosityError) | curvature error $($r.fluid.maxCurvatureRelativeError) | residual $($r.fluid.maxPressureResidualMismatch)"
  }finally{$p.Dispose()}
}
foreach($case in @(@('invalid-nan','--fluid-viscosity=NaN','finite, nonnegative'),
                  @('invalid-diffusion','--fluid-viscosity=100','32 diffusion subcycles'),
                  @('invalid-capillary','--fluid-surface-tension=10000','smaller simulation timestep'))){
  if($case[0] -notmatch $CaseFilter){continue}
  $started=[DateTime]::UtcNow
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-validate --fluid-backend=$Backend --fluid-solver-only --frames=1 --name=$($case[0]) $($case[1])" -WorkingDirectory $out -PassThru
  try {
    if(!$p.WaitForExit(60000)){$p.Kill();throw 'Invalid material rejection timeout'}
    $p.Refresh()
    if(!$p.ExitCode -or (Get-Item "$out/lab-error.txt").LastWriteTimeUtc -lt $started -or
       (Get-Content "$out/lab-error.txt" -Raw) -notmatch $case[2]){throw "Material was not rejected correctly: $($case[0])"}
    Write-Host "PASS $($case[0]) rejected before simulation"
  }finally{$p.Dispose()}
}
