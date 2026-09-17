param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [string]$CudaToolkit = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1',
  [string]$Architectures = '120',
  [ValidateSet('primary','cig')][string]$Context='primary',
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) {
  throw 'Close the lab before running the CUDA GPU fixture.'
}
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
if (!$NoBuild) {
  # Local toolset selection also works when the toolkit's VS integration has
  # not been installed globally. Do not modify Visual Studio or system policy.
  & $cmake -S $PSScriptRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64 `
    -T "cuda=$CudaToolkit" -DNVMATRIXENGINE_CUDA_FLUID=ON -DCMAKE_SUPPRESS_REGENERATION=ON "-DCMAKE_CUDA_ARCHITECTURES=$Architectures"
  if ($LASTEXITCODE) { throw 'CUDA CMake configuration failed.' }
  & $cmake --build $BuildDir --config Release --target NVMatrixEngineCudaInteropTest --parallel 4
  if ($LASTEXITCODE) { throw 'CUDA interop build failed.' }
}
$runtime = (Resolve-Path "$BuildDir/bin/Release").Path
$exe = Join-Path $runtime 'NVMatrixEngineCudaInteropTest.exe'
$log = Join-Path $runtime "cuda-interop-$Context.log"
$err = Join-Path $runtime "cuda-interop-$Context.stderr.log"
$args = "`"$runtime`"" + $(if($Context -eq 'cig'){' --cig'}else{''})
$test = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $runtime `
  -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
$null = $test.Handle
if (!$test.WaitForExit(60000)) {
  Stop-Process -Id $test.Id -Force
  throw "CUDA fixture timed out; stopped only test PID $($test.Id). See $log and $err."
}
$test.Refresh()
Get-Content $log
if ((Get-Item $err).Length) { Get-Content $err }
if ($null -eq $test.ExitCode -or $test.ExitCode -ne 0) { throw "CUDA interop fixture failed ($($test.ExitCode))." }
if (!(Select-String -Path $log -Pattern '^PASS CUDA interop:' -Quiet)) {
  throw 'CUDA fixture returned without its completion marker.'
}
$cases=@(Get-Content $log | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { $_.case })
$expected=if($Context -eq 'cig'){9}else{8}
if($cases.Count -ne $expected -or @($cases|Where-Object {!$_.pass}).Count){throw 'Incomplete interop case matrix'}
foreach($case in $cases|Where-Object {$_.case -in 'round-trip','empty-handoff'}){
  if($case.contextMode -ne $Context -or ($Context -eq 'cig' -and !$case.cigSharedMemoryBytes)){throw 'Incorrect CUDA context used'}
}
@{version=1; context=$Context; completedUTC=[DateTime]::UtcNow.ToString('o'); executableSHA256=(Get-FileHash $exe).Hash;
  logSHA256=(Get-FileHash $log).Hash; cases=$cases} | ConvertTo-Json -Depth 8 |
  Set-Content "$runtime/cuda-interop-$Context-validation.json" -Encoding UTF8
Write-Host 'CUDA interop validated. This fixture does not validate a liquid solver or game performance.'
