param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngineCUDA/build")
$ErrorActionPreference='Stop'
if(Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue){throw 'Close the lab before CLI validation'}
$runtime=(Resolve-Path "$BuildDir/bin/Release").Path
$exe=Join-Path $runtime 'NVMatrixFluidLab.exe'
. "$PSScriptRoot/cuda-test-mode.ps1"
$template=@{fluid=@{backend='cuda';ownedParticles=$false;validated=$true;pressureAudit='published-fine-face-flux';
  maxPublishedFluxDivergence=.00001;maxPublishedFluxMismatch=.0000001;
  cuda=@{contextMode='primary';cigSharedMemoryBytes=0;pressureMode='mixed';mixedPressure=$true;graphBuilds=4;directSteps=0;graphReplays=2;
    stagingBytes=1024;mixedPressureBytes=1024;completedFrames=1;submissions=1;rejectedFrames=0;
    pressureCaps=0;pressurePeakIterations=10;pressureCgBudget=32;pressurePageChanges=8;
    pressureChangesPerFrame=64;pressureDivergence=.00001;pressureCoarsePeak=1;
    graphNodes=100;graphBodyNodes=20;pressureLoopSolves=2;pressureSolves=2;pressureLoopIterations=16;
    pressureIterations=16;pressureLoop='conditional';ownedParticles=$false;ownershipBuffers=0;sharedBuffers=14;
    refinementPolicy='advected-error-v1';refinementWakeCellSteps=1;refinementTemporalCellSteps=1;refinementPaddedCellSteps=1}}} | ConvertTo-Json -Depth 8
Assert-CudaTestMode ($template | ConvertFrom-Json) 'cuda' $true 'mixed'
foreach($edit in @(
  {param($r) $r.fluid.cuda.pressureDivergence=$null},
  {param($r) $r.fluid.cuda.pressureCaps=1},
  {param($r) $r.fluid.cuda.completedFrames=0},
  {param($r) $r.fluid.cuda.pressurePageChanges=65},
  {param($r) $r.fluid.cuda.pressureDivergence=[double]::NaN},
  {param($r) $r.fluid.pressureAudit='uniform'},
  {param($r) $r.fluid.maxPublishedFluxDivergence=.001},
  {param($r) $r.fluid.cuda.graphBodyNodes=0},
  {param($r) $r.fluid.cuda.pressureLoopSolves=0},
  {param($r) $r.fluid.cuda.pressureLoopIterations=17},
  {param($r) $r.fluid.cuda.pressureLoop='unrolled'},
  {param($r) $r.fluid.cuda.contextMode='cig'},
  {param($r) $r.fluid.cuda.cigSharedMemoryBytes=1024},
  {param($r) $r.fluid.cuda.ownedParticles=$true},
  {param($r) $r.fluid.cuda.ownershipBuffers=4},
  {param($r) $r.fluid.cuda.sharedBuffers=18},
  {param($r) $r.fluid.cuda.refinementPolicy='none'}
)){
  $r=$template|ConvertFrom-Json; & $edit $r
  $rejected=$false
  try{Assert-CudaTestMode $r 'cuda' $true 'mixed'}catch{$rejected=$true}
  if(!$rejected){throw 'Pressure evidence validator accepted a corrupt report'}
}
$cig=$template|ConvertFrom-Json
$cig.fluid.cuda.contextMode='cig'; $cig.fluid.cuda.cigSharedMemoryBytes=86016
Assert-CudaTestMode $cig 'cuda' $true 'mixed' 'conditional' 'cig'
$cig.fluid.cuda.cigSharedMemoryBytes=0
$rejected=$false
try{Assert-CudaTestMode $cig 'cuda' $true 'mixed' 'conditional' 'cig'}catch{$rejected=$true}
if(!$rejected){throw 'CIG context without a confirmed graphics limit was accepted'}
$owned=$template|ConvertFrom-Json
$owned.fluid.ownedParticles=$owned.fluid.cuda.ownedParticles=$true
$owned.fluid.cuda.sharedBuffers=18; $owned.fluid.cuda.ownershipBuffers=4
Assert-CudaTestMode $owned 'cuda' $true 'mixed'
$owned.fluid.cuda.stagingBytes=0
$rejected=$false
try{Assert-CudaTestMode $owned 'cuda' $true 'mixed'}catch{$rejected=$true}
if(!$rejected){throw 'Ownership without a guarded publication working set was accepted'}
Write-Host 'PASS CUDA report validation: three positive cases and 19 malformed reports'
$cases=@(
  @('pressure-name','--fluid-backend=cuda --fluid-cuda-pressure=unknown','CUDA pressure must be'),
  @('context-name','--fluid-backend=cuda --fluid-cuda-context=unknown','CUDA context must be'),
  @('context-backend','--fluid-backend=dx12 --fluid-cuda-context=cig','CUDA settings require'),
  @('loop-name','--fluid-backend=cuda --fluid-cuda-pressure=mixed --fluid-cuda-pressure-loop=unknown','CUDA pressure loop must be'),
  @('uniform-loop','--fluid-backend=cuda --fluid-cuda-pressure-loop=unrolled','loop selection requires'),
  @('wrong-backend','--fluid-backend=dx12 --fluid-cuda-pressure=mixed','CUDA settings require'),
  @('graphs-backend','--fluid-backend=dx12 --fluid-cuda-graphs=on','CUDA settings require'),
  @('uniform-budget','--fluid-backend=cuda --fluid-cuda-pressure-bricks=1','CUDA pressure budgets require'),
  @('zero-bricks','--fluid-backend=cuda --fluid-cuda-pressure-bricks=0','range'),
  @('cg-cap','--fluid-backend=cuda --fluid-cuda-cg-iterations=33','range'),
  @('dx12-mac','--fluid-backend=cuda --fluid-cuda-pressure=mixed --fluid-mac','no DX12-only solver modes'),
  @('partial','--fluid-backend=cuda --fluid-cuda-pressure=mixed --fluid-validate --fluid-ballistic','complete fluid substeps')
)
$results=@()
foreach($case in $cases){
  $started=[DateTime]::UtcNow
  $p=Start-Process $exe -WorkingDirectory $runtime -PassThru -ArgumentList "--frames=1 --frame-gen=off $($case[1])"
  $null=$p.Handle
  try{
    if(!$p.WaitForExit(60000)){$p.Kill();throw "CLI rejection timeout: $($case[0])"}
    $p.Refresh()
    $errorPath=Join-Path $runtime 'lab-error.txt'
    if(!$p.ExitCode -or (Get-Item $errorPath).LastWriteTimeUtc -lt $started -or
       (Get-Content $errorPath -Raw) -notmatch $case[2]){throw "Wrong CLI rejection: $($case[0])"}
    $results+=@{case=$case[0];pass=$true;error=(Get-Content $errorPath -Raw).Trim()}
    Write-Host "PASS CUDA CLI $($case[0])"
  }finally{$p.Dispose()}
}
@{version=1;executableSHA256=(Get-FileHash $exe).Hash;completedUTC=[DateTime]::UtcNow.ToString('o');cases=$results} |
  ConvertTo-Json -Depth 6 | Set-Content "$runtime/cuda-options-validation.json" -Encoding UTF8
