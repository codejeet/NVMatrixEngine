param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',
      [ValidateSet('dx12','cuda')][string]$Backend='dx12',[switch]$CudaGraphs,
      [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
      [ValidateSet('conditional','unrolled')][string]$CudaPressureLoop='conditional',
      [ValidateSet('primary','cig')][string]$CudaContext='primary')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
$cudaLaunch=Get-CudaTestArguments $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
$out="$BuildDir/bin/Release"
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close game/lab before fluid GPU validation.'}
$cases=@(
  @('empty',0,4,0,'ballistic'),@('one',1,4,0,'ballistic'),@('odd',257,4,0,'ballistic'),
  @('gravity',100000,4,0,'ballistic'),@('fall',100000,180,0,'ballistic'),
  @('constant',100000,1,1,'apic'),@('affine',100000,1,2,'apic'),@('compression',100000,1,3,'apic'),
  @('roundtrip',100000,1,4,'apic'),@('apic-empty',0,4,0,'apic'),@('apic-odd',257,180,0,'apic'),
  @('apic-fall',100000,180,0,'apic'),@('apic-settle',100000,600,0,'apic'),@('flip-fall',100000,180,0,'flip'),
  @('controls',257,12,0,'controls'),@('apic-long',100000,1200,0,'apic')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  if($CudaPressure -ne 'uniform' -and ($case[3] -or $case[4] -eq 'ballistic')){
    Write-Host "SKIP $($case[0]): partial fixture does not run a complete MGPCG substep";continue
  }
  $name="fluid-$($case[0])";$started=[DateTime]::UtcNow
  if($Backend -eq 'cuda'){$name="cuda-$name"}
  if($CudaGraphs){$name="cuda-graphs-"+$name.Substring(5)}
  if($CudaPressure -ne 'uniform'){$name="cuda-$CudaPressure-"+$name.Substring(5)}
  if($CudaPressureLoop -eq 'unrolled'){$name="unrolled-$name"}
  if($CudaContext -eq 'cig'){$name="cig-$name"}
  $extra=" $cudaLaunch"
  if($case[3]){$extra+=" --fluid-transfer-test=$($case[3])"}
  if($case[4] -eq 'ballistic'){$extra+=' --fluid-ballistic'}
  if($case[4] -eq 'flip'){$extra+=' --fluid-flip'}
  if($case[4] -eq 'controls'){$extra+=' --fluid-controls-test'}
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList "--fluid-validate --fluid-solver-only --fluid-backend=$Backend --fluid-particles=$($case[1]) --frames=$($case[2]) --name=$name$extra" -WorkingDirectory $out -PassThru
  $null=$p.Handle
  try {
    if(!$p.WaitForExit(60000)){$p.Kill();throw "Fluid test timeout: $name"}
    $p.Refresh()
    if($p.ExitCode -ne 0){Get-Content "$out/lab-error.txt" -ErrorAction SilentlyContinue;throw "Fluid failed: $name"}
    $path="$out/$name.json"
    if((Get-Item $path).LastWriteTimeUtc -lt $started){throw 'Stale fluid report'}
    $r=Get-Content $path -Raw|ConvertFrom-Json
    Assert-CudaTestMode $r $Backend $CudaGraphs $CudaPressure $CudaPressureLoop $CudaContext
    if(!$r.fluid.validated -or $r.fluid.active -ne $case[1] -or $r.frames -ne $case[2] -or $r.dlssEvaluations -ne $case[2]){throw 'Fluid validation/count/frame mismatch'}
    $steps=if($case[3]){0}else{2*$case[2]}
    if($case[4] -eq 'controls'){$steps=3}
    if($r.fluid.steps -ne $steps){throw 'Fixed fluid timestep count mismatch'}
    Write-Host "PASS $name | $($r.fluid.active) particles | $($r.fluid.steps) steps | $($r.fluid.lastSimulationMs) ms | analytic error $($r.fluid.maxGravityError)"
  }finally{$p.Dispose()}
}
