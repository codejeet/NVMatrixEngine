# Fluid rendering / sampling implementation review — 12 September 2026

The [13 September CUDA/multiscale revision](CUDA_FLUID.md) supersedes the simulation
experiment ordering below: establish single-phase CUDA parity and bounded sparse
updates first; defer ST-FLIP/two-phase work until the integrated frame meets budget.

## What actually fits this engine

Simulation, ray intersection, and light sampling are separate costs. None of the
papers below is a drop-in replacement for our complete APIC + sparse procedural
DXR + spectral photon + DLSS-RR pipeline. Their published speedups compare their
own baselines, resolutions and scenes—not NVMatrixEngine's raw frame time.

| Work / primary source | Useful technique | Decision for this engine |
| --- | --- | --- |
| [ReSTIR PT Enhanced, 2026](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/) | Footprint-based reconnection, cheaper spatial reuse, decorrelation; reports 2–3× versus earlier ReSTIR PT | Integrated the already-pinned NVIDIA PT library's path reservoirs, hybrid shift, temporal/spatial MIS and footprint criterion. Independent fresh-estimator selection keeps RR's input noisy. This is a quality mode, not a promised FPS gain. |
| [Compatibility-guided neighbors, HPG 2026](https://research.nvidia.com/labs/rtr/publication/junkins2026compatibility/) | Improves variance/correlation tradeoff at 2–5% incremental cost in the paper | Good next PT quality step after broader path coverage. Current bridge uses one disk neighbor, depth/normal/material rejection; it does **not** claim to implement compatibility selection or reciprocal pairing. |
| [ReSTIR BDPT, 2025](https://research.nvidia.com/labs/rtr/publication/hedstrom2025restir/) | Light/eye subpaths and caustics reservoirs reach paths unidirectional PT misses | Research candidate, not a safe speed replacement. Paper reports roughly 50 ms/frame in its tests. Requires a new caustic MIS/ownership design; stacking it on our photon pass would double-count light. |
| [Partitioned specular manifold sampling, SIGGRAPH Asia 2025](https://research.nvidia.com/labs/rtr/publication/hong2025partition/) | Partition/resample difficult specular path samples | Candidate for selected light–prism–receiver constraints. Procedural changing-topology liquid would also need stable derivatives and robust manifold/root tracking; not implemented here. |
| [NVIDIA ray-guided water caustics, 2020](https://developer.nvidia.com/blog/generating-ray-traced-caustic-effects-in-unreal-engine-4-part-2) | Light-space surface data, photon footprints, cascaded allocation | Mature real-time reference, not new research. Its usual one-interface water assumptions do not cover detached droplets, sheets, nested glass, or repeated TIR. Keep our exact field intersections and spectral differentials; optimize conservative traversal first. |
| [ST-FLIP, SIGGRAPH 2026](https://ge.in.tum.de/publications/spatiotemporal-flip/) | Time-jittered particles and 4D P2G reduce large-timestep aliasing; authors report 2–8× in their tests | Most directly relevant **future simulation experiment**: compatible conceptual family with APIC/FLIP, unlike a solver replacement. Requires time attributes, slab-integrated transfers, resynchronization for rendering, and revalidation of collisions/emission/tension. Simply reducing substeps is not ST-FLIP. |
| [Leapfrog Flow Maps, SIGGRAPH 2025](https://yuchen-sun-cg.github.io/projects/lfm/) | GPU matrix-free AMGPCG; efficient vortical-flow evolution | Pressure multigrid ideas transfer. Their demonstrated smoke/vortex flows are not proof of a drop-in free-surface liquid solver. Keep existing fluid transport; validate a replaceable pressure solver separately. |
| [Fast VEM, SIGGRAPH 2026](https://arxiv.org/abs/2607.17725) | Boundary-aware Galerkin multigrid on body-fitted cut cells | Strong complex-boundary reference, but changes discretization, grid, transfers and pressure together. Its up-to-100× pressure improvement is against earlier cut-cell methods, not our regular MAC grid. Not a low-risk port. |

## Implemented now

- White neutral floor (0.82 linear diffuse reflectance), dark metre grid. The room
  flood source now covers the room, with an explicit 140 radiant W budget
  (~0.86 W/m² over its aperture). It remains traced through water; no emissive
  floor, ambient-light bypass, or screen-space caustic texture. The old pit keeps
  its 28 W source. Added the missing un-refracted direct complement of this source.
- Packed surface-cell bounds in the fluid page table: DXR's procedural shader now
  clips its software DDA to the same conservative region as the BLAS AABB. One
  extra uint per brick; no CPU readback, no lower field resolution, no skipped
  pressure iterations. The canonical scalar, normals, cubic root isolation and
  refinement are unchanged. `--fluid-full-brick-traversal` is a same-binary A/B
  reference.
- Genuine NVIDIA ReSTIR **PT**, previously absent, behind `--restir-pt` or
  `Play ReSTIR PT Lab.cmd`. Current scope is **two diffuse indirect segments from
  opaque primary surfaces**, including paths ending on existing caustic lighting.
  This is not just a one-bounce GI reservoir, nor a temporal radiance filter.

## PT integration and deliberate boundaries

The engine's existing direct-light sampler evaluates local lighting at a diffuse
endpoint. That Lambertian outgoing radiance (including photon irradiance) is the
SDK's view-independent endpoint integrand. Replay consumes the same light-sample
random dimensions even when skipping intermediate lighting. Reconnection applies
the BRDF, visibility, geometry Jacobian and Beer attenuation; the cached suffix
also retains attenuation. NVIDIA's SDK performs path selection, hybrid replay /
reconnection, inverse shifts and MIS normalization. Its optional light-ID NEE API
is not used; this is **not** a claim of unified ReSTIR DI/PT.

Camera-visible glass/water retain the existing deterministic Fresnel-split
specular prefix. After a diffuse event, transmissive surfaces terminate the path;
the spectral photon pass exclusively owns caustics. The first milestone therefore
does not reuse GI behind refractive primary surfaces or add glossy-indirect path
reuse. Extending those needs refractive reprojection / delta-prefix replay, not
screen-space reuse of a floor position pretending to be the water surface.

Persistent internal-resolution buffers: four 64-byte reservoir slots per padded
pixel (fresh, temporal, two history slots), two 64-byte surface frames, 256 neighbor
offsets. The camera writes fresh paths; two more `DispatchRays` passes run temporal
then spatial reuse. All traversal uses the existing SER-capable raygen helper,
including fluid procedural hit groups. No compute traversal or secondary framework.
All inter-pass dependencies have UAV barriers. Shader exports, root bindings and
the shader compiler use the repository's existing DX12 infrastructure.

Temporal reuse is rejected on camera cuts, geometry/light changes, and active
liquid/whitewater. Spatial reuse still works within that freshly rendered frame.
This conservative policy avoids caching animated caustic illumination. History
is capped at eight candidates/four frames by default. For DLSS-RR, 25% of final
pixels independently choose the fresh estimator instead of the reused estimator:
no preblur, firefly energy cap, or blending of noisy radiance histories. The SDK's
packed suffix uses nonnegative RGB, so out-of-gamut RGB endpoint components are
clipped in this optional indirect estimator; the photon atlases remain XYZ. This
is a color-space approximation, not fully spectral ReSTIR transport. The existing
DLSS guide buffers and RR + FG order are unchanged.

The normal fluid launcher keeps the baseline sampling mode until PT's broader
coverage/quality benefit justifies its cost. Use `--pt-spatial=0..2` and
`--pt-history=1..16` for controlled tests; history=1 and spatial=0 disable reuse
without reverting the fresh PT estimator. Reports expose allocation, affected /
reused pixels, temporal-valid frames, invalid samples and separate reuse timings.

Pinned dependency: RTXDI Library `f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`;
[pinned integration guide](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/RestirPT.md).
SDK sources stay in the ignored dependency cache; release output includes its
NVIDIA license. The application bridge is original engine code.

## Next performance work, ranked

1. Measure water hit / normal evaluation and camera specular branch cost after
   tight traversal. Cache/reorganize these only with hit/normal parity tests.
2. Matrix-free multigrid-preconditioned pressure solve, with the existing divergence
   and volume suite as acceptance gates. Preserve free-surface/solid coefficients.
3. ST-FLIP as an opt-in transfer/timestep experiment; compare equal simulated time,
   not fewer simulated seconds, and preserve current behavior as the reference.
4. Broader PT path coverage, then compatibility/reciprocal neighbors and duplication
   maps. Judge equal-time raw variance and RR stability, not just samples per frame.

See `test-restir-pt.ps1`, `restir-pt.test.mjs`, and the validation report for actual
GPU evidence. Research claims above are not measurements of this engine.
