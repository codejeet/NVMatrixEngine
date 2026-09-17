@echo off
set "NVMATRIXENGINE_NARROW_DIR=%LOCALAPPDATA%\NVMatrixEngineCUDA\build\bin\Release"
if not exist "%NVMATRIXENGINE_NARROW_DIR%\NVMatrixFluidLab.exe" (
  echo Run engine\build-cuda.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_NARROW_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine Narrow Band Deep Pool" "%NVMATRIXENGINE_NARROW_DIR%\NVMatrixFluidLab.exe" --fluid-deep-pool --fluid-narrow-band --fluid-cuda-pressure=uniform --quality=balanced --frame-gen=off
popd
