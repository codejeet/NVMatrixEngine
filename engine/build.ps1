param([string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngine/build")
$ErrorActionPreference = 'Stop'
$cmake = 'C:/Program Files/CMake/bin/cmake.exe'
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Windows x64 MSVC Build Tools are required.' }
& $cmake -S $PSScriptRoot -B $BuildDir -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs"
if ($LASTEXITCODE) { throw 'Lab CMake configuration failed.' }
& $cmake --build $BuildDir --config Release --parallel 4
if ($LASTEXITCODE) { throw 'Lab C++ build failed.' }
& "$PSScriptRoot/compile-shaders.ps1" -OutputDir "$BuildDir/bin/Release/shaders"
Write-Host "Experimental lab: $BuildDir/bin/Release/NVMatrixFluidLab.exe"
