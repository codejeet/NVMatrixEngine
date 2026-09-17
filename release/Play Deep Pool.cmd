@echo off
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --fluid-deep-pool --normal-lens --quality=balanced --frame-gen=auto
if errorlevel 1 pause
popd
