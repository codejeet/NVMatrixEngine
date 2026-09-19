# Applying RTXPT's rendering approach to Neon Night

This report records the initial Neon-only optimization. The renderer now exposes the faster RIS settings engine-wide. The subsequent temporal-adaptive implementation was reverted after a performance regression; see [current renderer settings](RENDER_SAMPLING.md). The measurements below remain a historical comparison of the earlier implementation.

The supplied RTXPT 1.7.0 screenshot shows an RTX 5090 rendering Bistro at 7.092 ms/frame, with 1536×864 internal pixels and 2560×1440 output. Settings are one path per pixel, NEE-AT, four candidate light samples, two full samples, Russian roulette, NVAPI HitObject and thread reordering, a 26-bounce limit and three diffuse bounces. ReSTIR DI/GI are unchecked. RTXPT's title code labels generated-frame rates separately; the screenshot shows its ordinary frame-rate label.

Implementation research used the newer upstream [RTXPT source at f08d1c7](https://github.com/NVIDIA-RTX/RTXPT/tree/f08d1c739071e0faad0c7c274d861124c511abab). The changes here are an independent implementation of the sampling and traversal ideas. This is not a port of the full RTXPT renderer.

## Why our small scene cost more

Our previous default traced two camera paths and independently tested four imported emitter samples plus the two blocks at each imported surface. That permits up to twelve direct-light visibility tests across the two paths at a given bounce depth, before rejecting zero-contribution samples. RTXPT's displayed setting allows two full light samples per path; its four candidates are used to select those samples before testing visibility. Candidate count is not shadow-ray count. RTXPT's source implements weighted candidate selection, and its light sampler uses local/global distributions and temporal feedback. Our original sampler only picked imported triangles from a global area-times-luminance distribution. [RTXPT next-event estimation](https://github.com/NVIDIA-RTX/RTXPT/blob/f08d1c739071e0faad0c7c274d861124c511abab/Rtxpt/Shaders/PathTracer/PathTracerNEE.hlsli), [light sampler](https://github.com/NVIDIA-RTX/RTXPT/blob/f08d1c739071e0faad0c7c274d861124c511abab/Rtxpt/Shaders/PathTracer/Lighting/LightSampler.hlsli).

RTXPT terminates low-contribution paths probabilistically and compensates the survivors. Its large maximum bounce count is a limit, not the amount of work every pixel performs. Our imported path loop had no throughput-based early termination. [RTXPT path loop and roulette](https://github.com/NVIDIA-RTX/RTXPT/blob/f08d1c739071e0faad0c7c274d861124c511abab/Rtxpt/Shaders/PathTracer/PathTracer.hlsli).

RTXPT also uses inline visibility queries and material-specific hit shaders. It applies SER around surface shading with information about the hit and path termination. Our `auto` mode disables SER, and our explicit SER path originally reordered every visibility ray through the same generic hit group. Enabling the switch alone does not reproduce RTXPT's execution structure. [RTXPT traversal bridge](https://github.com/NVIDIA-RTX/RTXPT/blob/f08d1c739071e0faad0c7c274d861124c511abab/Rtxpt/Shaders/PathTracerBridgeDonut.hlsli), [path dispatch](https://github.com/NVIDIA-RTX/RTXPT/blob/f08d1c739071e0faad0c7c274d861124c511abab/Rtxpt/Shaders/PathTracerSample.hlsl), [material hit groups](https://github.com/NVIDIA-RTX/RTXPT/blob/f08d1c739071e0faad0c7c274d861124c511abab/Rtxpt/PTPipelineBaker.cpp).

The [previous profile](NEON_PERFORMANCE.md) measured 18.12 ms of camera tracing out of a 19.10 ms GPU span at 1080p output. The scene's triangle count, bloom and HUD do not explain that cost. Our large deterministic glass branch stack and general-purpose shader also warrant a shader-profiler investigation; register spills or occupancy penalties have not been measured, so they are not assigned a speedup estimate here.

## Implemented changes

- **Shared, resampled direct lighting:** `shaders/model-nee.hlsli` gathers imported-emitter candidates and samples of the moving blocks/flashlight, weights them using incident intensity and a cheap BSDF-PDF approximation, and selects one candidate per full sample. It traces visibility and evaluates the full colored BRDF only for the selected candidate. Blocks no longer add an independent shadow-ray budget at every imported hit.
- **Estimator normalization:** imported estimates are divided by their candidate count; each separately sampled block or delta light estimates its own complete contribution. Reservoir selection is compensated by total weight divided by selected weight. Existing light/BSDF MIS weights remain complementary, using the same original emitter proposal PDF on both sides. Visibility is applied after selection, including when the selected light is blocked.
- **Russian roulette:** after the first scatter, low-throughput path tails can terminate. Surviving throughput is divided by survival probability. The first scattered hit remains available for DLSS's hit-distance guide.
- **Visibility compatibility:** shadow rays retain the existing pipeline traversal with first-hit termination, skipped closest-hit shading, hardware opaque traversal and alpha-tested any-hit acceptance.
- **Real-time budget:** normal play uses one path, two full direct-light samples, four imported candidates per reservoir and up to four surface bounces. The old two-path/four-light budget remains available. This budget change is reported separately from equal-budget measurements.

The new sampler implements per-surface resampling. It does not yet implement RTXPT's temporally trained NEE-AT distributions, material-specific shading kernels or stable-plane decomposition. More triangles in RTXPT's scene do not imply more expensive shading or more rays per pixel.

An inline-query experiment was removed after runtime failures on NVIDIA driver 616.64. The fallback pipeline caused a driver access violation; the NVAPI path repeatedly returned `DXGI_ERROR_DEVICE_HUNG` during model setup. DXC compilation alone did not catch this. The shipped code keeps pipeline visibility and the existing default of SER off. This does not establish a general limitation of inline ray queries; reproducing the driver issue in a smaller pipeline is separate work.

## Measured results

RTX 5090, same stationary Neon entrance camera and scene, 240 frames per run, first 32 excluded. Frame generation and SER are off; DLSS Ray Reconstruction remains enabled. These measurements use pipeline visibility, so inline queries contribute none of the reported gain.

| Output / DLSS mode | Sampler and budget | Camera GPU | Whole frame median / p95 | Rendered FPS |
| --- | --- | ---: | ---: | ---: |
| 1920×1080 / Quality | Reference, 2 paths / 4 full lights | 17.90 ms | 19.89 / 21.40 ms | 50.3 |
| 1920×1080 / Quality | Resampled + roulette, 2 / 4 | 7.63 ms | 9.46 / 10.42 ms | 105.7 |
| 1920×1080 / Quality | Resampled + roulette, **1 / 2 default** | 2.18 ms | 4.06 / 4.76 ms | 246.2 |
| 2560×1440 / Balanced | Resampled + roulette, **1 / 2 default** | 2.94 ms | 5.18 / 5.79 ms | 193.0 |

The controlled equal-budget change improves whole-frame throughput by **2.10×**. Changing the budget as well gives **4.90×** over the previous default in this view. Lower budgets can increase noise and reconstruction instability in motion. The 1080p runs render internally at 1280×720; our DLSS Balanced run uses 1485×835, slightly below the screenshot's 1536×864. Our 193 FPS and RTXPT's 141 FPS are different scenes and are not evidence that this engine is faster than RTXPT.

The [machine-readable report](rtxpt-performance.json) records commands, medians/p95, shader identifiers, report/source hashes and quality measurements. High-budget reference/resampled captures used eight paths and eight full light samples at 853×480 internal resolution. Depth, normal/roughness and both albedo guides matched exactly; every captured guide was finite. Average noisy RGB differed by +0.46%, −0.01%, +0.28% across the image. A floor-only mask differed by +3.69%, +0.48%, +2.95%; this single-frame stochastic check is not a converged-error bound. Visual inspection retained sign lighting, wet reflections, blocks and the glass ball.

Validation includes 243 Node checks, all six transport shader variants, the 180-frame controls/settings GPU test, glTF/GLB/OBJ and gameplay/ReSTIR GPU regressions, a 16-frame Water Lab run, both explicit and default-graphics CMD launches, and a live profiling-script smoke run. The controls test now waits for the FPS counter's 500 ms wall-clock window before asserting; faster rendering had exposed its previous fixed-frame timing assumption. D3D12's debug layer was unavailable on this machine.

## Reproducing comparisons

The current profiling script now compares the engine-wide implementations; the archived JSON above retains the commands and results for this historical version. From the `engine` folder in Windows PowerShell:

```powershell
.\profile-neon.ps1 -Sweep -Capture
.\profile-neon.ps1 -Width 2560 -Height 1440 -Quality balanced -Capture
.\profile-neon.ps1 -Paths 8 -LightSamples 8 -Reference -Capture
.\profile-neon.ps1 -Paths 8 -LightSamples 8 -Capture
```

`-Reference` restores independent direct-light sampling, the original fixed-depth path loop and pipeline visibility. It leaves GPU-local buffers and opaque hardware traversal enabled. `-LightCandidates` controls the resampling candidate count. All runs disable frame generation and report actual internal/output resolution; the RTXPT screenshot and our scene are not a matched-scene benchmark.
