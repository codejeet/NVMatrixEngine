param(
  [string]$BuildDir = "$env:LOCALAPPDATA/NVMatrixEngineHgi/build",
  [string]$OutputDir = "$env:USERPROFILE/Downloads",
  [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9.-]{0,63}$')][string]$Version = '0.1.4-preview',
  [string]$CudaToolkit = 'C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.1',
  [switch]$AllowUnverifiedFrameGeneration,
  [switch]$SkipValidation
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$SkipValidation -and (Get-Process NVMatrixFluidLab -ErrorAction SilentlyContinue)) { throw 'Close the demo before packaging.' }
$repo = [IO.Path]::GetFullPath((Resolve-Path "$PSScriptRoot/..").ProviderPath)
$runtime = (Resolve-Path "$BuildDir/bin/Release").Path
$deps = (Resolve-Path "$repo/shared/.deps").ProviderPath
$gitRepo = $repo.Replace('\','/')
# Trust only this explicitly selected checkout for this command, not every WSL
# path and not a persistent/global Git configuration change.
$commit = (& git -c "safe.directory=$gitRepo" -C $gitRepo rev-parse HEAD | Out-String).Trim()
if ($LASTEXITCODE -or $commit -notmatch '^[a-f0-9]{40}$') { throw 'Commit the source before packaging.' }
$dirty = & git -c "safe.directory=$gitRepo" -C $gitRepo status --porcelain
if ($LASTEXITCODE -or $dirty) { throw 'Package only a clean committed source tree.' }
$cache = Get-Content "$BuildDir/CMakeCache.txt" -Raw
$cudaEnabled = $cache -match 'NVMATRIXENGINE_CUDA_FLUID:BOOL=ON'
$cudaArchitectures = @()
if ($cudaEnabled) { $cudaArchitectures = @(86,89,120) }
foreach ($arch in $cudaArchitectures) {
  if ($cache -notmatch "CMAKE_CUDA_ARCHITECTURES:[^=]+=[^\r\n]*\b$arch\b") { throw "Release CUDA architecture missing: $arch" }
}
$archiveName = "NVMatrixEngine-$Version-win64.zip"
$archive = Join-Path ([IO.Path]::GetFullPath($OutputDir)) $archiveName
foreach ($target in @($archive,"$archive.sha256","$archive.verification.json")) {
  if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite release artifact: $target" }
}
$nvidia = @('sl.interposer.dll','sl.common.dll','sl.dlss_d.dll','sl.dlss.dll',
  'nvngx_dlss.dll','nvngx_dlssd.dll','sl.dlss_g.dll','nvngx_dlssg.dll','sl.reflex.dll','sl.pcl.dll')
$payload = @('NVMatrixFluidLab.exe','WinPixEventRuntime.dll','D3D12/D3D12Core.dll',
  'assets/CIE_xyz_1931_2deg.csv','assets/ocean/day.hdr','assets/ocean/night.hdr','assets/ocean/README.md',
  'ui/lab.rml','ui/lab.rcss','ui/Poppins-Regular.ttf','shaders/ui.hlsl') + $nvidia
$neonFiles = @(Get-ChildItem "$PSScriptRoot/assets/neon-night" -Recurse -File | ForEach-Object {
  'assets/neon-night/' + $_.FullName.Substring((Get-Item "$PSScriptRoot/assets/neon-night").FullName.Length + 1).Replace('\','/')
})
$payload += $neonFiles
$payload += @(Get-ChildItem "$runtime/shaders" -Filter '*.dxil' -File | ForEach-Object { "shaders/$($_.Name)" })
if (@($payload | Where-Object { $_ -like '*.dxil' }).Count -lt 30) { throw 'Compile the renderer shaders first.' }
foreach ($cue in @('tick','rotate','dock','grab','throw','jump','unlock','click','victory')) { $payload += "assets/audio/$cue.wav" }
foreach ($track in 0..4) { $payload += "assets/audio/music-$track.mp3" }
foreach ($relative in $payload) {
  if (!(Test-Path "$runtime/$relative" -PathType Leaf) -or (Get-Item "$runtime/$relative").Length -eq 0) {
    throw "Missing runtime input: $relative"
  }
}
if (!$SkipValidation) {
  foreach ($relative in (@('ui/lab.rml','ui/lab.rcss','assets/ocean/day.hdr','assets/ocean/night.hdr','assets/ocean/README.md') + $neonFiles)) {
    if ((Get-FileHash "$PSScriptRoot/$relative").Hash -ne (Get-FileHash "$runtime/$relative").Hash) { throw "Stale runtime asset: $relative" }
  }
  if ((Get-FileHash "$runtime/assets/CIE_xyz_1931_2deg.csv").Hash.ToLowerInvariant() -ne 'fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1') { throw 'CIE dataset differs from its licensed source.' }
}

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'App-local Microsoft C++ redist files are required on the packaging machine.' }
$redist = (Get-Content "$vs/VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt" -Raw).Trim()
$crt = "$vs/VC/Redist/MSVC/$redist/x64/Microsoft.VC143.CRT"
$vc = @('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll',
  'msvcp140_codecvt_ids.dll','vccorlib140.dll','vcruntime140.dll','vcruntime140_1.dll','vcruntime140_threads.dll')
if (!$SkipValidation) {
  foreach ($file in @($nvidia | ForEach-Object { "$runtime/$_" }) + @($vc | ForEach-Object { "$crt/$_" })) {
    if ((Get-AuthenticodeSignature -LiteralPath $file).Status -ne 'Valid') { throw "Vendor signature invalid: $file" }
  }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ('NVMatrix-release-' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $work 'staging/NVMatrixEngine'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
function Copy-Payload([string]$Source,[string]$Relative) {
  if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing package input: $Relative" }
  $to = Join-Path $stage $Relative
  New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($to)) -Force | Out-Null
  Copy-Item -LiteralPath $Source -Destination $to
}
foreach ($relative in $payload) { Copy-Payload "$runtime/$relative" $relative }
foreach ($file in $vc) { Copy-Payload "$crt/$file" $file }
foreach ($file in @('Streamline-LICENSE.txt','NVAPI-LICENSE.txt','RTXDI-LICENSE.txt','Bullet-LICENSE.txt',
  'RmlUi-LICENSE.txt','FreeType-LICENSE.txt','miniaudio-LICENSE.txt','Font-LICENSE.txt','PIX-LICENSE.txt','PIX-ThirdPartyNotices.txt',
  'Assimp-LICENSE.txt','RapidJSON-LICENSE.txt','zlib-LICENSE.txt','stb-image-LICENSE.txt')) {
  Copy-Payload "$runtime/$file" "licenses/$file"
}
Copy-Payload "$deps/external/ngx-sdk/license.txt" 'licenses/NVIDIA-RTX-SDK-LICENSE.txt'
Copy-Payload "$deps/bin/x64/nvngx_dlss.license.txt" 'licenses/nvngx_dlss.license.txt'
Copy-Payload "$deps/bin/x64/reflex.license.txt" 'licenses/reflex.license.txt'
Copy-Payload "$deps/agility-1.619.5/LICENSE.txt" 'licenses/Agility-LICENSE.txt'
Copy-Payload "$deps/agility-1.619.5/LICENSE-CODE.txt" 'licenses/Agility-LICENSE-CODE.txt'
if ($cudaEnabled) { Copy-Payload "$CudaToolkit/EULA.txt" 'licenses/CUDA-EULA.txt' }
Copy-Payload "$vs/Licenses/1033/Redist.txt" 'licenses/Microsoft-VC-REDIST.txt'
foreach ($file in @('README.md','COPYRIGHT.md','THIRD_PARTY_NOTICES.md','CITATION.cff')) { Copy-Payload "$repo/$file" $file }
foreach ($file in Get-ChildItem "$repo/release" -File | Where-Object { $_.Extension -in '.cmd','.txt' }) {
  if (!$cudaEnabled -and $file.Name -in @('Play CUDA Water Lab.cmd','Play Deep Pool.cmd','Play Narrow Band Deep Pool.cmd')) { continue }
  Copy-Payload $file.FullName $file.Name
}
foreach ($file in @('README.md','HAMILTONIAN_WATER.md','OCEAN_LAB.md','MODEL_LOADING.md','NEON_NIGHT.md','NEON_PERFORMANCE.md','RTXPT_COMPARISON.md','RENDER_SAMPLING.md','neon-performance.json','rtxpt-performance.json','sampling-validation.json','profile-neon.ps1')) { Copy-Payload "$PSScriptRoot/$file" "engine/$file" }
Copy-Payload "$PSScriptRoot/Play Neon Night.cmd" 'engine/Play Neon Night.cmd'
Copy-Payload "$PSScriptRoot/assets/ocean/README.md" 'engine/assets/ocean/README.md'
Copy-Payload "$PSScriptRoot/assets/neon-night/README.md" 'engine/assets/neon-night/README.md'
Copy-Payload "$PSScriptRoot/assets/neon-night/assets.json" 'engine/assets/neon-night/assets.json'
Copy-Payload "$PSScriptRoot/neon-night-validation.json" 'engine/neon-night-validation.json'
Copy-Payload "$repo/release/NOTES.md" 'release/NOTES.md'
foreach ($file in Get-ChildItem "$repo/docs" -Recurse -File | Where-Object { $_.Extension -in '.md','.png','.gif' }) {
  $relative = $file.FullName.Substring($repo.Length + 1).Replace('\','/')
  Copy-Payload $file.FullName $relative
}
$files = @(Get-ChildItem $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
  [ordered]@{ path=$_.FullName.Substring($stage.Length + 1).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash $_.FullName).Hash.ToLowerInvariant() }
})
[ordered]@{ product='NVMatrixEngine'; version=$Version; sourceCommit=$commit; cudaEnabled=$cudaEnabled; cudaArchitectures=$cudaArchitectures;
  createdUtc=[DateTime]::UtcNow.ToString('o'); files=$files } | ConvertTo-Json -Depth 6 |
  Set-Content "$stage/release-manifest.json" -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$candidate = Join-Path $work $archiveName
# Create entries explicitly: ZipFile on .NET Framework can emit backslashes,
# which are not portable ZIP path separators.
$zip = [IO.Compression.ZipFile]::Open($candidate,[IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($file in Get-ChildItem $stage -Recurse -File) {
    $relative = 'NVMatrixEngine/' + $file.FullName.Substring($stage.Length + 1).Replace('\','/')
    $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$file.FullName,$relative,[IO.Compression.CompressionLevel]::Optimal)
  }
} finally { $zip.Dispose() }
if ($SkipValidation) {
  # Package the existing build without extracting it or launching test processes.
  # Hashes identify the payload; they do not certify its runtime behavior.
  $hash = (Get-FileHash $candidate).Hash.ToLowerInvariant()
  "$hash  $archiveName" | Set-Content "$candidate.sha256" -Encoding ASCII
  [ordered]@{ archive=$archiveName; sourceCommit=$commit; sha256=$hash; zipBytes=(Get-Item $candidate).Length;
    payloadFiles=$files.Count+1; validationStatus='skipped'; validationSkipped=$true; verifiedUtc=$null;
    windowsVersion=[Environment]::OSVersion.Version.ToString(); checks=@();
    sinkingBallVerified=$null; generatedPresentations=$null; frameGenerationOutputVerified=$null;
    appLocalModules=@(); limitations='Packaged the existing build with -SkipValidation. No fresh source/runtime consistency, vendor signature, ZIP round-trip or runtime checks were performed.' } |
    ConvertTo-Json -Depth 6 | Set-Content "$candidate.verification.json" -Encoding UTF8
  New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($archive)) | Out-Null
  foreach ($suffix in @('.sha256','.verification.json','')) {
    if (Test-Path "$archive$suffix") { throw 'Destination appeared during packaging; refusing overwrite.' }
    Copy-Item "$candidate$suffix" "$archive$suffix"
  }
  Write-Host "READY (validation skipped): $archive"
  Write-Host "SHA256: $hash"
  Write-Host "Packaging workspace: $work"
  return
}
$extracted = Join-Path $work 'relocated playtest with spaces'
[IO.Compression.ZipFile]::ExtractToDirectory($candidate,$extracted)
$testRoot = Join-Path $extracted 'NVMatrixEngine'
foreach ($file in $files) {
  if ((Get-FileHash "$testRoot/$($file.path)").Hash.ToLowerInvariant() -ne $file.sha256) { throw "ZIP round-trip mismatch: $($file.path)" }
}
if (@(Get-ChildItem $testRoot -Recurse -File).Count -ne $files.Count + 1) { throw 'Unexpected extracted payload.' }
Write-Host "Verified $($files.Count) extracted files. Running bounded portable tests..."
$checks = @()
$observed = @{}
$requiredModules = @('sl.interposer.dll','sl.common.dll','sl.dlss_d.dll','nvngx_dlssd.dll',
  'sl.dlss_g.dll','nvngx_dlssg.dll','msvcp140.dll','vcruntime140.dll')
function Run-Bounded([string]$Name,[string]$Arguments,[int]$Frames,[switch]$ObserveModules,[switch]$Ocean) {
  $started = [DateTime]::UtcNow
  $p = Start-Process "$testRoot/NVMatrixFluidLab.exe" -ArgumentList $Arguments -WorkingDirectory $work -PassThru
  $null = $p.Handle
  $clock = [Diagnostics.Stopwatch]::StartNew()
  try {
    while (!$p.HasExited) {
      if ($clock.Elapsed.TotalSeconds -gt 120) { throw "Portable test timeout: $Name" }
      if ($ObserveModules) {
        try { $modules = @((Get-Process -Id $p.Id -ErrorAction Stop).Modules) } catch { $modules = @() }
        foreach ($module in $modules) {
          if ($module.ModuleName -in $requiredModules) {
            if (![string]::Equals($module.FileName,(Join-Path $testRoot $module.ModuleName),[StringComparison]::OrdinalIgnoreCase)) { throw "Non-local vendor module: $($module.ModuleName)" }
            $observed[$module.ModuleName] = $true
          }
        }
      }
      Start-Sleep -Milliseconds 100
      $p.Refresh()
    }
    $p.WaitForExit(); $p.Refresh()
    if ($p.ExitCode -ne 0) {
      if (Test-Path "$testRoot/lab-error.txt") { Get-Content "$testRoot/lab-error.txt" }
      throw "Portable test failed: $Name ($($p.ExitCode))"
    }
  } finally {
    if (!$p.HasExited) { $p.Kill(); $p.WaitForExit() }
    $p.Dispose()
  }
  if (!$Frames) { return $null }
  if ((Get-Item "$testRoot/$Name.json").LastWriteTimeUtc -lt $started) { throw "Stale test report: $Name" }
  $r = Get-Content "$testRoot/$Name.json" -Raw | ConvertFrom-Json
  if ($r.frames -ne $Frames -or $r.dlssEvaluations -ne $Frames -or !$r.fluidSurface.surfaceBricks -or !$r.waterPhotonEntries) { throw "Incomplete fluid rendering: $Name" }
  if ((Get-Item "$testRoot/$Name.game.json").LastWriteTimeUtc -lt $started) { throw "Stale gameplay report: $Name" }
  $g = Get-Content "$testRoot/$Name.game.json" -Raw | ConvertFrom-Json
  if (!$Ocean -and ($g.ballFloats -ne $false -or [Math]::Abs($g.ballDensityKgM3 - 2500) -gt .01 -or $g.ballMassKg -lt 3000)) {
    throw "Packaged water avatar did not default to solid sinking glass: $Name"
  }
  Copy-Item "$testRoot/NVMatrixEngine.log" "$work/$Name.log"
  if (Select-String -Path "$work/$Name.log" -Pattern 'SL ERROR:|LAB ERROR:') { throw "Runtime error in $Name log" }
  return $r
}
$null = Run-Bounded 'cpu' '--self-test' 0
$checks += 'Native gameplay/audio self-test'
$common = '--fluid-room --boat --normal-lens --width=1280 --height=720 --quality=balanced'
$room = Run-Bounded 'release-water' "$common --fluid-emitter --fluid-validate --frames=120 --frame-gen=off --name=release-water" 120
if (!$room.fluid.validated -or $room.fluidProbes.badRoots -or $room.fluidProbes.truncated) { throw 'Water geometry validation failed.' }
$checks += 'DX12 room/inlet/boat/whitewater + fluid optical validation (120 frames)'
$tour = Run-Bounded 'release-tour' "$common --demo-tour --fluid-depth=.55 --fluid-particles=200000 --fluid-emitter --frames=240 --frame-gen=off --name=release-tour" 240
$checks += 'Sinking-ball roll/jump recording tour (240 frames)'
$underwater = Run-Bounded 'release-underwater' "$common --underwater-view --fluid-depth=1.4 --fluid-particles=400000 --frames=120 --frame-gen=off --name=release-underwater" 120
$checks += 'Underwater first-person room rendering (120 frames)'
$fg = Run-Bounded 'release-fg' "$common --frames=120 --frame-gen=2 --name=release-fg" 120 -ObserveModules
if (!$fg.frameGeneration.enabled -or $fg.frameGeneration.status -or !$fg.frameGeneration.reflex) { throw 'Invalid frame-generation / Reflex runtime state.' }
$fgVerified = $fg.frameGeneration.extraPresents -ge 20
if (!$fgVerified) {
  if (!$AllowUnverifiedFrameGeneration) { throw 'Frame generation did not produce valid extra presentations.' }
  Write-Warning 'FG output is UNVERIFIED: enabled without errors, but insufficient extra presentations. Disclose this in the release notes.'
}
foreach ($module in $requiredModules) { if (!$observed.ContainsKey($module)) { throw "Did not observe packaged module: $module" } }
$checks += 'DLSS FG / Reflex initialization and app-local vendor DLL loading'
if ($fgVerified) { $checks += 'Actual DLSS 2x generated presentations' }
$narrowEvidence = $null
if ($cudaEnabled) {
$cuda = Run-Bounded 'release-cuda' "$common --fluid-backend=cuda --fluid-cuda-graphs=on --frames=96 --frame-gen=off --name=release-cuda" 96
if ($cuda.fluid.backend -ne 'cuda' -or !$cuda.fluid.cuda.graphReplays -or $cuda.fluid.cuda.rejectedFrames) { throw 'CUDA graph smoke test failed.' }
$checks += 'CUDA uniform graph-replay water (96 frames)'
$narrow = Run-Bounded 'release-narrow' '--fluid-deep-pool --boat --normal-lens --fluid-narrow-band --fluid-validate --width=1280 --height=720 --quality=balanced --frames=96 --frame-gen=off --name=release-narrow' 96
if (!$narrow.fluid.cuda.narrowBand -or !$narrow.fluid.narrowBandOwnership.gridVolumeM3 -or $narrow.fluid.active -ge $narrow.fluid.particles -or $narrow.fluid.particleAuthority.relativeVolumeError -gt 1e-11 -or $narrow.fluidProbes.badRoots -or $narrow.fluidProbes.truncated) { throw 'Live narrow-band smoke/conservation failed.' }
$checks += 'Deep-pool CUDA live particle retirement, conservation and optical validation (96 frames)'
$narrowEvidence = [ordered]@{ activeParticles=$narrow.fluid.active; gridVolumeM3=$narrow.fluid.narrowBandOwnership.gridVolumeM3; relativeVolumeError=$narrow.fluid.particleAuthority.relativeVolumeError }
}
foreach ($scene in @('large','ocean')) {
  $name = "release-$scene"
  $r = Run-Bounded $name "--water-lab=$scene --boat --normal-lens --width=1280 --height=720 --quality=balanced --frames=180 --frame-gen=off --name=$name" 180 -Ocean:($scene -eq 'ocean')
  if ($r.waterPath -ne 'hamiltonian' -or $r.hamiltonian.nonfinite -or $r.hamiltonian.boundaryOverflow -or !$r.hamiltonian.boundarySeeded) { throw "Invalid packaged Hamiltonian water: $scene" }
  if ($scene -eq 'ocean' -and (!$r.oceanLab -or $r.fluid.initialDepth -ne 6 -or $r.fluidSurface.allocatedBytes -gt 50000000)) { throw 'Invalid packaged fixed-grid ocean.' }
  $checks += "Hamiltonian $scene water (180 frames)"
}
$pt = Run-Bounded 'release-restir' "$common --restir-pt --frames=96 --frame-gen=off --name=release-restir" 96
if (!$pt.restirPT.enabled -or !$pt.restirPT.pixelsLastFrame) { throw 'ReSTIR PT was not active in its smoke test.' }
$checks += 'Optional ReSTIR PT room render (96 frames)'
$hash = (Get-FileHash $candidate).Hash.ToLowerInvariant()
"$hash  $archiveName" | Set-Content "$candidate.sha256" -Encoding ASCII
[ordered]@{ archive=$archiveName; sourceCommit=$commit; sha256=$hash; zipBytes=(Get-Item $candidate).Length;
  payloadFiles=$files.Count+1; validationStatus='passed'; validationSkipped=$false; verifiedUtc=[DateTime]::UtcNow.ToString('o'); adapter=$room.adapter;
  windowsVersion=[Environment]::OSVersion.Version.ToString(); checks=@('ZIP round-trip SHA-256')+$checks;
  sinkingBallVerified=$true; ballDensityKgM3=2500;
  generatedPresentations=$fg.frameGeneration.extraPresents; frameGenerationOutputVerified=$fgVerified;
  knownIssues=@($(if (!$fgVerified) {'FG enabled with status 0 but no verified extra presentations; also reproduced with the predecessor executable on this PC.'}));
  appLocalModules=@($observed.Keys | Sort-Object);
  cudaEnabled=$cudaEnabled; narrowBand=$narrowEvidence;
  limitations='Same RTX 5090 development PC, relocated extraction. Not clean-OS, cross-GPU, performance or D3D12 debug-layer certification.' } |
  ConvertTo-Json -Depth 6 | Set-Content "$candidate.verification.json" -Encoding UTF8
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($archive)) | Out-Null
foreach ($suffix in @('.sha256','.verification.json','')) {
  if (Test-Path "$archive$suffix") { throw 'Destination appeared during tests; refusing overwrite.' }
  Copy-Item "$candidate$suffix" "$archive$suffix"
}
Write-Host "READY: $archive"
Write-Host "SHA256: $hash"
Write-Host "Evidence retained outside ZIP: $work"
