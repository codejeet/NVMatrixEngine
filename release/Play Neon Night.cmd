@echo off
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --scene=neon-night --quality=quality --frame-gen=auto
if errorlevel 1 (
  echo Neon Night could not start. See lab-error.txt and NVMatrixEngine.log in this folder.
  pause
)
popd
