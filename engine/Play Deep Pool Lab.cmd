@echo off
set "NVMATRIXENGINE_DEEP_POOL_DIR=%LOCALAPPDATA%\NVMatrixEngineCUDA\build\bin\Release"
if not exist "%NVMATRIXENGINE_DEEP_POOL_DIR%\NVMatrixFluidLab.exe" (
  echo Run engine\build-cuda.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_DEEP_POOL_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine Deep Pool" "%NVMATRIXENGINE_DEEP_POOL_DIR%\NVMatrixFluidLab.exe" --fluid-deep-pool --fluid-backend=cuda --fluid-cuda-graphs=on --quality=balanced --frame-gen=off
popd
