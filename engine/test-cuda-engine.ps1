param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [ValidateSet('solver','material','surface','room','all')][string]$Suite = 'all',
  [string]$CaseFilter = '.*', [switch]$CudaGraphs,
  [ValidateSet('uniform','fine','mixed')][string]$CudaPressure='uniform',
  [ValidateSet('conditional','unrolled')][string]$CudaPressureLoop='conditional',
  [ValidateSet('primary','cig')][string]$CudaContext='primary'
)
$ErrorActionPreference = 'Stop'
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab first.' }
$runtime = (Resolve-Path "$BuildDir/bin/Release").Path
$exe = Join-Path $runtime 'NVMatrixFluidLab.exe'
$binary = Get-FileHash $exe
$sources = @(Get-ChildItem "$PSScriptRoot/src" -File -Recurse | Where-Object { $_.Extension -in '.h','.cpp','.cu','.cuh' })
$sources += @(Get-ChildItem "$PSScriptRoot/shaders" -File -Recurse)
$sources += @(Get-ChildItem "$PSScriptRoot/../shared/src" -File | Where-Object { $_.Extension -in '.h','.cpp' })
$sources += @(Get-ChildItem $PSScriptRoot -Filter 'test-*.ps1' -File)
$sources += Get-Item "$PSScriptRoot/cuda-test-mode.ps1"
$hashes = [ordered]@{}
foreach ($file in $sources) {
  if ($file.Extension -in '.h','.cpp','.cu','.cuh' -and $file.LastWriteTimeUtc -gt (Get-Item $exe).LastWriteTimeUtc) {
    throw "Rebuild the CUDA lab: executable predates $($file.FullName)"
  }
  $hashes[$file.FullName] = (Get-FileHash $file.FullName).Hash
}
$shaderHashes = [ordered]@{}
foreach ($file in Get-ChildItem "$runtime/shaders" -Filter '*.dxil') {
  $shaderHashes[$file.Name] = (Get-FileHash $file.FullName).Hash
}
$started = [DateTime]::UtcNow
foreach ($entry in @(@('solver','test-fluid.ps1'),@('material','test-fluid-material.ps1'),
                     @('surface','test-fluid-render.ps1'),@('room','test-room-water.ps1'))) {
  if ($Suite -ne 'all' -and $Suite -ne $entry[0]) { continue }
  & "$PSScriptRoot/$($entry[1])" -BuildDir $BuildDir -Backend cuda -CaseFilter $CaseFilter -CudaGraphs:$CudaGraphs -CudaPressure $CudaPressure -CudaPressureLoop $CudaPressureLoop -CudaContext $CudaContext
}
if ((Get-FileHash $exe).Hash -ne $binary.Hash) { throw 'CUDA executable changed during validation.' }
foreach ($file in $sources) {
  if ((Get-FileHash $file.FullName).Hash -ne $hashes[$file.FullName]) { throw 'CUDA source changed during validation.' }
}
foreach ($name in $shaderHashes.Keys) {
  if ((Get-FileHash "$runtime/shaders/$name").Hash -ne $shaderHashes[$name]) { throw 'Shader changed during validation.' }
}
$reports = @()
$expectedLoop=if($CudaPressure -eq 'uniform'){'none'}elseif($CudaGraphs){$CudaPressureLoop}else{'unrolled'}
foreach ($file in Get-ChildItem $runtime -Filter '*cuda-*.json' | Where-Object { $_.LastWriteTimeUtc -ge $started }) {
  $r = Get-Content $file.FullName -Raw | ConvertFrom-Json
  if ($r.fluid.backend -eq 'cuda' -and $r.fluid.validated -and $r.fluid.cuda.pressureMode -eq $CudaPressure -and
      $r.fluid.cuda.pressureLoop -eq $expectedLoop -and $r.fluid.cuda.contextMode -eq $CudaContext) {
    $reports += @{ file=$file.Name; sha256=(Get-FileHash $file.FullName).Hash;
                  frames=$r.frames; steps=$r.fluid.steps; particles=$r.fluid.active }
  }
}
if (!$reports.Count) { throw 'No fresh passing CUDA engine reports; check CaseFilter.' }
[ordered]@{
  version=4; suite=$Suite; caseFilter=$CaseFilter; cudaGraphs=[bool]$CudaGraphs; cudaPressure=$CudaPressure; cudaPressureLoop=$expectedLoop; cudaContext=$CudaContext; startedUTC=$started.ToString('o');
  completedUTC=[DateTime]::UtcNow.ToString('o'); executableSHA256=$binary.Hash;
  purpose='Single-phase engine correctness in the selected pressure mode; not a speedup or full multiscale acceptance result';
  sources=$hashes; shaders=$shaderHashes; reports=$reports
} | ConvertTo-Json -Depth 12 | Set-Content "$runtime/cuda-engine-$Suite-$CudaPressure-$expectedLoop-graphs-$([bool]$CudaGraphs)-$CudaContext-validation.json" -Encoding UTF8
Write-Host "CUDA engine evidence: $runtime/cuda-engine-$Suite-$CudaPressure-$expectedLoop-graphs-$([bool]$CudaGraphs)-$CudaContext-validation.json"
