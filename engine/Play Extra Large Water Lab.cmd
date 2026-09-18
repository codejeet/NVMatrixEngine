@echo off
setlocal
set "NVMATRIXENGINE_OCEAN_DIR=%LOCALAPPDATA%\NVMatrixEngineHgi\build\bin\Release"
if not exist "%NVMATRIXENGINE_OCEAN_DIR%\NVMatrixFluidLab.exe" (
  set "NVMATRIXENGINE_OCEAN_DIR=%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release"
)
if not exist "%NVMATRIXENGINE_OCEAN_DIR%\NVMatrixFluidLab.exe" (
  echo No Extra Large Water Lab build was found.
  echo Run engine\setup.ps1 and engine\build.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_OCEAN_DIR%"
if errorlevel 1 exit /b 1
"%NVMATRIXENGINE_OCEAN_DIR%\NVMatrixFluidLab.exe" --water-lab=extra-large --normal-lens --boat --quality=balanced --frame-gen=auto %*
set "NVMATRIXENGINE_OCEAN_EXIT=%ERRORLEVEL%"
if not "%NVMATRIXENGINE_OCEAN_EXIT%"=="0" (
  echo The lab could not start. See lab-error.txt and NVMatrixEngine.log in %NVMATRIXENGINE_OCEAN_DIR%.
  pause
)
popd
exit /b %NVMATRIXENGINE_OCEAN_EXIT%
