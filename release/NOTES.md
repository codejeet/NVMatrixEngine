# NVMatrixEngine 0.1.5 — Ocean swells and hull contact

Ocean Island now starts with broad, tall swells: a 1.8 m wave-height scale and a
48 m minimum initial wavelength, with a smooth taper between 48 and 72 m. The
retained spectral energy is normalized so selecting longer waves preserves their
height. Both elevation and velocity potential initialize the coupled simulation;
the boat, local water and rendered surface use that same evolving water state.

The initial spectrum can be adjusted with `--wave-amplitude`,
`--wave-wind-speed` and `--wave-min-wavelength`. The larger height allowance is
limited to wind spectra whose minimum wavelength is at least eight water depths.
The wave filter runs at initialization; it adds no per-frame simulation pass.

Particle contact now accounts for radius and the normal-sampling halo outside
baked mesh bounds. This corrects false contacts with empty parts of the tapered
boat hull's bounding box. Boundary seeding, detached water and secondary particles
use the same clearance rule; distant particles retain the fast rejection.

The README features a fresh ocean boat screenshot from the updated engine.
Engine-wide fast RIS path-tracing defaults, PBR glTF/GLB/OBJ imports, Neon Night,
DLSS controls, and the existing water and gameplay features are included.

Download **NVMatrixEngine-0.1.5-preview-win64.zip**, extract the entire folder, and
open **Play Extra Large Water Lab.cmd** for the ocean boat scene. **Play Water
Lab.cmd**, **Play Large Water Lab.cmd**, and **Play Neon Night.cmd** are also
included. The portable DX12 build bundles assets and runtime dependencies;
optional CUDA experiments remain in source builds.

The executable and affected shaders compiled successfully. A live ocean session
was captured for documentation. **Release tests and package validation remain
skipped at the maintainer's request.** The screenshot is not a benchmark or a
long-run stability, hull-artifact or shadow-flicker certification. The adjacent
verification JSON records skipped package validation. A source-commit manifest,
per-file hashes and archive SHA-256 checksum identify the exact payload.

Target: Windows 11 x64 and a compatible high-end NVIDIA RTX GPU/driver.
Development uses an RTX 5090; other hardware and frame-generation output were
not reverified for this release. Close and reopen an already-running lab to load
the new executable and shaders.
