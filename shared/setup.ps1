# Download only pinned official upstream dependencies into this project.
param([switch]$SerExperiments)
$ErrorActionPreference = 'Stop'
$deps = Join-Path $PSScriptRoot '.deps'
New-Item -ItemType Directory -Force $deps | Out-Null
function Download-Checked($url, $file, $hash) {
  if (!(Test-Path $file)) { Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing }
  if ((Get-FileHash $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) { throw "Dependency checksum mismatch: $file" }
}
Download-Checked 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.12.0/streamline-sdk-v2.12.0.zip' "$deps/streamline-sdk-v2.12.0.zip" 'f5c0a3d870707dddc3570fb4bcd3655cf48a8a68c3a9d342910cfa21b77dcf48'
if (!(Test-Path "$deps/include/sl.h")) { Expand-Archive "$deps/streamline-sdk-v2.12.0.zip" -DestinationPath $deps -Force }
if ($SerExperiments) {
  Download-Checked 'https://codeload.github.com/NVIDIA/nvapi/tar.gz/87dca625e83fd89a983e19b904e5f3a580da90d2' "$deps/nvapi-87dca625.tar.gz" '39e9776330a32458c233d3dbc2caa1e6b12742dd949158a275981093a160260f'
  if (!(Test-Path "$deps/nvapi/nvapi.h")) {
    New-Item -ItemType Directory -Force "$deps/nvapi" | Out-Null
    tar -xzf "$deps/nvapi-87dca625.tar.gz" -C "$deps/nvapi" --strip-components=1
    if ($LASTEXITCODE) { throw 'NVAPI extraction failed.' }
  }
}
foreach ($item in @(
  @('mikke89/RmlUi', '6.3', 'rmlui-6.3', 'RmlUi-6.3', 'd977298bb6147610e5984d5db85ddf284020d655a8713913f6982074f1dbdede'),
  @('freetype/freetype', 'VER-2-14-1', 'freetype-2.14.1', 'freetype-VER-2-14-1', '44bd69d1f0750603410cfd4f26f1c5523c5a3a087a2dfa8639b757dc7c6677be'),
  @('mackron/miniaudio', '0.11.25', 'miniaudio-0.11.25', 'miniaudio-0.11.25', 'b900edcffe979816e2560a0580b9b1216d674b4f17fbadeca8f777a7f8ab0274')
)) {
  Download-Checked "https://codeload.github.com/$($item[0])/tar.gz/refs/tags/$($item[1])" "$deps/$($item[2]).tar.gz" $item[4]
  if (!(Test-Path "$deps/$($item[3])/CMakeLists.txt")) {
    tar -xzf "$deps/$($item[2]).tar.gz" -C $deps
    if ($LASTEXITCODE) { throw 'UI/audio dependency extraction failed.' }
  }
}
if (!(Test-Path "$deps/bullet/.git")) {
  git clone --depth 1 --branch 3.25 --filter=blob:none --sparse https://github.com/bulletphysics/bullet3.git "$deps/bullet"
  if ($LASTEXITCODE) { throw 'Bullet download failed.' }
}
git -C "$deps/bullet" sparse-checkout set src
if ($LASTEXITCODE) { throw 'Bullet source checkout failed.' }
$commit = git -C "$deps/bullet" rev-parse HEAD
if ($commit -ne '2c204c49e56ed15ec5fcfa71d199ab6d6570b3f5') { throw 'Unexpected Bullet revision.' }
Write-Host 'Streamline 2.12.0, Bullet 3.25, RmlUi 6.3, FreeType 2.14.1 and miniaudio 0.11.25 ready.'
