param([string]$BuildDir="$env:LOCALAPPDATA/NVMatrixEngine/build",[string]$CaseFilter='.*',[switch]$Reference,
      [ValidateSet('dx12','cuda')][string]$Backend='dx12',[switch]$CudaGraphs,
      [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
      [ValidateSet('primary','cig')][string]$CudaContext='primary')
$ErrorActionPreference='Stop'
. "$PSScriptRoot/cuda-test-mode.ps1"
$out="$BuildDir/bin/Release"
$exe=Get-Item "$out/NVMatrixFluidLab.exe"
$binary=(Get-FileHash $exe.FullName).Hash
$sources=@(Get-ChildItem "$PSScriptRoot/src" -File -Recurse | Where-Object {$_.Extension -in '.h','.cpp','.cu','.cuh'})
$sources+=@(Get-ChildItem "$PSScriptRoot/../shared/src" -File | Where-Object {$_.Extension -in '.h','.cpp'})
$sources+=@(Get-ChildItem "$PSScriptRoot/shaders" -File -Recurse)
$sources+=Get-Item $PSCommandPath,"$PSScriptRoot/cuda-test-mode.ps1"
$hashes=[ordered]@{}
foreach($file in $sources){
  if($file.Extension -in '.h','.cpp','.cu','.cuh' -and $file.LastWriteTimeUtc -gt $exe.LastWriteTimeUtc){
    throw "Rebuild the lab before ownership validation: executable predates $($file.FullName)"
  }
  $hashes[$file.FullName]=(Get-FileHash $file.FullName).Hash
}
$shaders=[ordered]@{}
foreach($file in Get-ChildItem "$out/shaders" -Filter '*.dxil'){$shaders[$file.Name]=(Get-FileHash $file.FullName).Hash}
$results=@();$suiteStarted=[DateTime]::UtcNow
$cases=@(
  @('empty',4,'--fluid-room --fluid-particles=0'),
  @('short',8,'--fluid-room'),
  @('affine',1,'--fluid-pit --fluid-particles=4096 --fluid-transfer-test=4'),
  @('flip',32,'--fluid-room --fluid-flip'),
  @('inlet',64,'--fluid-room --fluid-emitter'),
  @('mixed',64,'--fluid-room --fluid-emitter --fluid-cut-pressure --fluid-mac-validate --fluid-cut-validate --fluid-sparse-work --fluid-work-validate --adaptive-rays --restir-pt'),
  @('reset',180,'--fluid-room-test')
)
foreach($case in $cases){
  if($case[0] -notmatch $CaseFilter){continue}
  $pressure=if($Backend -eq 'cuda' -and $case[0] -eq 'mixed'){'mixed'}else{$CudaPressure}
  if($Backend -eq 'cuda' -and $pressure -ne 'uniform' -and $case[0] -eq 'affine'){
    Write-Host 'SKIP affine partial-step fixture: CUDA MGPCG requires full substeps';continue
  }
  if(Get-Process NVMatrixFluidLab*,NVMatrixEngine,NVMatrixEngineExchangeGpuTest -ErrorAction SilentlyContinue){throw 'Close GPU workloads before live authority tests'}
  $prefix=if($Reference){'owned-reference'}else{'owned-particles'}
  $mode=if($Reference){''}else{'--fluid-owned-particles'}
  $name="$prefix-$($case[0])";$started=[DateTime]::UtcNow
  $scenario=$case[2]
  if($Backend -eq 'cuda'){
    $name="cuda-$pressure-$CudaContext-$([bool]$CudaGraphs)-$name"
    if($case[0] -eq 'mixed'){$scenario='--fluid-room --fluid-emitter --adaptive-rays --restir-pt'}
  }
  $cudaLaunch=Get-CudaTestArguments $Backend $CudaGraphs $pressure 'conditional' $CudaContext
  $launchArguments="$mode --fluid-backend=$Backend $cudaLaunch --fluid-validate --fluid-deterministic-bins --atomics=fixed --no-whitewater --frame-gen=off --width=960 --height=540 --quality=balanced --frames=$($case[1]) --capture --name=$name $scenario"
  $p=Start-Process "$out/NVMatrixFluidLab.exe" -ArgumentList $launchArguments -WorkingDirectory $out -PassThru
  $null=$p.Handle
  try{
    if(!$p.WaitForExit(180000)){$p.Kill();$null=$p.WaitForExit(5000);throw "Owned live authority test timed out: $name"}
    $p.Refresh();if($null -eq $p.ExitCode -or $p.ExitCode -ne 0){Get-Content "$out/lab-error.txt";throw "Live authority test failed: $name"}
    $path="$out/$name.json";if((Get-Item $path).LastWriteTimeUtc -lt $started){throw 'Stale authority report'}
    $r=Get-Content $path -Raw|ConvertFrom-Json;$f=$r.fluid;$a=$f.particleAuthority
    Assert-CudaTestMode $r $Backend $CudaGraphs $pressure 'conditional' $CudaContext
    if(!$f.validated -or $f.ownedParticles -eq [bool]$Reference){throw 'Live ownership mode or fluid audit mismatch'}
    if(!$Reference -and (!$a.validated -or $a.inventoryBits -ne 64 -or $a.invalid -or $a.relativeVolumeError -gt 1e-11 -or $a.massCacheRelativeError -gt 2e-7 -or $a.velocityCacheRelativeError -gt 3e-7)){throw 'Live authoritative quantity audit failed'}
    if($r.frames -ne $case[1] -or $f.droppedSeconds){throw 'Incomplete simulation test'}
    if($case[0] -ne 'affine' -and ($r.dlssEvaluations -ne $r.frames -or $r.photonCounters[5] -or $r.photonCounters[6] -or $r.fluidProbes.badRoots -or $r.fluidProbes.truncated)){throw 'Live authority optical regression'}
    if($Backend -eq 'dx12' -and $case[0] -eq 'mixed' -and (!$f.adaptiveMac.validated -or !$f.cutCells.validated -or !$f.work.validated -or $f.work.sameStateFaceMaxDifference -or $f.work.sameStateDensityMaxDifference)){throw 'Mixed/sparse authoritative operator regression'}
    if($case[0] -in @('inlet','mixed') -and !$f.emittedParticles){throw 'Inlet did not exercise new particle authority'}
    if($case[0] -eq 'empty' -and $a.volumeM3){throw 'Empty authority invented water'}
    $results+=@{case=$case[0];file="$name.json";sha256=(Get-FileHash $path).Hash;frames=$r.frames;
      steps=$f.steps;pressure=$pressure;particles=$f.active;emitted=$f.emittedParticles;pass=$true}
    Write-Host "PASS $name | $($r.frames) frames | emitted $($f.emittedParticles) | mass error $($a.relativeVolumeError) | velocity cache error $($a.velocityCacheRelativeError)"
  }finally{$p.Dispose()}
}
if(!$results.Count){throw 'No passing ownership cases; check CaseFilter'}
if((Get-FileHash $exe.FullName).Hash -ne $binary){throw 'Ownership executable changed during validation'}
foreach($file in $sources){if((Get-FileHash $file.FullName).Hash -ne $hashes[$file.FullName]){throw "Ownership source changed: $($file.FullName)"}}
foreach($name in $shaders.Keys){if((Get-FileHash "$out/shaders/$name").Hash -ne $shaders[$name]){throw "Ownership shader changed: $name"}}
$label=if($Reference){'reference'}else{'owned'}
$manifest="$out/$Backend-$CudaPressure-$CudaContext-graphs-$([bool]$CudaGraphs)-$label-validation.json"
[ordered]@{version=1;scope='Live all-particle-owned solver, source/reset lifecycle and rendered optical audits; not grid-owned transport or adaptive surface acceptance';
  backend=$Backend;ownedParticles=![bool]$Reference;caseFilter=$CaseFilter;cudaPressure=$CudaPressure;cudaContext=$CudaContext;cudaGraphs=[bool]$CudaGraphs;
  startedUTC=$suiteStarted.ToString('o');completedUTC=[DateTime]::UtcNow.ToString('o');executableSHA256=$binary;
  sources=$hashes;shaders=$shaders;cases=$results} | ConvertTo-Json -Depth 12 | Set-Content $manifest -Encoding UTF8
Write-Host "Ownership evidence: $manifest"
