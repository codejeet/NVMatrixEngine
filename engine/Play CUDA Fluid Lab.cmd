@echo off
set "NVMATRIXENGINE_CUDA_LAB_DIR=%LOCALAPPDATA%\NVMatrixEngineCUDA\build\bin\Release"
if not exist "%NVMATRIXENGINE_CUDA_LAB_DIR%\NVMatrixFluidLab.exe" (
  echo Run engine\build-cuda.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_CUDA_LAB_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine CUDA Fluid Lab" "%NVMATRIXENGINE_CUDA_LAB_DIR%\NVMatrixFluidLab.exe" --fluid-room --fluid-backend=cuda --fluid-cuda-graphs=on --quality=balanced --frame-gen=off
popd
