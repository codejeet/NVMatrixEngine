# Shared, ignored dependency cache; production native build settings are not changed.
$ErrorActionPreference = 'Stop'
$deps = Join-Path $PSScriptRoot '../shared/.deps'
$shared = @('nvapi/nvapi.h','bullet/src/btBulletDynamicsCommon.h','RmlUi-6.3/CMakeLists.txt','freetype-VER-2-14-1/CMakeLists.txt','miniaudio-0.11.25/miniaudio.h')
if (@($shared | Where-Object { !(Test-Path "$deps/$_") }).Count) { & "$PSScriptRoot/../shared/setup.ps1" -SerExperiments }
function Get-Pinned($url, $file, $hash) {
  if (!(Test-Path $file)) { Invoke-WebRequest $url -OutFile $file -UseBasicParsing }
  if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) { throw "Checksum mismatch: $file" }
}
Get-Pinned 'https://codeload.github.com/assimp/assimp/tar.gz/refs/tags/v5.4.3' "$deps/assimp-5.4.3.tar.gz" '66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb'
if (!(Test-Path "$deps/assimp-5.4.3/CMakeLists.txt")) {
  tar -xzf "$deps/assimp-5.4.3.tar.gz" -C $deps
  if ($LASTEXITCODE) { throw 'Assimp extraction failed.' }
}
Get-Pinned 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.619.5/microsoft.direct3d.d3d12.1.619.5.nupkg' "$deps/agility-1.619.5.zip" '0e9bcf32aac9a79343ede9b21e4864950ee54577e3d8e19bfcdf002bb4e9bfd6'
Get-Pinned 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2607/dxc_2026_07_29.zip' "$deps/dxc-1.9.2607.zip" 'a1dfb116ba3eeae6a1582291b53a8e7bf65ad760676bd3194685c8f7367cd241'
Get-Pinned 'https://codeload.github.com/NVIDIA-RTX/RTXDI-Library/tar.gz/f12037fa8e97ebc08e9e3edfd2de528ed1772a4b' "$deps/rtxdi-f12037fa.tar.gz" '4f38eed1afafb9632c2d4bee6ab68d4a472b2e1b7fffe9757f92a199bea9f36d'
Get-Pinned 'https://files.cie.co.at/Publications-datasets/CIE_xyz_1931_2deg.csv' "$deps/CIE_xyz_1931_2deg.csv" 'fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1'
Get-Pinned 'https://api.nuget.org/v3-flatcontainer/winpixeventruntime/1.0.240308001/winpixeventruntime.1.0.240308001.nupkg' "$deps/pix-1.0.240308001.zip" '726acc93d6968e2146261a1e415521747d50ad69894c2b42b5d0d4c29fd66ec4'
foreach ($name in @('agility-1.619.5', 'dxc-1.9.2607','pix-1.0.240308001')) {
  if (!(Test-Path "$deps/$name")) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory([IO.Path]::GetFullPath("$deps/$name.zip"),[IO.Path]::GetFullPath("$deps/$name"))
  }
}
if (!(Test-Path "$deps/rtxdi-f12037fa/Include")) {
  New-Item -ItemType Directory -Force "$deps/rtxdi-f12037fa" | Out-Null
  tar -xzf "$deps/rtxdi-f12037fa.tar.gz" -C "$deps/rtxdi-f12037fa" --strip-components=1
  if ($LASTEXITCODE) { throw 'RTXDI library extraction failed.' }
}
Write-Host 'Pinned Agility 1.619.5, DXC 1.9.2607, RTXDI library and CIE data ready.'
