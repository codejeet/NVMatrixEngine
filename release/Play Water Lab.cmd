@echo off
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --fluid-room --normal-lens --quality=balanced --frame-gen=auto
if errorlevel 1 (
  echo The lab could not start. See lab-error.txt and NVMatrixEngine.log in this folder.
  pause
)
popd
