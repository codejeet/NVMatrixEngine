# Implemented architecture

This describes the integrated preview, not a proposed replacement engine. The default is the uniform DX12 liquid solver and baseline camera/photon renderer. Experiments are separately selectable.

## Frame pipeline

```text
Bullet / user input
  → GPU liquid substeps (APIC or FLIP/PIC, MAC pressure, collision)
  → anisotropic surface reconstruction + secondary whitewater
  → procedural fluid AABBs / BLAS update → scene TLAS
  → light-side spectral photon DispatchRays → receiver-space accumulation/history
  → camera DispatchRays → raw lighting + DLSS guides
  → optional ReSTIR PT temporal/spatial raygen passes
  → DLSS Ray Reconstruction + super resolution
  → presentation / optional frame generation / RmlUi
```

This is a dependency diagram, not a claim that simulation N+1 overlaps rendering N. CUDA/DX12 external fences preserve ownership; the current integrated schedule still has synchronization and dense work to optimize.

## Engine connections

| Concern | Implementation |
| --- | --- |
| Frame orchestration, descriptors, resource states | `engine/src/renderer.cpp`, `renderer.h`, `gpu_resources.h` |
| BLAS/TLAS, SBT and transport pipelines | Renderer plus `engine/shaders/transport.hlsl` |
| Liquid simulation and resources | `engine/src/fluid/fluid_system.*`; compute kernels in `engine/shaders/fluid/` |
| Surface and procedural geometry | `fluid_surface.*`, `reconstruction.hlsl`, `field.hlsli`, `intersection.hlsli` |
| Optical importance | `engine/src/optical_importance.*`, `engine/ADAPTIVE_OPTICS.md` |
| GPU/CPU profiling | Timestamped renderer/fluid stages, PIX markers, bounded-run JSON reports |
| DLSS / Reflex | `engine/src/streamline.*` |
| UI / rigid bodies / audio | RmlUi HUD, Bullet gameplay and `shared/src/` support |

There is no additional rendering framework or separate fluid screen-space renderer.

## Liquid solver

Particles hold position, velocity and APIC affine terms. GPU binning builds cell ranges for neighborhood operations. Quadratic B-spline transfers independently accumulate momentum and weights on staggered MAC faces. Grid velocities are normalized, copied for FLIP deltas, forced, constrained against solid velocities, projected through the pressure solve, and extrapolated before grid-to-particle transfer.

APIC is the default. The optional FLIP/PIC mode blends the projected grid velocity with the pre/post-projection velocity delta. Particle advection, SDF collision, and density repair follow. The initial pressure baseline is fixed-iteration Jacobi; more elaborate active and hierarchical solvers remain selectable experiments. Simulation substeps are independent of rendered frames, within bounded catch-up limits.

Analytic and voxelized mesh SDFs provide solid collision. Moving rigid-body geometry supplies boundary motion. Viscosity and surface tension use real-time approximations rather than claiming a fully resolved two-phase Navier–Stokes solver.

### Surface representation and intersection

Neighbor covariance produces anisotropic reconstruction kernels. A sparse brick field stores a continuous, piecewise trilinear scalar, with 8³ cells / 9³ nodes per brick. Simulation particles and the surface representation are distinct: the particles are not the primary visible geometry.

Active brick AABBs form a procedural BLAS in the scene TLAS. The intersection shader clips the ray to the brick and traverses its cells with integer DDA. Within each cell, the trilinear field restricted to the ray is cubic. The shader isolates roots between extrema and uses 16 bisection iterations to refine crossings. It does **not** assume this reconstructed scalar is an exact signed-distance bound and blindly sphere-trace through thin water. Field gradients supply boundary normals used by both camera and photon rays.

Optional surface LOD uses reconstruction error and optical importance, with conservative canonical sampling. It does not yet deliver a generally useful gameplay speedup or fully compressed multiresolution surface storage.

### Foam and rigid-body coupling

Surface foam is an advected noisy coating rather than uniformly visible particle spheres. A bounded secondary pool supplies bubbles/spray; this is not a pressure-coupled air phase. The simulation and optical interfaces still use the continuous water field.

Twenty GPU field/velocity probes drive approximate hydrostatic buoyancy and quadratic drag for the boat, avatar and props. Only small probe results cross the existing frame synchronization—not the particle arrays. Moving solid SDFs interact with the water. This is practical two-way visual interaction, **not** a monolithic, exactly momentum-conserving fluid/rigid-body pressure solve; overturning water and slamming are limitations of the height-probe model.

## CUDA and narrow-band ownership

CUDA is optional at build and runtime. External D3D12 memory and fences keep buffers GPU-resident. Graph replay reduces launch overhead. Uniform CUDA pressure is separate from the more expensive fine/mixed MGPCG experiments; conditional graph pressure and CUDA-in-Graphics scheduling are additional explicit options.

The live narrow-band path actually retires calm interior particles into flowing 2×2×2-cell grid owners. Both owners share FP64 volume/linear-momentum accounting. Retirement clears particle records and returns IDs; restoration reserves safe recycled IDs before moving quantity back. Failure to find capacity or collision-safe sites leaves volume grid-owned rather than losing it.

Surface/solid proximity, velocity/APIC variation, padding and a dwell period protect important regions. Combined particle/grid volume participates in grid transfer, pressure support, density measurement and the same canonical rendered surface. Smaller grid remnants stay grid-owned until enough quantity exists for a useful restoration stencil.

This is a real ownership implementation, but **not yet a fully sparse multiresolution fluid solver**: fine-grid work, capacity-sized buffers, reconstruction and synchronization can dominate after particle retirement. Fewer particles alone do not establish lower frame time. See [the exact ownership implementation and evidence](../engine/NARROW_BAND.md).

## Light transport

### Camera paths and spectral photons

Camera transport and light-side caustic transport have separate responsibilities. The photon pass samples light emission and wavelengths over 380–780 nm, traces refractive/specular chains through glass and the actual animated fluid, and splats normalized differential footprints into receiver-space accumulation. Wavelength-dependent IOR, Fresnel/TIR, absorption and CIE XYZ conversion produce spectral separation without making the RGB guide albedo itself rainbow-colored.

NVAPI floating-point atomics are capability-selected, with a fixed-point fallback. Receiver-space temporal filtering is independent of DLSS's screen-space reconstruction. It stabilizes finite photon samples; it is not an unbiased infinite-resolution caustic solution.

Native DXR HitObject and NVAPI SER shader variants are implemented, but `--ser=auto` deliberately retains ordinary TraceRay in this workload: reordering short visibility rays with the current large live dielectric state was not a demonstrated win. `--ser=dxr` / `--ser=nvapi` select the capability-checked experimental variants; support is not the same as a measured speedup.

Camera rays preserve bounded deterministic reflection/refraction splitting at primary dielectric prefixes. Diffuse eye paths do not duplicate photon-owned transmissive caustics. The system supports multi-interface specular photon paths, but it is not a general BDPT vertex-connection/MIS integrator.

With procedural liquid active, diffuse endpoints use contribution-weighted, compensated visibility sampling of point and area lights, testing every nonzero candidate with probability at least 50%. Higher optical sample budgets, collimators and the flashlight retain full visibility. Water shadow queries stop at a proven root bracket; camera and photon queries retain full distance refinement. [Estimator, water behavior and reference switches](../engine/LAMBERTIAN_REDUCTION.md).

### ReSTIR PT and adaptive optics

`--restir-pt` integrates the pinned NVIDIA RTXDI PT kernels for two-segment diffuse indirect paths originating on opaque primary surfaces. Hybrid replay/reconnection, temporal and spatial reuse, and library MIS reuse entire path candidates rather than blur an already shaded image. Four padded-pixel reservoir buffers and two surface-history buffers add significant memory/work. A fresh-estimator mixture preserves noisy input for RR.

History is invalidated for incompatible scene/camera changes and active water/whitewater changes; spatial reuse can still operate. Glossy/refractive-primary GI reuse, unified ReSTIR DI, and ReSTIR BDPT are not implemented. This mode is optional quality/research functionality, not an advertised speedup over the cheaper baseline.

`--adaptive-rays` estimates optical importance from radiance variance, geometry/motion/reprojection confidence and actual photon receiver contribution. It changes fresh sample allocation and feeds optical error back to surface importance. It preserves estimator accounting rather than arbitrarily suppressing dark pixels. Validation and limitations are in [ADAPTIVE_OPTICS.md](../engine/ADAPTIVE_OPTICS.md).

### Water medium and DLSS

Water is a dielectric boundary with approximately 1.333 IOR, depth-dependent Beer–Lambert attenuation and medium entry/exit handling. Visible laser beams integrate extinction and single scattering. General multiple-scattering liquid volumes and arbitrary nested media are not claimed as complete.

The renderer supplies internal-resolution radiance, diffuse/specular albedo, normal/roughness, depth, motion and specular-hit-distance guides to Streamline DLSS Ray Reconstruction. RR performs screen-space denoising and super resolution; there is no intervening SVGF/NRD screen-space denoiser. Caustics remain lighting, not baked albedo. Frame generation and Reflex are optional presentation integrations; generated frames do not represent extra physics steps or path-traced samples.

## Research references

These are conceptual foundations or integrated libraries, not claims that their complete published systems have been reproduced:

- [NVIDIA RTXDI ReSTIR PT integration](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/RestirPT.md); the build pins its library revision.
- [Streamline](https://github.com/NVIDIA-RTX/Streamline), including its versioned DLSS input contracts.
- [APIC: The Affine Particle-In-Cell Method](https://doi.org/10.1145/2766996).
- [CIE standard-observer data](https://doi.org/10.25039/CIE.DS.xvudnb9b).

Narrow-band, adaptive-grid and multigrid research motivates the experiments. ST-FLIP, full two-phase simulation and a general multiscale free-surface solver are not shipping features.
