@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --water-lab=extra-large --normal-lens --boat --quality=balanced --frame-gen=auto %*
set "NVMATRIXENGINE_OCEAN_EXIT=%ERRORLEVEL%"
if not "%NVMATRIXENGINE_OCEAN_EXIT%"=="0" (
  echo The lab could not start. See lab-error.txt and NVMatrixEngine.log in this folder.
  pause
)
popd
exit /b %NVMATRIXENGINE_OCEAN_EXIT%
