# NVMatrixEngine 0.1.4 — Faster path tracing and Neon Night

This preview brings the optimized renderer to every scene: GPU-local geometry and
material data, faster opaque traversal, and shared fast RIS direct-light sampling.
Defaults are one base camera path, two full light samples, four imported-light
candidates, four surface bounces, and Russian roulette. Esc → Path tracing exposes
the sampler, reference mode and budgets.

The release adds generic glTF, GLB and OBJ loading with PBR textures and the playable
Neon Night alley. Imported CC0 street props, wet materials, 4K signs, movable neon
blocks, bloom, rolling-ball controls, FPS counters and Esc settings are included.
Startup shows progress and keeps the window responsive during background loading.
Ocean terrain caustics receive denser sampling around the glass ball and short
motion history.

Download **NVMatrixEngine-0.1.4-preview-win64.zip**, extract the entire folder, and
open **Play Water Lab.cmd**, **Play Large Water Lab.cmd**, **Play Extra Large Water
Lab.cmd**, or **Play Neon Night.cmd**. The portable DX12 build bundles assets and
runtime dependencies. Optional CUDA experiments remain in source builds.

**Tests and release validation were skipped at the maintainer's request.** This
archive packages the existing optimized build; no fresh scene runs, benchmarks,
vendor-signature checks or relocated-extraction tests were performed. The adjacent
verification JSON explicitly records skipped validation. Earlier development
measurements in the included documentation are historical, not fresh verification
of this archive. A per-file manifest and archive SHA-256 checksum are included.

Target: Windows 11 x64, a compatible high-end NVIDIA RTX GPU and driver. Development
uses an RTX 5090; this preview does not certify other hardware. Frame generation
availability depends on the GPU, driver and OS, and generated presentations do not
increase simulation speed. Actual generated output was not reverified for this
release.
