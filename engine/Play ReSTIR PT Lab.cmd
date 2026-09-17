@echo off
set "NVMATRIXENGINE_PT_LAB_DIR=%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release"
if not exist "%NVMATRIXENGINE_PT_LAB_DIR%\NVMatrixFluidLab.exe" (
  echo Run engine\setup.ps1 and engine\build.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_PT_LAB_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine ReSTIR PT Fluid Lab" "%NVMATRIXENGINE_PT_LAB_DIR%\NVMatrixFluidLab.exe" --fluid --restir-pt
popd
