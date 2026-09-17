@echo off
setlocal
set "NVMATRIXENGINE_FLUID_LAB_DIR=%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release"
if not exist "%NVMATRIXENGINE_FLUID_LAB_DIR%\NVMatrixFluidLab.exe" (
  set "NVMATRIXENGINE_FLUID_LAB_DIR=%LOCALAPPDATA%\NVMatrixEngineCUDA\build\bin\Release"
)
if not exist "%NVMATRIXENGINE_FLUID_LAB_DIR%\NVMatrixFluidLab.exe" (
  echo No Fluid Lab build was found in NVMatrixEngine or NVMatrixEngineCUDA.
  echo Run engine\setup.ps1, then engine\build.ps1 or engine\build-cuda.ps1 in Windows PowerShell.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_FLUID_LAB_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine GPU Fluid Lab" "%NVMATRIXENGINE_FLUID_LAB_DIR%\NVMatrixFluidLab.exe" --fluid %*
set "NVMATRIXENGINE_FLUID_LAB_EXIT=%ERRORLEVEL%"
popd
exit /b %NVMATRIXENGINE_FLUID_LAB_EXIT%
