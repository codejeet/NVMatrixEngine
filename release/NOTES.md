# NVMatrixEngine 0.1.2 — Fisheye DLSS and lighting improvements

NVMatrixEngine is a C++20 / DirectX 12 game engine with GPU liquid simulation, path-traced lighting and spectral caustics. Water Lab is the playable demo: a pool with a wall inlet, sinking glass ball, powered boat and underwater views.

## Changes in 0.1.2

- **Fisheye frame generation:** both normal and fisheye cameras support DLSS FG. A cached bidirectional lens-distortion map supplies the fisheye projection to DLSS while preserving the separate HUD composition.
- **Separate FPS counters:** the HUD shows Render FPS and DLSS output FPS using actual presented-frame counts, plus RR/FG status.
- **DLSS presets in Esc:** switch Ray Reconstruction upscaling between Quality, Balanced and Performance without resetting the water. The settings display the actual input/output resolution.
- **Cheaper diffuse lighting:** contribution-weighted Lambertian visibility sampling reduces shadow-ray work with probability compensation. Water shadow intersections can stop once a surface crossing is established; camera and photon intersections retain refined roots. See the [implementation and earlier measurements](https://github.com/codejeet/NVMatrixEngine/blob/v0.1.2-preview/engine/LAMBERTIAN_REDUCTION.md).
- **Camera and launcher:** 90° default diagonal FOV, shared forward/inverse fisheye mapping, and a development launcher that handles WSL UNC paths and locates the available CUDA build.
- **Game-engine branding:** updated README, bundled instructions and software citation metadata.

## Run

Download **NVMatrixEngine-0.1.2-preview-win64.zip**, extract the entire folder, and open **Play Water Lab.cmd**. The portable build includes its runtime dependencies; no installer, administrator access or CUDA Toolkit is required. GitHub's source-code ZIP does not contain the executable.

Target: Windows 11 x64, a high-end NVIDIA RTX GPU and compatible driver. The default launcher uses DX12 water simulation, Balanced DLSS and automatic frame generation where supported. Both lens modes are available in Esc. See `START-HERE.txt` for controls.

## Packaging and scope

**Release validation was skipped for this update.** This archive packages the existing executable and updated shaders without a new build, relocated-extraction test or runtime-validation run. The adjacent `.verification.json` explicitly records `validationStatus: "skipped"`; earlier development results are not fresh validation of this ZIP.

The ZIP includes dependency notices, a per-file manifest and the custom soundtrack. Its source commit and SHA-256 checksum identify the package. Development has used an RTX 5090; this release does not certify other hardware or a clean Windows installation.

Frame generation adds presentation frames; it does not accelerate simulation or raw rendering. Performance varies with resolution, active particles, fluid depth and lighting. CUDA/adaptive modes remain experimental, and ReSTIR PT currently covers opaque-primary diffuse indirect paths. Full ReSTIR BDPT remains future work.
