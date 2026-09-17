@echo off
set "NVMATRIXENGINE_BULK_LAB_DIR=%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release"
if not exist "%NVMATRIXENGINE_BULK_LAB_DIR%\NVMatrixFluidLab.exe" (
  echo Run engine\setup.ps1 and engine\build.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_BULK_LAB_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine Passive Bulk Inventory Lab" "%NVMATRIXENGINE_BULK_LAB_DIR%\NVMatrixFluidLab.exe" --fluid-room --fluid-adaptive --fluid-bulk-view=1
popd
