param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [string]$CudaToolkit = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1',
  [string]$Architectures = '120',
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) {
  throw 'Close the lab before running CUDA GPU tests.'
}
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
if (!$NoBuild) {
  # Configure explicitly each run: MSBuild lowercases dependency names on WSL
  # paths, so its automatic regeneration rule is not reliable on this filesystem.
  & $cmake -S $PSScriptRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64 `
    -T "cuda=$CudaToolkit" -DNVMATRIXENGINE_CUDA_FLUID=ON -DCMAKE_SUPPRESS_REGENERATION=ON "-DCMAKE_CUDA_ARCHITECTURES=$Architectures"
  if ($LASTEXITCODE) { throw 'CUDA configuration failed.' }
  & $cmake --build $BuildDir --config Release --target NVMatrixEngineCudaInteropTest NVMatrixEngineCudaTransferTest NVMatrixEngineCudaCoreTest NVMatrixEngineCudaBrickTest NVMatrixEngineCudaMacTest NVMatrixEngineCudaSolverTest NVMatrixEngineCudaSurfaceTest NVMatrixEngineCudaOwnedTransportTest NVMatrixEngineCudaExchangeTest NVMatrixEngineCudaJointTransferTest NVMatrixEngineCudaGeometricTransportTest --parallel 4
  if ($LASTEXITCODE) { throw 'CUDA build failed.' }
}
$runtime = (Resolve-Path "$BuildDir/bin/Release").Path
# Recompile the HLSL reference even when only an include changed and MSBuild
# considers the executable up-to-date. Never compare against a stale shader.
& "$PSScriptRoot/compile-cuda-tests.ps1" -OutputDir "$runtime/shaders"
& "$PSScriptRoot/compile-fluid-exchange.ps1" -OutputDir "$runtime/shaders"
$inputs = @('CMakeLists.txt','src/gpu_resources.h','src/gpu_cuda_interop.h','src/gpu_cuda_interop.cpp',
  'src/fluid/fluid_uniforms.h','src/fluid/fluid_colliders.h','src/fluid/cuda/fluid_cuda_kernels.h','src/fluid/cuda/fluid_cuda_transfer.h',
  'src/fluid/cuda/fluid_cuda_transfer.cu','tests/cuda_interop_gpu.cpp','tests/cuda_interop_kernel.cu',
  'tests/cuda_transfer_gpu.cpp','tests/cuda_core_gpu.cpp','tests/cuda_bricks_gpu.cu','tests/cuda_mac_gpu.cu','tests/cuda_solver_gpu.cu',
  'tests/cuda_fluid_fixture.h','tests/cuda_owned_transport_gpu.cu','tests/cuda_joint_transfer_gpu.cu','tests/cuda_geometric_transport_gpu.cu','tests/cuda_owned_exchange.h','tests/fluid_exchange_gpu.cpp',
  'tests/cuda_surface_gpu.cpp','src/fluid/fluid_cuda.h','src/fluid/fluid_cuda.cpp',
  'src/fluid/fluid_surface.h','src/fluid/fluid_surface.cpp','src/fluid/fluid_surface_lod_validation.cpp','src/fluid/fluid_system.h','../shared/src/camera.h',
  'tests/cuda_surface_rays.h','tests/cuda_surface_planes.h','src/scene.h','shaders/common.hlsli','shaders/optical-state.hlsli','shaders/interface-media.hlsli',
  'src/fluid/fluid_particle_grid_exchange.h','src/fluid/fluid_particle_grid_exchange.cpp','test-cuda.ps1','compile-cuda-tests.ps1','compile-fluid-exchange.ps1')
$cudaSources=@(Get-ChildItem "$PSScriptRoot/src/fluid/cuda" -File | ForEach-Object { 'src/fluid/cuda/' + $_.Name })
$inputs += $cudaSources
$inputs += @(Get-ChildItem "$PSScriptRoot/shaders/fluid" -File | ForEach-Object { 'shaders/fluid/' + $_.Name })
$hashes = [ordered]@{}
foreach ($source in $inputs) { $hashes[$source] = (Get-FileHash (Join-Path $PSScriptRoot $source)).Hash }
$results = @()
foreach ($fixture in @(
  @{ Name='NVMatrixEngineCudaInteropTest'; Prefix='cuda-interop'; Marker='^PASS CUDA interop:'; Cases=8;
    Sources=@('tests/cuda_interop_gpu.cpp','tests/cuda_interop_kernel.cu') },
  @{ Name='NVMatrixEngineCudaTransferTest'; Prefix='cuda-transfer'; Marker='^PASS CUDA transfers:'; Cases=8;
    Sources=@('tests/cuda_transfer_gpu.cpp','tests/cuda_fluid_fixture.h','src/fluid/fluid_uniforms.h')+$cudaSources },
  @{ Name='NVMatrixEngineCudaCoreTest'; Prefix='cuda-core'; Marker='^PASS CUDA core kernels:'; Cases=19;
    Sources=@('tests/cuda_core_gpu.cpp','tests/cuda_fluid_fixture.h','src/fluid/fluid_uniforms.h','src/fluid/fluid_colliders.h')+$cudaSources },
  @{ Name='NVMatrixEngineCudaBrickTest'; Prefix='cuda-bricks'; Marker='^PASS CUDA brick pool:'; Cases=5;
    Sources=@('tests/cuda_bricks_gpu.cu')+$cudaSources },
  @{ Name='NVMatrixEngineCudaMacTest'; Prefix='cuda-mac'; Marker='^PASS CUDA mixed MAC:'; Cases=38;
    Sources=@('tests/cuda_mac_gpu.cu')+$cudaSources },
  @{ Name='NVMatrixEngineCudaSolverTest'; Prefix='cuda-solver'; Marker='^PASS CUDA composed MGPCG:'; Cases=39;
    Sources=@('tests/cuda_solver_gpu.cu')+$cudaSources },
  @{ Name='NVMatrixEngineCudaSurfaceTest'; Prefix='cuda-surface'; Marker='^PASS CUDA surface geometry:'; Cases=7;
    Sources=@('tests/cuda_surface_gpu.cpp','tests/cuda_fluid_fixture.h','src/fluid/fluid_cuda.h','src/fluid/fluid_cuda.cpp',
              'src/fluid/fluid_surface.h','src/fluid/fluid_surface.cpp','src/fluid/fluid_surface_lod_validation.cpp','src/fluid/fluid_system.h','../shared/src/camera.h',
              'tests/cuda_surface_rays.h','tests/cuda_surface_planes.h','src/scene.h')+$cudaSources },
  @{ Name='NVMatrixEngineCudaOwnedTransportTest'; Prefix='cuda-owned-transport'; Marker='^PASS CUDA owned transport:'; Cases=12;
    Sources=@('tests/cuda_owned_transport_gpu.cu')+$cudaSources },
  @{ Name='NVMatrixEngineCudaJointTransferTest'; Prefix='cuda-joint-transfer'; Marker='^PASS CUDA joint transfers:'; Cases=12;
    Sources=@('tests/cuda_joint_transfer_gpu.cu')+$cudaSources },
  @{ Name='NVMatrixEngineCudaGeometricTransportTest'; Prefix='cuda-geometric-transport'; Marker='^PASS CUDA geometric transport:'; Cases=15;
    Sources=@('tests/cuda_geometric_transport_gpu.cu')+$cudaSources },
  @{ Name='NVMatrixEngineCudaExchangeTest'; Prefix='cuda-exchange'; Marker='^PASS CUDA exchange:'; Cases=7;
    Sources=@('tests/fluid_exchange_gpu.cpp','tests/cuda_owned_exchange.h','src/fluid/fluid_particle_grid_exchange.h',
              'src/fluid/fluid_particle_grid_exchange.cpp')+$cudaSources }
)) {
  $log = Join-Path $runtime "$($fixture.Prefix).log"
  $err = Join-Path $runtime "$($fixture.Prefix).stderr.log"
  $exe = Join-Path $runtime "$($fixture.Name).exe"
  $compiledSources = $fixture.Sources
  if($fixture.Name -in 'NVMatrixEngineCudaInteropTest','NVMatrixEngineCudaTransferTest','NVMatrixEngineCudaCoreTest','NVMatrixEngineCudaExchangeTest','NVMatrixEngineCudaSurfaceTest') {
    $compiledSources += @('src/gpu_resources.h','src/gpu_cuda_interop.h','src/gpu_cuda_interop.cpp')
  }
  foreach ($source in $compiledSources) {
    if ((Get-Item (Join-Path $PSScriptRoot $source)).LastWriteTimeUtc -gt (Get-Item $exe).LastWriteTimeUtc) {
      throw "$exe is older than $source. Rebuild before recording evidence."
    }
  }
  $start = [DateTime]::UtcNow
  $process = Start-Process -FilePath $exe -ArgumentList "`"$runtime`"" -WorkingDirectory $runtime `
    -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
  $null=$process.Handle
  if (!$process.WaitForExit(60000)) {
    Stop-Process -Id $process.Id -Force
    throw "Stopped timed-out CUDA fixture PID $($process.Id); logs: $log, $err"
  }
  $process.Refresh()
  $lines = @(Get-Content $log)
  $lines | Write-Output
  if ((Get-Item $err).Length) { Get-Content $err }
  if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0 -or !($lines | Select-String -Pattern $fixture.Marker)) {
    throw "$($fixture.Name) failed ($($process.ExitCode))."
  }
  $data = @($lines | Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
  $cases = @($data | Where-Object { $_.case })
  if ($cases.Count -ne $fixture.Cases -or @($cases | Where-Object { !$_.pass }).Count) {
    throw "$($fixture.Name) did not produce the expected passing case matrix."
  }
  $results += @{ test=$fixture.Name; startedUTC=$start.ToString('o'); executableSHA256=(Get-FileHash $exe).Hash; results=$data }
}
foreach ($source in $inputs) {
  if ($hashes[$source] -ne (Get-FileHash (Join-Path $PSScriptRoot $source)).Hash) {
    throw "Source changed during CUDA validation: $source"
  }
}
$shaderHashes = [ordered]@{}
foreach ($shader in Get-ChildItem "$runtime/shaders" -Filter '*.dxil') { $shaderHashes[$shader.Name] = (Get-FileHash $shader.FullName).Hash }
$report = [ordered]@{ version=12; completedUTC=[DateTime]::UtcNow.ToString('o');
  phaseMovingPlaneFixture=$true; phaseAnalyticDomainClipping=$true;
  phaseDxrIntersections=$true; phaseDielectricFixtures=$true;
  canonicalPhaseField=$true; phaseProceduralBlasBuild=$true; phaseOpticalAcceptance=$false;
  scope='GPU interop, APIC/FLIP, mixed-MGPCG, brick pool, coupled MAC, conservative grid transport, shared DX12 exchange and transactional joint Solver fixtures; not live joint particle/grid rendering';
  composedSubsteps=$true; rendererExercised=$false; multiscale=$false; mixedPressureFixture=$true; multigridFixture=$true; qualifiedFineFallbackFixture=$true;
  mixedPressureComposedSubsteps=$true; transactionalPublication=$true;
  gridOwnedTransportFixture=$true; sharedGridExchangeFixture=$true; liveJointParticleGridFlow=$false;
  jointPredictionFixture=$true; jointTransportSequenceFixture=$true;
  jointSolverLifecycleFixture=$true; jointSolverLatePressureRollbackFixture=$true;
  geometricPhaseTransportFixture=$true; jointGeometricSupportFixture=$true;
  fullPoolCapacityAdmissionFixture=$true; deviceControlledCapacityLoops=$true;
  transactionalSurfaceGeometry=$true; surfaceGeometryDx12Consumer=$true;
  coupledInterfaceCapacityFlux=$false; gridOwnedSurfaceRendering=$false;
  sources=$hashes; shaders=$shaderHashes; tests=$results }
$report | ConvertTo-Json -Depth 12 | Set-Content "$runtime/cuda-validation.json" -Encoding UTF8
Write-Host "CUDA evidence: $runtime/cuda-validation.json"
