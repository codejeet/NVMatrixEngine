# Engine-wide fast path-tracing defaults

Water Lab, Ocean, Neon Night and imported-model scenes use the same default settings:

- **Fast resampled (RIS)** direct-light sampling.
- **1 base camera path**, **2 full light samples**, **4 imported light candidates**, **4 surface bounces**.
- **Russian roulette enabled**, with survival compensation.

Open **Esc → Path tracing** to change these settings. The fast sampler and independent reference mode are both selectable. Counts accept 1–8. Settings apply immediately and reset reconstruction history when necessary. No scene-name check enables the sampler.

The NEE-AT extension was reverted because it reduced Neon throughput from the earlier optimized build. Its global/local proposal tables, temporal history, early-feedback pass, primary-hit cache and extra GPU dispatches are removed. `--light-sampling=nee-at` is no longer supported. The `neeAT` report field remains a compatibility marker with `enabled: false` and `allocatedBytes: 0`.

## Fast sampler

The restored sampler draws imported-emitter candidates from the static power-weighted CDF, adds candidates for the flashlight and built-in lights, then selects one candidate per full sample. Only the selected candidate receives a visibility ray and full material evaluation. Weighted-reservoir compensation and the original proposal PDF preserve complementary light/BSDF MIS. Moving lights use their current poses. There is no temporal sampling cache.

The same method handles imported PBR and built-in diffuse surfaces. Ocean contributes its existing environment sampler and lanterns; other labs contribute their point, collimated and area sources. Analytic/photon caustics remain separate. Lower sample counts can increase noise, especially during motion.

The two-full-light budget applies to ordinary PBR path vertices and diffuse direct lighting. Imported-material endpoints reached through deterministic glass retain the earlier optical endpoint budget; adaptive optical sampling can raise it. The base-path and surface-bounce settings do not replace glass branch limits or the optional ReSTIR PT estimator's separate initial-sampling/depth policy.

## Running and measuring

```powershell
.\NVMatrixFluidLab.exe --light-sampling=ris --path-samples=1 --light-samples=2 --light-candidates=4 --path-bounces=4 --russian-roulette=on
.\NVMatrixFluidLab.exe --light-sampling=reference --russian-roulette=off
.\profile-neon.ps1 -Sweep -Capture
```

These switches work with any scene selector. Older `--neon-*` count switches remain aliases for the engine-wide options; `--neon-reference` selects reference sampling and disables roulette.

The profiler records CPU and GPU frame times with frame generation off, excludes startup frames and exports median/p95 summaries. `-Sampling`, `-Paths`, `-LightSamples`, `-LightCandidates`, `-Bounces` and `-Roulette` select a configuration. Reference evaluates light classes independently, so the same nominal light-sample count can trace more shadow rays.

For a live menu regression, including budgets, roulette, flashlight, DLSS resolution changes and restart:

```powershell
.\NVMatrixFluidLab.exe --sampling-controls-test --frames=96 --capture --frame-gen=off --ser=off --atomics=fixed
```

Add `--scene=neon-night` or `--water-lab=ocean` to exercise the same controls in another scene. Current validation and measured performance are recorded in [sampling-validation.json](sampling-validation.json). The earlier Neon-only results remain in [RTXPT_COMPARISON.md](RTXPT_COMPARISON.md).

## Measured after restoring the fast sampler

RTX 5090, the Neon entrance view, 1920×1080 output / 1280×720 internal, DLSS Quality Ray Reconstruction, frame generation and SER off. Each run renders 240 frames; the first 32 are excluded.

| Current mode | Whole frame median / p95 | Camera GPU median | Rendered FPS |
| --- | ---: | ---: | ---: |
| Independent reference | 5.69 / 6.33 ms | 3.98 ms | 175.7 |
| Fast RIS default | **4.32 / 4.98 ms** | **2.37 ms** | **231.7** |

The reverted adaptive build measured 5.80 ms / 172.6 FPS at these settings. The restored shared implementation improves throughput by about 34% against that run. The earlier Neon-only implementation measured 4.06 ms / 246 FPS, so the current shared result is close but does not fully match that historical timing. No claim of an identical speedup in other scenes is implied.

The adaptive cache allocation is now zero (previously 77.5 MiB at this internal resolution). All 243 Node checks and six transport shader variants passed. Live settings tests passed in Neon, Water Lab and Ocean. At eight paths/eight full samples, reference and RIS depth/normal/albedo guides matched exactly; all captured inputs were finite. Whole-image mean RGB differed by less than 0.4%, and floor-only differences were below 3.2%; this is a single-frame stochastic sanity check.
