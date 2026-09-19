@echo off
setlocal
set "NVMATRIXENGINE_NEON_DIR="
for %%D in ("%~dp0" "%~dp0.." "%LOCALAPPDATA%\NVMatrixEngineHgi\build\bin\Release" "%LOCALAPPDATA%\NVMatrixEngine\build\bin\Release" "%LOCALAPPDATA%\NVMatrixModelTest\build\bin\Release") do (
  if not defined NVMATRIXENGINE_NEON_DIR if exist "%%~D\NVMatrixFluidLab.exe" if exist "%%~D\assets\neon-night\neon-night.gltf" if exist "%%~D\shaders\BloomVertical.dxil" set "NVMATRIXENGINE_NEON_DIR=%%~D"
)
if not defined NVMATRIXENGINE_NEON_DIR (
  echo No Neon Night build was found.
  echo Run engine\setup.ps1 and engine\build.ps1 in Windows PowerShell first.
  pause
  exit /b 1
)
pushd "%NVMATRIXENGINE_NEON_DIR%"
if errorlevel 1 exit /b 1
"%NVMATRIXENGINE_NEON_DIR%\NVMatrixFluidLab.exe" --scene=neon-night --quality=quality --frame-gen=auto %*
set "NVMATRIXENGINE_NEON_EXIT=%ERRORLEVEL%"
if not "%NVMATRIXENGINE_NEON_EXIT%"=="0" (
  echo Neon Night could not start. See lab-error.txt and NVMatrixEngine.log in "%NVMATRIXENGINE_NEON_DIR%".
  pause
)
popd
exit /b %NVMATRIXENGINE_NEON_EXIT%
