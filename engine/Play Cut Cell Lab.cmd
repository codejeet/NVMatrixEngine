@echo off
set "NVMATRIXENGINE_CUT_LAB_DIR=%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release"
if not exist "%NVMATRIXENGINE_CUT_LAB_DIR%\NVMatrixFluidLab.exe" (
  echo Run engine\setup.ps1 and engine\build.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_CUT_LAB_DIR%"
if errorlevel 1 exit /b 1
start "NVMatrixEngine Solid Cut Cell Lab" "%NVMATRIXENGINE_CUT_LAB_DIR%\NVMatrixFluidLab.exe" --fluid-room --fluid-cut-cells --fluid-bulk --quality=balanced
popd
