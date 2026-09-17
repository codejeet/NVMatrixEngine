param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build",
  [string]$CudaToolkit = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1',
  [string]$Architectures = '86;89;120'
)
$ErrorActionPreference = 'Stop'
if (Get-Process NVMatrixFluidLab,NVMatrixEngine -ErrorAction SilentlyContinue) {
  throw 'Close the game/lab before rebuilding CUDA.'
}
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
& $cmake -S $PSScriptRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64 `
  -T "cuda=$CudaToolkit" -DNVMATRIXENGINE_CUDA_FLUID=ON -DCMAKE_SUPPRESS_REGENERATION=ON "-DCMAKE_CUDA_ARCHITECTURES=$Architectures"
if ($LASTEXITCODE) { throw 'CUDA lab configuration failed.' }
& $cmake --build $BuildDir --config Release --target NVMatrixFluidLab --parallel 4
if ($LASTEXITCODE) { throw 'CUDA lab build failed.' }
& "$PSScriptRoot/compile-shaders.ps1" -OutputDir "$BuildDir/bin/Release/shaders"
Write-Host "CUDA lab (opt-in): $BuildDir/bin/Release/NVMatrixFluidLab.exe --fluid-room --fluid-backend=cuda --fluid-cuda-graphs=on"
