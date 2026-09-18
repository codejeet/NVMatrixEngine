# Release validation

## Version 0.1.3: fixed-grid Hamiltonian ocean

The Windows Release executable and all 436 current shader outputs were rebuilt after removing dynamic contact-cell refinement. Every rebuilt shader matches the saved pre-refinement shader with the same name byte for byte. The source retains fixed fluid cell sizes and the existing Hamiltonian/3D activity regions.

- 241 Node reference/contract tests passed; the whitewater and packaging checks were rerun after the final source cleanup.
- The native experience fixture passed island/pier collision, hull draft, propulsion/steering, camera bounds, sink/float physics and sustained swimming.
- Hamiltonian GPU fixtures passed on 32, 64 and 128 sample grids, including FFT/dispersion, nonlinear response, feedback, source transfer and adaptive activity selection.
- All nine ocean/indoor GPU cases passed, including day/night, outlet controls, the 600-frame swimming/boat voyage, ReSTIR PT, both indoor defaults and inlet whitewater optics. Fresh results are in [ocean-validation.json](../engine/ocean-validation.json). Ocean surface storage is 42,834,756 bytes with no contact-refinement allocation.

The portable package uses the DX12 backend; optional CUDA experiments remain in source builds. Packaging checks asset consistency, vendor signatures, archive hashes and relocated-extraction runtime tests. The adjacent `*.verification.json` is the authoritative result for the downloadable ZIP, including whether actual generated presentations were observed. Tests use the RTX 5090 development machine and do not certify other GPUs, a clean Windows installation or the D3D12 debug layer.

## Version 0.1.2: validation skipped

This update packages the existing standalone executable and updated shaders. At the maintainer's request, no new build, test suite, signature check, ZIP extraction check or runtime-validation run was performed for this release. The archive still includes a per-file SHA-256 manifest and has an adjacent archive checksum.

The downloadable `*.verification.json` is the authoritative per-ZIP record. For 0.1.2 it records `validationStatus: "skipped"`, `validationSkipped: true`, no verification timestamp and an empty list of checks. Its source commit identifies the packaged source snapshot; source/runtime consistency was not rechecked during packaging.

Earlier development runs exercised the new Lambertian reduction, separate FPS counters, DLSS presets and fisheye FG; see [engine development records](../engine/VALIDATION.md) and [Lambertian measurements](../engine/LAMBERTIAN_REDUCTION.md). Those results are not fresh validation of the downloadable archive. Fisheye FG is now supported, and the HUD reports actual output presentations.

## Historical release results: 0.1.0–0.1.1

The remaining results below describe the preceding releases. Their checks covered native/Node tests, shader compilation, bounded water renders and the allowlisted portable package after extraction to a different directory.

### Test scope

Test machine: Windows 11, NVIDIA RTX 5090, driver 616.64. CUDA build: Toolkit 13.1, SM 86/89/120. Only the RTX 5090 was physically tested. D3D12 debug-layer/GPU validation was unavailable on this machine, so passing numerical/render checks is not a debug-layer certification.

The source retains dated numerical and image-validation records in `engine/`. These document development checkpoints, not fresh benchmark claims for this release. In particular, the live narrow-band ownership checkpoint reduced live particles in a 900k deep-pool case while preserving its volume ledger, but did not establish a net speedup over the default room solver.

No published screenshot is an offline or AI-generated substitute: the images are lossless conversions of the executable's final-frame captures. Frame generation is disabled for documentation captures; any validation of generated presentations is a separate run.

### Standalone source checks

- 241 Node reference/contract tests passed.
- 9 native CTest cases passed: gameplay/audio, orbit input, rolling camera, camera/watercraft, water optics, mesh SDF, complexity policy, collider timeline and submission profiling.
- The CUDA-enabled Windows Release executable was rebuilt with SM 86/89/120 kernels. Third-party Bullet/RmlUi header warnings remain; this is not a warning-free SDK build.
- All 374 renderer/compute shader outputs (plus the runtime UI shader source) were compiled successfully with the pinned DXC for 0.1.0 and are unchanged in 0.1.1.
- The export removes the shared UI/audio dependency on predecessor renderer/DLSS headers and uses only the included shared support plus downloaded SDKs.

The downloadable `*.verification.json` is the authoritative per-ZIP smoke-test and checksum record. Its source commit identifies the packaged snapshot. Development reports elsewhere in the repository should not be mistaken for newly measured release performance.

### Sinking-ball patch validation

Version 0.1.1 makes solid glass the `Game` constructor default, independent of renderer/UI synchronization. The rebuilt native fixture checks initial 2,500 kg/m³ density, actual descent to the floor without calling the density setter, fresh room/deep-pool defaults, optional Float behavior and reset persistence. The boat propulsion fixture explicitly opts into its lightweight pilot rather than relying on the old default.

Bounded runs now report actual physics mode, mass, density and ball height in `*.game.json`. Packaging rejects rendered room, underwater, CUDA, narrow-band and ReSTIR PT cases unless the packaged executable reports Sink and solid-glass density. The per-ZIP report records `sinkingBallVerified` separately from the unresolved FG-output check.

### Frame-generation results in 0.1.0–0.1.1

Checks for those releases on the development machine reported FG supported/loaded/enabled, Reflex active and SDK status 0, but **zero extra presented frames**. This reproduced in the predecessor executable, in foreground/topmost runs, at 720p and 1080p, and in the interactive VSync/lifecycle fixture. The cause is unresolved; it is not established as an export-only regression or a specific driver defect. Earlier development reports of working FG are not fresh release certification.

The integration and production DLLs remain available, but those previews did **not** claim verified generated output. Use the raw-render FPS for comparisons. Packaging normally rejects this outcome; those previews explicitly used `-AllowUnverifiedFrameGeneration`, recorded `frameGenerationOutputVerified: false`, and preserved the warning in the release report. This exception does not bypass DLL loading/signature, SDK error, rendering, CUDA or conservation checks.
