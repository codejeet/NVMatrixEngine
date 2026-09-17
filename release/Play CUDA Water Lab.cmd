@echo off
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --fluid-room --normal-lens --fluid-backend=cuda --fluid-cuda-graphs=on --quality=balanced --frame-gen=auto
if errorlevel 1 (
  echo CUDA is optional and requires a compatible NVIDIA driver. Try Play Water Lab.cmd.
  pause
)
popd
