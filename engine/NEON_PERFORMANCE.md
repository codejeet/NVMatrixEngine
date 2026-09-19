# Neon Night frame-time profile

This historical memory/traversal profile was measured on an NVIDIA GeForce RTX 5090 on September 18, 2026, using two camera paths, four imported emitter samples per bounce and four maximum bounces. The scene includes the two movable neon blocks and 4K signs. Subsequent sampling improvements and current defaults are documented in [Applying RTXPT's rendering approach](RTXPT_COMPARISON.md).

| Output / internal resolution | Before, median frame | After, median frame | Rendered FPS before → after | After p95 |
| --- | ---: | ---: | ---: | ---: |
| 1280×720 / 853×480 | 28.55 ms | 10.58 ms | 35.0 → 94.5 | 11.56 ms |
| 1920×1080 / 1280×720 | 48.48 ms | 19.99 ms | 20.6 → 50.0 | 21.93 ms |

These are **rendered frames, with frame generation disabled**. DLSS Quality upscaling/Ray Reconstruction remains enabled in both versions. Each run records 240 frames at the same stationary entrance camera and excludes the first 32 frames. SER is off and photon atomics use the fixed-point path. This is a short, reproducible scene benchmark, not an average over moving around the alley. The before scene did not contain the new blocks, so the end-to-end comparison includes their additional rendering cost.

[Machine-readable measurements](neon-performance.json) include medians, p95 values, sample counts, commands, source/report hashes and the sampling sweep.

## What changed

- **GPU memory:** immutable triangle vertices, imported vertex attributes, material records and emitter tables now reside in `D3D12_HEAP_TYPE_DEFAULT` buffers. Previously, ray hits repeatedly fetched them from CPU upload heaps. Upload staging buffers are released only after the copy completes. On the updated 720p scene, camera tracing measured about 20.5 ms with hardware traversal and upload buffers, then 9.1 ms with GPU-local buffers.
- **Triangle traversal:** opaque imported meshes use opaque BLAS geometry and hardware backface culling. Alpha-masked/blended materials retain their any-hit shader; double-sided and legacy procedural meshes retain their previous visibility behavior. With GPU-local buffers, the controlled 1080p comparison measured 22.63 ms/frame for software any-hit versus 19.99 ms for hardware traversal. Depth, normal/roughness, diffuse albedo and specular albedo guides were exactly equal at every pixel.
- **Sign detail:** all five signs use newly rendered 4096×1024 or 1024×4096 artwork. Mip selection uses separate world-space U/V gradients and actual texture dimensions, preventing the excessive blur caused by the old area-only footprint on elongated signs.
- **Neon blocks:** the pink and cyan Water Lab blocks share their original geometry and controller. Their current transforms drive direct light sampling, so lighting follows pickup, throwing and rolling.

## Remaining frame cost

Updated 1080p scene, original sampling budget, median GPU timestamps:

| Work | Time |
| --- | ---: |
| Camera path tracing | 18.117 ms |
| DLSS Ray Reconstruction | 0.862 ms |
| Photon dispatch + atlas update | 0.036 ms |
| Composite | 0.014 ms |
| Bloom | 0.014 ms |
| Presentation drawing + HUD | 0.016 ms |
| Geometry/acceleration setup | 0.006 ms |
| Total GPU command span | 19.100 ms |
| Whole frame, CPU wall clock | 19.989 ms |

Individual medians do not necessarily sum to the median total. `presentationAndHud` measures GPU drawing and diagnostic copies; it does not include display scanout or Windows presentation latency. The whole-frame timer includes CPU work and waiting. The existing CPU submission timeline is retained in the raw report.

Camera tracing still accounts for about 95% of GPU time. Further optimization should focus on surface/material fetches, visibility rays and path/light sampling. Bloom and HUD work are small in this scene.

The sampling sweep isolates that remaining cost at the same 1080p output and DLSS Quality resolution:

| Paths / emitter samples / bounces | Median frame | Rendered FPS | Status |
| --- | ---: | ---: | --- |
| 2 / 4 / 4 | 19.99 ms | 50.0 | Previous play budget |
| 1 / 4 / 4 | 10.46 ms | 95.7 | Optional quality experiment |
| 1 / 2 / 4 | 8.33 ms | 120.0 | Optional quality experiment |

These measurements predate resampled lighting and Russian roulette. Reduced budgets produce fewer independent samples and can increase noise or reconstruction instability, especially in motion. See the newer report for comparisons using the current sampler.

## Repeat the profile

From Windows PowerShell in the `engine` folder:

```powershell
.\profile-neon.ps1 -Paths 2 -LightSamples 4 -Reference -Capture
.\profile-neon.ps1 -Width 1280 -Height 720 -Paths 2 -LightSamples 4 -Reference -Capture
```

The script finds a runtime using the same search order as `Play Neon Night.cmd`. Pass `-RuntimeDirectory C:\path\to\Release` to select a build. Results go into a dated folder under `engine/profiles`: raw per-frame JSON, logs, `summary.json`, `timings.csv`, and optional PPM/float-guide captures. `-Frames 600` provides a longer run. Run benchmarks serially with other GPU workloads closed.

To profile a specific sampling budget:

```powershell
.\profile-neon.ps1 -Paths 1 -LightSamples 4 -Bounces 4 -Reference -Capture
```

The corresponding executable switches are `--profile-latency`, `--neon-path-samples=N`, `--neon-light-samples=N`, `--neon-bounces=N` and `--model-anyhit-reference`. Counts accept 1–8. The reference switch restores software material/backface testing while retaining GPU-local buffers. All profiling runs explicitly disable frame generation.

Validation includes six transport shader variants, native ball/camera/block interaction tests, imported-format GPU regressions, the shared controls/settings GPU test, finite capture guides, and the reference/hardware guide comparison. The D3D12 debug layer was unavailable on the test machine.
