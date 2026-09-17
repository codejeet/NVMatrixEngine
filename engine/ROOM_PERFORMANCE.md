# Live room-water performance pass

Baseline: `e083618` (including the latest white-grid minification filter and
rolling-safe caustic history). This is the **room-wide** liquid scene, not the old
pit benchmark. Measurements and image checks are recorded in
`room-performance-validation.json`.

## Measured result

Pooled medians from 804 timed real frames per version/view:

| View | Camera GPU ms, before → after | Renderer ms, before → after | Raw throughput gain | Renderer p95 ms, before → after |
| --- | --- | --- | --- | --- |
| Stationary gameplay | 8.011 → 7.805 | 13.563 → 13.546 | effectively unchanged | 16.456 → 15.278 |
| Orbit | 6.372 → 5.188 | 12.366 → 11.717 | 5.5% | 15.201 → 14.338 |
| Rolling / jumping | 6.605 → 6.025 | 12.842 → 12.273 | 4.6% | 15.627 → 14.792 |

Camera-pass reductions are 2.6%, 18.6% and 8.8%, respectively; total-frame
improvements are smaller because simulation, reconstruction, RR, AS updates and
submission remain. The stationary median-frame difference is within run noise,
not a meaningful speedup. The p95 interval improves by approximately 5–7%.

## Scope and retained change

The dominant measured pass is camera transport through the glass/water scene;
the APIC simulation is about 2 ms, surface reconstruction about 1.0–1.2 ms,
and spectral photons about 0.7 ms at these settings. No solver iteration counts,
particle counts, field spacing, photon budgets or optical bounce limits are reduced.

Boolean visibility rays now use `ACCEPT_FIRST_HIT_AND_END_SEARCH` plus
`SKIP_CLOSEST_HIT_SHADER`. They stop on any valid occluder and do not request
surface attributes. A hit leaves a hit sentinel in the payload; the existing miss
shader clears it. This follows the [DXR ray-flag contract](https://learn.microsoft.com/en-us/windows/win32/direct3d12/ray_flag).
Procedural water and whitewater intersections are **not** skipped. Masks, ray
offsets, segment limits and the rule that photons own refracted illumination
are unchanged. This covers direct-light visibility, photon launch blocker checks
and RTXDI PT reconnection visibility across plain DXR, standard SER and NVAPI.
Camera/continuation/photon transport rays still find the closest surface.

The unchanged scalar field, normal filter, spectral estimator, atlas histories,
DLSS guides and RR evaluation remain in use. There is no new temporal denoiser,
frozen liquid, reduced-quality water setting or reliance on generated frames.

Other experiments were discarded: shared normal taps were slower; wave-reduced
diagnostic counters and an inline reflection-stack rewrite showed no reliable
gain; primary-hit reuse caused a small raw-radiance mismatch in the deterministic
optics comparison. None of these experiments is in the final shader.

## Reproduce

Before changing the shader, save the six original `Transport-*.dxil` libraries to
a separate directory. Keep the executable, non-Transport kernels and scene assets
identical. Then compile the candidate with `compile-shaders.ps1` and run:

```powershell
.\profile-room-paired.ps1 -BaselineShaderDir C:\path\to\original-shaders -Tag visibility-final
.\test-render-performance.ps1 -BaselineShaderDir C:\path\to\original-shaders
.\test-water-temporal.ps1 -Name water-temporal-performance
.\test-water-temporal.ps1 -Name water-temporal-performance-foam -Whitewater
```

```bash
node engine/validate-room-performance.mjs /path/to/runtime visibility-final
node engine/validate-water-temporal.mjs /path/to/runtime water-temporal-performance water-temporal-final --preserve
node --test engine/*.test.mjs
```

The paired runner saves/restores the candidate libraries even if a test fails,
refuses to overlap another lab/game process, and records executable/shader hashes
and actual run order. The baseline is compiled original code, not a runtime
reference branch that changes the shader's register allocation.

Settings: RTX 5090, output 1920×1080, DLSS Balanced (1114×626 internal), FG off,
100k initial particles / 250k capacity, live wall inlet and foam/bubbles,
65,536 spectral photons, 120 Hz APIC, 120 pressure and 60 density iterations,
two simulation substeps per real frame. Each run has 300 frames, of which the
first 32 are excluded. Three adjacent before/after pairs per view; order reverses
in the second pair. There are no per-cell GPU validation probes in timed runs.

The reported renderer interval includes submission, present and the GPU fence,
but excludes the outer gameplay update. Its reciprocal is **not** presented FG
FPS. Column medians need not sum. Live APIC summation order and GPU scheduling
vary between runs, so per-run measurements are included, not just a best case.

Static sphere, thin sheet and ordinary gameplay fixtures compare raw radiance,
all DLSS guides, RR output and both caustic atlases on four backend/atomic paths.
The raw-color aggregate error limit is 0.001%, RR 0.5%; geometry/material guides
and irradiance also use a 0.001% limit. This is bounded numerical agreement, not a
claim of bit-identical learned reconstruction. The camera event totals and
procedural entry/exit/root probes are checked independently. The separate
scripted orbit/top-down/rolling sequence checks temporal behavior with live and
paused liquid; its primary-guide reprojection is not ground-truth refracted motion.

Windows Graphics Tools / GPU-based validation was unavailable on this setup;
successful ordinary runs do not constitute a D3D12 debug-layer validation pass.

## Validation result

All six transport libraries and the other kernels compile; 54 Node tests and
five Windows CTests pass. The 12 deterministic before/after image comparisons
pass: maximum raw relative L1 error is below 6e-9, all geometry/material/motion/
specular-distance guides match exactly, camera event/truncation counts match,
and RR aggregate difference stays below 0.215%. Float photon sums differ only
by accumulation-order rounding; fixed-point comparisons are exact. All tested
procedural fluid roots and glass-shell entry/exit probes pass.

The 320-frame orbit/top-down/rolling sequence passes with and without whitewater,
using `--preserve` against the pre-optimization corrected sequences. RR variation
changes by less than 1%; mean luminance by less than 0.11%. Water history stays
at ten resets while ordinary rolling invalidates the static atlas 58 times;
global RR resets once. The previous flicker fix is retained, not redefined as
zero residual flicker. Room before/after captures were also visually inspected.

Five additional live-room GPU cases pass with fixed atomics, standard SER,
NVAPI SER, 2× FG and optional RTXDI PT (540 real frames). Captured guides are
finite, photon energy is bounded, and fluid/whitewater probes report no bad hits.
These probe-instrumented runs are correctness checks, not the timings above.
