@echo off
setlocal
set "NVMATRIXENGINE_HAMILTONIAN_DIR=%LOCALAPPDATA%\NVMatrixEngineHgi\build\bin\Release"
if not exist "%NVMATRIXENGINE_HAMILTONIAN_DIR%\NVMatrixFluidLab.exe" (
  set "NVMATRIXENGINE_HAMILTONIAN_DIR=%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release"
)
if not exist "%NVMATRIXENGINE_HAMILTONIAN_DIR%\NVMatrixFluidLab.exe" (
  echo No Hamiltonian Water Lab build was found.
  echo Run engine\setup.ps1 and engine\build.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_HAMILTONIAN_DIR%"
if errorlevel 1 exit /b 1
"%NVMATRIXENGINE_HAMILTONIAN_DIR%\NVMatrixFluidLab.exe" --water-path=hamiltonian --normal-lens --boat --quality=balanced --frame-gen=auto %*
set "NVMATRIXENGINE_HAMILTONIAN_EXIT=%ERRORLEVEL%"
if not "%NVMATRIXENGINE_HAMILTONIAN_EXIT%"=="0" (
  echo The lab could not start. See lab-error.txt and NVMatrixEngine.log in %NVMATRIXENGINE_HAMILTONIAN_DIR%.
  pause
)
popd
exit /b %NVMATRIXENGINE_HAMILTONIAN_EXIT%
