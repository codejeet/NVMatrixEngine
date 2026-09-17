# Release validation

The public preview is rebuilt from this standalone source tree. It does not ship a renamed stale executable from the predecessor project.

Release checks cover native/Node tests, renderer shader compilation, real bounded water renders, and the allowlisted portable package after extraction to a different directory. Exact release results are recorded below before publication and in the downloadable verification report.

## Scope

Test machine: Windows 11, NVIDIA RTX 5090, driver 616.64. CUDA build: Toolkit 13.1, SM 86/89/120. Only the RTX 5090 was physically tested. D3D12 debug-layer/GPU validation was unavailable on this machine, so passing numerical/render checks is not a debug-layer certification.

The source retains dated numerical and image-validation records in `engine/`. These document development checkpoints, not fresh benchmark claims for this release. In particular, the live narrow-band ownership checkpoint reduced live particles in a 900k deep-pool case while preserving its volume ledger, but did not establish a net speedup over the default room solver.

No published screenshot is an offline or AI-generated substitute: the images are lossless conversions of the executable's final-frame captures. Frame generation is disabled for documentation captures; any validation of generated presentations is a separate run.

## Standalone source checks

- 241 Node reference/contract tests passed.
- 9 native CTest cases passed: gameplay/audio, orbit input, rolling camera, camera/watercraft, water optics, mesh SDF, complexity policy, collider timeline and submission profiling.
- The CUDA-enabled Windows Release executable was rebuilt with SM 86/89/120 kernels. Third-party Bullet/RmlUi header warnings remain; this is not a warning-free SDK build.
- All 374 renderer/compute shader outputs (plus the runtime UI shader source) were compiled successfully with the pinned DXC for 0.1.0 and are unchanged in 0.1.1.
- The export removes the shared UI/audio dependency on predecessor renderer/DLSS headers and uses only the included shared support plus downloaded SDKs.

The downloadable `*.verification.json` is the authoritative per-ZIP smoke-test and checksum record. Its source commit identifies the packaged snapshot. Development reports elsewhere in the repository should not be mistaken for newly measured release performance.

## Sinking-ball patch validation

Version 0.1.1 makes solid glass the `Game` constructor default, independent of renderer/UI synchronization. The rebuilt native fixture checks initial 2,500 kg/m³ density, actual descent to the floor without calling the density setter, fresh room/deep-pool defaults, optional Float behavior and reset persistence. The boat propulsion fixture explicitly opts into its lightweight pilot rather than relying on the old default.

Bounded runs now report actual physics mode, mass, density and ball height in `*.game.json`. Packaging rejects rendered room, underwater, CUDA, narrow-band and ReSTIR PT cases unless the packaged executable reports Sink and solid-glass density. The per-ZIP report records `sinkingBallVerified` separately from the unresolved FG-output check.

## Frame-generation caveat

Fresh checks on the release machine reported FG supported/loaded/enabled, Reflex active and SDK status 0, but **zero extra presented frames**. This reproduced in the predecessor executable, in foreground/topmost runs, at 720p and 1080p, and in the interactive VSync/lifecycle fixture. The cause is unresolved; it is not established as an export-only regression or a specific driver defect. Earlier development reports of working FG are not fresh release certification.

The integration and production DLLs remain available, but this preview does **not** claim verified generated output. Use the raw-render FPS for comparisons. Packaging normally rejects this outcome; the preview explicitly uses `-AllowUnverifiedFrameGeneration`, records `frameGenerationOutputVerified: false`, and preserves the warning in the release report. This exception does not bypass DLL loading/signature, SDK error, rendering, CUDA or conservation checks.
