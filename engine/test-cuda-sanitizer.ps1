param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [string]$CudaToolkit = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1',
  [ValidateSet('memcheck','initcheck','synccheck')][string[]]$Tools = @('memcheck','initcheck','synccheck'),
  [ValidateSet('binary','hybrid')][string]$MemcheckMode = 'binary',
  [ValidateSet('Conditional','Mac','Solver','OwnedTransport','Exchange','JointTransfer','GeometricTransport')][string[]]$Fixtures = @('Conditional','Mac','Solver','OwnedTransport','Exchange','JointTransfer','GeometricTransport'),
  [switch]$ExcludeLoopControl,
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) { throw 'Close the game/lab first.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw 'This optional sanitizer fixture requires an administrator terminal. Normal builds and the demo do not require elevation.'
}
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
$sanitizer = Join-Path $CudaToolkit 'compute-sanitizer/compute-sanitizer.exe'
if (!(Test-Path $sanitizer)) { throw 'Compute Sanitizer executable not found.' }
if (!$NoBuild) {
  # Use the same toolkit for the compiler and its compile-time memcheck runtime.
  & $cmake -S $PSScriptRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64 `
    -T "cuda=$CudaToolkit" -DNVMATRIXENGINE_CUDA_FLUID=ON -DCMAKE_SUPPRESS_REGENERATION=ON -DCMAKE_CUDA_ARCHITECTURES=120
  if ($LASTEXITCODE) { throw 'CUDA sanitizer configuration failed (requires CUDA 13.1+).' }
  & $cmake --build $BuildDir --config Release --target NVMatrixEngineCudaConditionalTest NVMatrixEngineCudaMacTest NVMatrixEngineCudaSolverTest `
    NVMatrixEngineCudaConditionalMemcheckTest NVMatrixEngineCudaMacMemcheckTest NVMatrixEngineCudaSolverMemcheckTest `
    NVMatrixEngineCudaOwnedTransportTest NVMatrixEngineCudaExchangeTest NVMatrixEngineCudaJointTransferTest NVMatrixEngineCudaGeometricTransportTest NVMatrixEngineCudaGeometricTransportMemcheckTest --parallel 4
  if ($LASTEXITCODE) { throw 'CUDA sanitizer test build failed.' }
}
$runtime = (Resolve-Path "$BuildDir/bin/Release").Path
if ('Exchange' -in $Fixtures) {
  & "$PSScriptRoot/compile-cuda-tests.ps1" -OutputDir "$runtime/shaders"
  & "$PSScriptRoot/compile-fluid-exchange.ps1" -OutputDir "$runtime/shaders"
}
$compilerInfo = Get-ChildItem "$BuildDir/CMakeFiles" -Recurse -Filter CMakeCUDACompiler.cmake |
  Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if (!$compilerInfo) { throw 'Missing configured CUDA compiler metadata.' }
$compilerText = Get-Content $compilerInfo.FullName -Raw
if ($compilerText -notmatch 'set\(CMAKE_CUDA_COMPILER "([^"]+)"\)') { throw 'Cannot identify configured CUDA compiler.' }
if ((Resolve-Path $Matches[1]).Path -ne (Resolve-Path "$CudaToolkit/bin/nvcc.exe").Path) {
  throw 'Use the sanitizer from the toolkit that compiled these tests.'
}
$inputs = @(Get-ChildItem "$PSScriptRoot/src/fluid/cuda" -File)
$inputs += @(Get-Item "$PSScriptRoot/src/fluid/fluid_uniforms.h","$PSScriptRoot/src/fluid/fluid_colliders.h",`
  "$PSScriptRoot/tests/cuda_conditional_gpu.cu","$PSScriptRoot/tests/cuda_mac_gpu.cu",`
  "$PSScriptRoot/tests/cuda_solver_gpu.cu","$PSScriptRoot/CMakeLists.txt",$PSCommandPath)
$inputs += @(Get-Item "$PSScriptRoot/tests/cuda_owned_transport_gpu.cu","$PSScriptRoot/tests/cuda_joint_transfer_gpu.cu","$PSScriptRoot/tests/cuda_geometric_transport_gpu.cu","$PSScriptRoot/tests/cuda_owned_exchange.h",`
  "$PSScriptRoot/tests/fluid_exchange_gpu.cpp","$PSScriptRoot/src/fluid/fluid_particle_grid_exchange.h",`
  "$PSScriptRoot/src/fluid/fluid_particle_grid_exchange.cpp","$PSScriptRoot/src/gpu_resources.h",`
  "$PSScriptRoot/src/gpu_cuda_interop.h","$PSScriptRoot/src/gpu_cuda_interop.cpp",`
  "$PSScriptRoot/compile-cuda-tests.ps1","$PSScriptRoot/compile-fluid-exchange.ps1")
$inputs += @(Get-ChildItem "$PSScriptRoot/shaders/fluid" -File)
$hashes = [ordered]@{}
foreach ($file in $inputs) { $hashes[$file.FullName] = (Get-FileHash $file.FullName).Hash }
$shaderHashes = [ordered]@{}
if ('Exchange' -in $Fixtures) {
  foreach ($file in Get-ChildItem "$runtime/shaders" -Filter '*.dxil') { $shaderHashes[$file.Name] = (Get-FileHash $file.FullName).Hash }
}
$version = (& $sanitizer --version | Out-String).Trim()
if ($LASTEXITCODE) { throw 'Cannot query sanitizer version.' }
$results = @()
foreach ($tool in $Tools) {
  foreach ($fixture in $Fixtures) {
    $compileTime = $tool -eq 'memcheck' -and ($fixture -eq 'Conditional' -or ($fixture -in 'Mac','Solver','GeometricTransport' -and $MemcheckMode -eq 'hybrid'))
    $suffix = if ($compileTime) { 'MemcheckTest' } else { 'Test' }
    $exclusions = @()
    if ($tool -ne 'initcheck' -and $fixture -in 'Mac','Solver','GeometricTransport' -and $ExcludeLoopControl) {
      $exclusions = if($fixture -eq 'GeometricTransport'){@('boundedLoopCondition')}elseif($fixture -eq 'Solver'){@('loopCondition','boundedLoopCondition')}else{@('loopCondition')}
    }
    $exe = Join-Path $runtime "NVMatrixEngineCuda$fixture$suffix.exe"
    $binary = Get-FileHash $exe
    foreach ($file in $inputs | Where-Object { $_.Extension -in '.h','.cuh','.cu','.cpp' }) {
      # The standalone executable intentionally has no dependency on the solver.
      if ($fixture -eq 'Conditional' -and $file.Name -ne 'cuda_conditional_gpu.cu') { continue }
      if ($fixture -ne 'Conditional' -and $file.Name -eq 'cuda_conditional_gpu.cu') { continue }
      if ($file.LastWriteTimeUtc -gt (Get-Item $exe).LastWriteTimeUtc) { throw "Rebuild stale $exe ($($file.Name))." }
    }
    $name = "cuda-loops-$fixture-$tool"
    if ($exclusions.Count) { $name += '-partial' }
    $log = Join-Path $runtime "$name.log"
    $stdout = Join-Path $runtime "$name.stdout.log"
    $stderr = Join-Path $runtime "$name.stderr.log"
    $started = [DateTime]::UtcNow
    $args = "--tool $tool --error-exitcode 99 --log-file `"$log`" `"$exe`""
    if ($fixture -eq 'Exchange') { $args += " `"$runtime`"" }
    foreach($excludedKernel in $exclusions) { $args = "--kernel-name-exclude kns=$excludedKernel $args" }
    $p = Start-Process $sanitizer -ArgumentList $args -WorkingDirectory $runtime -PassThru `
      -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
      $null = $p.Handle # Retain the native handle before a short-lived child exits.
      if (!$p.WaitForExit(240000)) { $p.Kill(); throw "Sanitizer timed out: $name" }
      $p.Refresh()
      $output = Get-Content $stdout -Raw
      $diagnostics = Get-Content $log -Raw
      if ($null -eq $p.ExitCode -or $p.ExitCode -ne 0 -or $output -notmatch '(?m)^PASS CUDA ' -or
          $diagnostics -notmatch 'ERROR SUMMARY: 0 errors' -or
          $diagnostics -match '(?i)Target application returned an error|process didn.t terminate|Internal Sanitizer Error') {
        Get-Content $stdout,$stderr
        Get-Content $log -TotalCount 65
        throw "Sanitizer failed: $name (exit $($p.ExitCode)). Zero-error text alone is not success."
      }
      if ((Get-FileHash $exe).Hash -ne $binary.Hash) { throw 'Sanitizer test executable changed.' }
      if ($fixture -ne 'Conditional') {
        $cases = @($output -split '\r?\n' | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
        $expected = @{ Mac=35; Solver=39; OwnedTransport=12; Exchange=7; JointTransfer=12; GeometricTransport=15 }[$fixture]
        if ($cases.Count -ne $expected -or @($cases | Where-Object { !$_.pass }).Count) { throw "Incomplete $fixture case matrix." }
      }
      $results += [ordered]@{ fixture=$fixture; tool=$tool; executableSHA256=$binary.Hash;
        loopControlCompileTimeMemcheck=$compileTime;
        numericalKernelsBinaryInstrumented=($fixture -ne 'Conditional');
        kernelExclusions=$exclusions; startedUTC=$started.ToString('o');
        log=[IO.Path]::GetFileName($log); logSHA256=(Get-FileHash $log).Hash;
        stdout=[IO.Path]::GetFileName($stdout); stdoutSHA256=(Get-FileHash $stdout).Hash }
      if ($exclusions.Count) { Write-Warning "PARTIAL $name | $($exclusions -join ', ') NOT checked by $tool" }
      else { Write-Host "PASS $name | no kernel exclusions" }
    } finally { $p.Dispose() }
  }
}
foreach ($file in $inputs) {
  if ((Get-FileHash $file.FullName).Hash -ne $hashes[$file.FullName]) { throw "Source changed: $($file.FullName)" }
}
foreach ($name in $shaderHashes.Keys) {
  if ((Get-FileHash "$runtime/shaders/$name").Hash -ne $shaderHashes[$name]) { throw "Shader changed: $name" }
}
$partial = $ExcludeLoopControl -or @('memcheck','initcheck','synccheck' | Where-Object { $_ -notin $Tools }).Count -gt 0 -or
  @('Conditional','Mac','Solver','OwnedTransport','Exchange','JointTransfer','GeometricTransport' | Where-Object { $_ -notin $Fixtures }).Count -gt 0
$reportName = if ($partial) { 'cuda-sanitizer-partial-validation.json' } else { 'cuda-sanitizer-validation.json' }
# Preserve distinct focused evidence instead of overwriting a different subset.
if (@('Conditional','Mac','Solver','OwnedTransport','Exchange','JointTransfer','GeometricTransport' | Where-Object { $_ -notin $Fixtures }).Count) {
  $reportName = 'cuda-sanitizer-' + ($Fixtures -join '-') + '-validation.json'
}
[ordered]@{ version=2; completedUTC=[DateTime]::UtcNow.ToString('o'); sanitizer=$version; partialCoverage=[bool]$partial;
  fixtures=$Fixtures; sanitizerSHA256=(Get-FileHash $sanitizer).Hash; sources=$hashes; shaders=$shaderHashes; results=$results;
  scope='Selected CUDA graph, pressure, composed solver, grid-owned transport, joint transfer/transport sequence and shared exchange fixtures. Not DX12 GPU validation, live joint flow or performance evidence.'
} | ConvertTo-Json -Depth 10 | Set-Content "$runtime/$reportName" -Encoding UTF8
Write-Host "CUDA sanitizer evidence: $runtime/$reportName"
