@echo off
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --fluid-deep-pool --normal-lens --fluid-narrow-band --fluid-cuda-pressure=uniform --quality=balanced --frame-gen=off
if errorlevel 1 pause
popd
