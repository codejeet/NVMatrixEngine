@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1
"%~dp0NVMatrixFluidLab.exe" --water-path=hamiltonian --normal-lens --boat --quality=balanced --frame-gen=auto %*
set "NVMATRIXENGINE_HAMILTONIAN_EXIT=%ERRORLEVEL%"
if not "%NVMATRIXENGINE_HAMILTONIAN_EXIT%"=="0" (
  echo The lab could not start. See lab-error.txt and NVMatrixEngine.log in this folder.
  pause
)
popd
exit /b %NVMATRIXENGINE_HAMILTONIAN_EXIT%
