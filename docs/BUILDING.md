# Building NVMatrixEngine

## Requirements

Windows x64; Visual Studio 2022 Build Tools with the C++ desktop workload and Windows SDK; CMake at `C:/Program Files/CMake/bin/cmake.exe`; Git; Windows PowerShell. Run commands from the repository root. A normal user terminal is sufficient. The optional CUDA build additionally requires CUDA Toolkit 13.1 at its standard location (or pass `-CudaToolkit`).

The renderer requires an NVIDIA RTX adapter and compatible current driver. The packaged preview was tested on RTX 5090 / driver 616.64. Optional SDK features are capability checked; source compilation alone does not certify a GPU.

## Fetch pinned dependencies

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/setup.ps1
```

The setup scripts fetch dependencies into ignored `shared/.deps`: Streamline 2.12.0, NVAPI, DX12 Agility 1.619.5, DXC 1.9.2607, RTXDI Library at `f12037fa`, PIX, Bullet 3.25, RmlUi 6.3, FreeType 2.14.1, miniaudio 0.11.25 and CIE observer data. See the scripts for complete pinned identifiers/checksums. Dependencies retain their licenses; the public repo does not vendor their SDK caches.

Model loading additionally uses Assimp 5.4.3 and its bundled stb_image, RapidJSON and zlib. Only the glTF and OBJ importers are enabled. See [model loading](../engine/MODEL_LOADING.md) for usage and portable importer tests.

## DX12-only build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build.ps1
& "$env:LOCALAPPDATA/NVMatrixEngine/build/bin/Release/NVMatrixFluidLab.exe"
```

## Optional CUDA build

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build-cuda.ps1
& "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build/bin/Release/NVMatrixFluidLab.exe" --fluid-room --normal-lens --fluid-backend=cuda --fluid-cuda-graphs=on --quality=balanced
```

The CUDA build still defaults to DX12 simulation unless the backend is requested. Default CUDA architecture targets are `86;89;120`. For a local 5090-only development build pass `-Architectures 120`; do not label such a build as a generally validated Ampere/Ada release.

Both scripts compile C++ and HLSL and copy runtime resources. CMake by itself does **not** compile all renderer shaders; after invoking CMake manually run `engine/compile-shaders.ps1 -OutputDir <runtime>/shaders`.

## Tests and captures

```powershell
& 'C:/Program Files/CMake/bin/ctest.exe' --test-dir "$env:LOCALAPPDATA/NVMatrixEngine/build" -C Release --output-on-failure
node --test engine/*.test.mjs
& "$env:LOCALAPPDATA/NVMatrixEngine/build/bin/Release/NVMatrixFluidLab.exe" --fluid-room --boat --normal-lens --frames=120 --capture --name=water-smoke
```

Node tests require a recent Node.js. The CUDA convenience build targets the main executable, not every test binary; build the relevant test targets before running CTest there. GPU fixtures are deliberately separate from hardware-independent tests. See `engine/test-*.ps1` for bounded hardware checks. Run GPU tests serially and close interactive demos first.

`--debug` requests the installed D3D12 debug layer; `--gpu-validation` additionally requests GPU validation. These optional development components were unavailable on the release validation machine; no debug-layer certification is claimed. The optional CUDA sanitizer fixture may require an administrator terminal. Build/run/release scripts do not request elevation or modify driver/TDR settings.

## Portable packaging

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/package-release.ps1 -BuildDir "$env:LOCALAPPDATA/NVMatrixEngineCUDA/build" -Version 0.1.2-preview -OutputDir "$env:USERPROFILE/Downloads"
```

Packaging uses an explicit runtime allowlist, checks NVIDIA/VC runtime signatures, includes dependency notices, writes per-file SHA-256 hashes, and refuses to overwrite an existing archive. It does not include source caches, PDBs, logs, developer captures, credentials or unrelated executables. By default it tests an extracted, relocated copy before producing the final archive. Release acceptance is documented in [VALIDATION.md](VALIDATION.md).

Add `-SkipValidation` to package the existing build without source/runtime consistency checks, vendor signature checks, extraction checks or test runs. This mode can copy the runtime while the demo remains open. It still requires a clean committed checkout and all packaging inputs, writes the manifest and archive checksum, and marks the verification sidecar `validationStatus: "skipped"` with an empty list of checks. Versions 0.1.2, 0.1.4 and 0.1.5 use this explicitly requested mode; their release notes disclose that no fresh package validation ran.

The first preview used the explicit `-AllowUnverifiedFrameGeneration` packaging exception described in the validation notes. Without it, packaging rejects an FG run that produces no extra presentations. Do not silently treat that exception as FG certification.

Neon Night is bundled with the source and copied into the runtime by CMake. After compiling shaders and building, run `Play Neon Night.cmd` or `NVMatrixFluidLab.exe --scene=neon-night`. [Scene setup and licensed assets](../engine/NEON_NIGHT.md).
