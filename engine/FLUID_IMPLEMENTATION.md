# GPU liquid implementation map

Current checkpoint: the opt-in liquid lab now has an end-to-end procedural DXR
render path, not just particle debug rendering. See the September 12 checkpoint
below for validated capabilities and remaining limitations. Mainline `native/`
and its portable release are unchanged.

## Render/sampling checkpoint — 2026-09-12

The pool now has a white gridded floor and room-wide traced fill. Fluid page
tables include packed surface-cell bounds so the procedural shader skips empty
brick extents without changing the scalar field or root solver. Optional NVIDIA
ReSTIR PT resamples opaque-primary diffuse indirect paths; specular water/glass
prefixes and photon-owned caustics retain their existing transport. See the
[research/integration review](FLUID_RENDER_RESEARCH.md) and
[measured validation](VALIDATION.md). The APIC solver is unchanged by this pass.

## Room inlet and whitewater checkpoint — 2026-09-12

The fluid launcher now selects a room-wide shallow pool with a GPU swept-disc
wall inlet and separate bounded foam/bubble/spray phase. See
[controls, architecture and numerical limits](ROOM_WATER.md). `--fluid-pit`
preserves the earlier glass tank, while `--fluid-validate` by itself continues
to select its existing regression fixture. The ordinary non-fluid chamber is
unchanged.

This adds the wall-emitter subset of M13 and a subgrid M14 implementation, not
every proposed emitter/drain/force API or resolved multiphase simulation. The
canonical smooth APIC field remains separate from secondary sphere geometry.
Both use the existing queue/resources, DXR pipeline, optics and RR/FG integration.
Secondary AABBs never require CPU position readback. The added timing columns
measure secondary update and BLAS separately from camera/photon cost.

## Raw-frame optimization checkpoint — 2026-09-12

Per-frame GPU timing history now separates simulation, reconstruction and BLAS
from the photon/camera/RR passes. The water/glass camera-ray workload dominates;
photon caustics are much cheaper. See [measured results](VALIDATION.md) and
`profile-fluid.ps1` for identical 100k-particle, 1080p Balanced, FG-off workloads.

- `SurfaceBounds` reuses its existing eight-corner cell reads to generate a
  conservative 512-bit surface mask per brick and tighter surface-cell AABBs.
  The page-table buffer contains these masks after its spatial-to-slot mapping.
  DXR skips field loads in strictly one-sign cells; retained cells use the same
  cubic root isolation/refinement. Zeros/tangencies and thin sheets are retained.
  BLAS prebuild/build both prefer fast trace; topology still rebuilds on GPU.
- Pressure divergence and density-gather passes cache six-neighbor masks, inverse
  diagonals and constant RHS/ghost-pressure terms before Jacobi. They reuse MAC
  face scratch only between its diffusion/extrapolation lifetimes and after G2P.
  No extra solver buffer or CPU synchronization. The free-surface/capillary and
  solid-boundary operators, 120/60 iterations and 120 Hz timestep are unchanged.
- Beam raygen assembles air/water scattering and receiver-endpoint masks in spare
  bytes of the existing stats UAV. Camera rays visit only eligible segments, in
  the original summation order; glass has no scattering. Emission, attenuation,
  beam integration, Fresnel branches and photon power are unchanged.

Anisotropic reconstruction, filtered normals, field resolution, particle count,
optical event/weight limits and RR settings are not reduced. Simulation floating
point summation/reassociation can alter individual trajectories over time; this
is not a deterministic replay feature. Static canonical-field comparisons check
optical preservation separately from the long-run physical validation suite.

This supersedes the interrupted CPU heightfield experiment. That unvalidated work,
including the spectral-noise prototype, is recoverably parked in the git stash
`Superseded heightfield and spectral-noise WIP before GPU liquid spec`.

## Connections to this engine

| Existing system | Concrete integration |
| --- | --- |
| DX12 ownership | `src/renderer.cpp`: existing NVIDIA device, direct queue, allocator, command list and fences; fluid never creates a second renderer/device |
| Buffers/resources | Extract the existing committed-buffer helper into `src/gpu_resources.h`; renderer and `src/fluid/fluid_system.cpp` share it |
| Compute | Existing DXC/PSO pattern; fluid owns its root signature and replaceable pass PSOs under `shaders/fluid/`; `compile-shaders.ps1` compiles them |
| Frame scheduling | No frame graph exists. `Renderer::render` explicitly records fluid substeps before photon/camera rays; UAV barriers and state transitions are explicit. Keep one queue until correctness/timing justify double-buffered async overlap |
| DXR | `Renderer::loadScene/buildTlas/pipelines/dispatch`: add a fluid procedural BLAS and separate hit-group/SBT record at the procedural-surface milestone; retain triangles for ordinary geometry |
| Path tracer/material | `shaders/transport.hlsl`: `trace`, `surface`, `glass`, `interfaceMedia`, `dielectricView`; fluid returns the existing water material/medium ID 8 with field-derived normals |
| Medium | `shaders/common.hlsli`: measured spectral water IOR/absorption and RGB camera extinction; replace tank-height membership with canonical fluid-field membership when enabled |
| Caustics | `PhotonRaygen`, differential splats and `resolve.hlsl`; actual fluid intersections drive photon paths. Separate changing-fluid history from stationary transport; address spectral variance before claiming stable animated caustics |
| UI/input | `src/hud.*`, `ui/lab.rml`, `src/main.cpp`: opt-in fluid lab, pause/single-step, diagnostics; keep gameplay and minimal FPS intact |
| Presentation | `src/streamline.*`: RR/SR then optional DLSS Frame Generation with Reflex; interpolate rendered liquid, never advance the solver on generated frames; separate UI and HUD-less scene |
| Timing/validation | Existing timestamp/readback/report/capture infrastructure; add separate fluid timings and deterministic GPU/CPU checks. No particle readback in normal simulation |
| Rigid bodies | Shared `native/src/game.*` stays unchanged initially; later coupling consumes transforms/velocities and returns reduced forces without per-particle CPU work |

## Milestone gates

1. Persistent GPU particles, GPU initialization/spawning, gravity/advection, debug
   particle visualization; deterministic GPU gravity/collision/finite-state tests.
2. Histogram → hierarchical exclusive scan → scatter; every active particle occurs
   exactly once in its cell range, empty/boundary/non-power-of-two tests.
3. Staggered MAC P2G with quadratic weights/APIC terms; partition-of-unity and
   constant/affine velocity tests. Keep pressure solver behind a pass abstraction.
4. Free-surface pressure projection; measure divergence before/after, residual,
   and closed-container compatibility (not just plausible screenshots).
5. G2P PIC/FLIP/APIC and stable fixed substeps; transport/volume regression scenes.
6. Analytic then reusable mesh SDF collisions, including grid boundary velocities.
7. Continuous isotropic scalar field, explicit support bounds and topology tests.
8. GPU brick AABBs + procedural DXR intersection, zero-crossing refinement and
   gradients; no CPU brick-position readback or marching-cubes default.
9. Dielectric/medium integration; closed-volume entry/exit and Beer–Lambert tests.
10. Animated caustic-pool and nested-optic tests, including cold-start colour noise.
11. Covariance-based anisotropic reconstruction with stable eigensystem/scale caps.
12. Viscosity/surface tension, timestep-aware stability and droplet tests.
13. GPU emitters/drains/forces with capacity/overflow diagnostics.
14. Separate foam/spray/bubble hooks and secondary-particle rendering.
15. Sparse/indirect work, pressure improvements, AS strategy and async overlap,
    justified by separate simulation/reconstruction/BLAS/intersection timings.

100k particles and 60 FPS **combined** is an acceptance target, not a claim about
the unfinished system. Debug particles are not the final liquid surface. Dense MAC
storage is acceptable initially; simulation and render-field representations remain
separate. A non-distance scalar field requires cell-aware root isolation, not an
unsupported assumption that multiplying its value by 0.5 makes sphere tracing safe.

## Primary references

- [Jiang et al., The Affine Particle-In-Cell Method (2015)](https://www.andyselle.com/papers/24/apic.pdf)
- [MAC-grid APIC (2020)](https://www.cs.ucr.edu/~shinar/papers/2019-mac-apic.pdf)
- [Kugelstadt et al., Implicit Density Projection (2019)](https://andreaslongva.com/pdf/2019-TVCG-ImplicitDensityProjection-compressed.pdf)
- [Author's IDP reference integration in MantaFlow](https://github.com/thunil/mantaflow/blob/master/source/plugin/implicitdensityprojection.cpp)
- [Bridson/Müller-Fischer, Fluid simulation course notes](https://www.cs.ubc.ca/~rbridson/fluidsimulation/fluids_notes.pdf)
- [DirectX Raytracing functional specification](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
- [Elek et al., Spectral Ray Differentials (2014)](https://elek.pub/research.html)
- [Yu and Turk, Reconstructing Surfaces of Particle-Based Fluids Using Anisotropic Kernels (2010)](https://faculty.cc.gatech.edu/~turk/my_papers/sph_surfaces.pdf)

## Milestone 1 checkpoint — 2026-09-11

Built all six existing DXR variants plus fluid compute/debug shaders. Five actual
RTX 5090 GPU runs passed: 0, 1, 257 and 100,000 particles over eight fixed substeps,
then 100,000 particles over 360 substeps. All state was finite, active counts exact,
and particles stayed in the domain. Maximum early free-fall position/velocity error
was `3.813e-7` against the analytic solution. The final two-substep particle-only
GPU update took `0.050 ms` in the long run; this is **not FLIP performance**.

Run `NVMatrixFluidLab.exe --fluid`. `P` pauses the fluid, `.` advances one substep while
paused, `B` resets particles, and `V` toggles debug points. `U` exposes details.
`test-fluid.ps1` runs test-only particle readbacks; normal runs map no particle data.
The ordinary chamber remains the default. No liquid surface/pressure/coupling is
claimed at this checkpoint.

## Milestone 2 checkpoint — 2026-09-11

GPU histogram, block prefix scan, cell ranges and scatter are implemented. All
five particle cases passed again, including exhaustive range/membership/uniqueness
validation across the default **105,750 cells**. The test permits either neighboring
cell only within 2 micrometres of a cell boundary (CPU/GPU reciprocal rounding).
The 100k-particle long run's final two integration steps + binning took `0.092 ms`.
This remains ballistic debug transport, not incompressible fluid yet.

`--gpu-validation` attempts D3D12 debug + GPU validation before creating the device.
This Windows installation still returns `0x887A002D` for the debug interface.
No debug-layer/GBV validation is claimed; no OS feature or driver was changed.

## Milestone 3 checkpoint — 2026-09-11

Quadratic APIC face gathers now populate true staggered U/V/W grids, with separate
normalized velocity, saved pre-force velocity and weight. All seven GPU cases
pass, including 100k-particle constant and affine reproduction and per-axis weight
conservation. Three CPU numerical tests cover the quadratic moments, gather
support and block scan. P2G is not yet fed back into particles at this checkpoint.
The final gravity test update measured `0.433 ms`; the highly collapsed ballistic
floor test measured `1.293 ms`, illustrating why pressure/feedback are necessary.
Official, hash-pinned WinPixEventRuntime markers label fluid passes; this does not
replace the still-unavailable D3D12 debug-layer validation.

## Milestone 4 checkpoint — 2026-09-11

Dense free-surface Jacobi projection uses zero air pressure and no-through domain
walls. The solver dispatch is isolated in `projectGrid()` for later replacement.
GPU validation checks pressure finiteness, classification, divergence reduction,
and the identity between the pressure-equation residual and projected divergence.
The 100k compression fixture reduced RMS divergence from `3` to `1.355 /s` in 60
iterations (about 55%). This is a **limited V1 solve**, not a converged high-quality
pressure solution. A separate numerical test covers closed-wall compatibility and
free-surface convergence. Arbitrary solids and cut-cell boundaries are still M6.

`G` cycles particles → MAC velocity colour → pressure → divergence → liquid cells;
`V` toggles the selected overlay. These are debug billboards, not water rendering.
Normal reports use `null` for occupancy metrics that were not read back, rather
than misleading zero counts. All state readback remains explicit validation-only.

## Milestone 5 checkpoint — 2026-09-12

The full fixed-step GPU loop is connected: bin → APIC/FLIP P2G → gravity/boundary
conditions → pressure → four-layer velocity extrapolation → G2P → grid-velocity
advection → domain collision → positional density correction → current-position
bins. APIC is the default. `--fluid --fluid-flip` selects a distinct FLIP/PIC
transfer with `flipRatio=0.95`; affine terms are not accidentally added to FLIP.
Saved grid velocities are extrapolated alongside projected velocities.

The roundtrip test reports velocity error `1.073e-6 m/s` and affine derivative
error `1.182e-5 /s`. It uses separate tolerances (`1e-5 m/s`, `5e-5 /s`) because
the affine update differentiates float data at an 8 cm grid spacing. Bulk velocity
is subtracted when accumulating moments to reduce cancellation.

Longer runs caught substantial volume loss that finite-state/count/divergence
tests alone missed. Increasing Jacobi iterations did not fully solve it. An
original GPU compression-only positional density projection was added, informed
by Kugelstadt et al.'s IDP work. It leaves velocities/APIC terms unchanged, uses
quadratic density gathers, accounts for missing kernel support at domain walls,
and bounds each correction to 0.2 cells. This is an initial unilateral variant,
**not** the complete paper's general-solid/multiphase algorithm. It neither
spawns nor deletes particles. Rest particle volume comes from the initialized
lattice; under-resolved tiny-count debug fixtures cap it at half a grid cell.

All **16 GPU cases / 2,556 DLSS frames** passed, including actual window-message
pause, resume, single-step, reset and debug-view toggles. Six fluid numerical tests
and the 19 existing optics/source tests pass. The 100k APIC test after 2,400
substeps retained **97.28% of kernel-estimated rest volume** (5.26742 / 5.41494 m³),
with maximum cell occupancy 24 instead of the pathological 1,000+ seen without
correction. The long-run gate requires at least 95%. This density-integral proxy
is **not** a reconstructed geometric-volume measurement; M7 must add that test.
Final two-substep GPU times were approximately **1.92–2.17 ms** in the 10/20-second
settling tests. These are individual query samples, not an end-to-end liquid
rendering performance claim. Pressure remains a finite-iteration approximation
(120 velocity / 60 density iterations), not a fully converged solve.

`Play Fluid Lab.cmd` launches the debug simulation. The ordinary lab is still the
default. **Milestones 6–15 remain:** scene/mesh SDFs, continuous scalar field,
procedural DXR geometry, medium/animated-caustics integration, anisotropy and the
remaining fluid features/optimizations. The old static water snapshot is disabled
only in fluid mode, so it cannot be mistaken for the new simulation surface.

The existing four CTest cases and all five playable GPU regression cases also
passed (1,200 ordinary-chamber DLSS frames); their captures passed finite-guide,
atlas and motion checks. The fluid particle preview was inspected for visibility
and tank-wall occlusion. D3D12 debug/GBV remains unavailable on this Windows
installation; validation coverage above must not be described as a debug-layer pass.

## Continuous render-pipeline checkpoint — 2026-09-12

`Play Fluid Lab.cmd` / `NVMatrixFluidLab.exe --fluid` now renders the simulated water
as actual procedural scene geometry. `I` switches between the ball camera and
an orbitable pool inspection camera. `P` pauses, `.` single-steps, `B` resets;
`V` toggles solver overlays, `G` cycles their modes, and `N` cycles fluid normals,
brick IDs, motion guides, and normal shading. `U` keeps the existing minimal UI.
`--fluid-view` starts at the pool; `--fluid-solver-only` retains numerical-test mode.

### Implemented connections

- **Collisions:** sphere/box/capsule/cylinder/plane SDF kernels, moving MAC boundary
  velocities, solid-aware pressure/density projection, and particle contact with
  relative velocity, restitution, friction and slip. The ball, light cubes and
  triangular prism supply current rigid transforms and boundary velocities.
- **Mesh assets:** `src/fluid/mesh_sdf.h` provides a bounded reference import-time
  bake: watertight oriented-manifold validation, exact triangle distance and
  solid-angle inside/outside classification. The prism asset uploads once to a
  default-heap GPU buffer. Open meshes are rejected instead of treated as solid.
  The first API accepts one reusable asset; large meshes need a faster offline
  baker/asset cache. The room's open receiver quads are not voxelized as a solid.
- **Surface:** `FluidSurface` consumes the solver's GPU view, separate from APIC
  storage. A GPU-compacted page table/list addresses 8³-cell bricks with shared
  9³ node values. GPU-generated `ExecuteIndirect` arguments schedule reconstruction;
  no CPU particle/brick-position readback. Default field spacing is 4 cm.
  Backing buffers reserve their configured worst-case capacity (about 32.7 MB for
  field/shapes/BLAS here); this is sparse active work, not hardware tiled residency.
- **Anisotropy:** weighted covariance, a bounded symmetric Jacobi eigensolve,
  contracted ellipsoidal kernels, determinant-normalized weights and a small
  render-only center shift. This is an original covariance-shaped weighted-center
  reconstruction, **not** an implementation of the complete Yu/Turk density-sum
  method. Isolated particles retain spherical kernels; `--fluid-isotropic` is the
  baseline comparison. Simulation positions are never changed by reconstruction.
- **DXR:** GPU surface AABBs feed an independent procedural BLAS and SBT hit group.
  A full small BLAS rebuild is intentional: DXR does not permit inactive NaN AABBs
  to become active during an UPDATE/refit. Unchanged fluid can reuse its BLAS.
- **Intersection:** exact brick slab clipping, integer cell DDA, cubic root
  isolation at derivative extrema and 12-step bracket refinement. Same-sign
  endpoint intervals can contain two crossings; these are handled. No sphere-tracing
  distance-bound assumption, screen-space surface, or runtime marching-cubes mesh.
- **Optics:** camera, shadow, reflected/refracted continuation, photon and beam rays
  all use the same procedural hit group. It returns the existing water dielectric,
  measured spectral IOR/absorption, Fresnel/TIR and Beer–Lambert transport. Guides
  use reconstructed normals and weighted previous-render particle displacement.
  Reset/paused frames do not replay stale fluid motion into DLSS.
- **Caustics:** fluid-hit photon paths accumulate into a separate XYZ atlas with
  four-frame bounded EMA during simulation, longer accumulation when paused.
  Avatar-only motion preserves this short water history (and shortens paused
  history to four frames); light/optic/other-object changes and explicit resets
  still clear it. Static atlas and PT invalidation remain independent/unchanged.
  Static prism history remains independent. Water-flood photons use eight
  correlated, stratified wavelengths per aperture sample (when the budget divides
  by eight), with actual per-wavelength paths and unchanged radiant power.
  Composite sums both lighting atlases before the existing DLSS-RR/SR evaluation.
- **Diagnostics:** separate simulation/reconstruction/BLAS timings, active/surface
  bricks, finite field/kernel checks, a tetrahedral volume **estimate**, and optional
  DXR probe root/cell-walk counters. The volume diagnostic integrates local linear
  tetrahedra only; rendering still intersects the canonical trilinear scalar field.
  It is more useful for thin sheets than cell-center occupancy, but not an exact
  geometric volume integral. Detailed capture files append the separate fluid atlas.

### Gates and limits

`test-fluid-render.ps1` covers empty geometry, an analytic sphere, a 2 cm slab,
fall/splash/pool states, sphere/box/imported-prism collisions, isotropic comparison,
fixed/float photon atomics, standard SER and NVAPI hit objects. Through-rays check
entry/exit root residuals independently through the field sampler; analytic fixtures
also check expected intersection distances. The sphere test caught and eliminated
a floating-point repeated-cell DDA stall before shipping this checkpoint.

The existing solver and gameplay suites remain required. `validate-captures.mjs`
checks all finite RR guides, positive/bounded combined caustics, photon power
accounting and history limits. Probe counters add significant GPU work; do not
quote those instrumented frame times as ordinary gameplay performance.

This is an end-to-end **clear-water render checkpoint**, not completion of the
whole middleware specification. Remaining work includes conservative nested
dielectric/contact handling (the initial SDF surface uses a small contact clearance),
general volume multiple scattering/material presets, topology-aware deformation
motion at splits/merges, sub-grid sheets/droplets, and geometric-volume calibration.
Camera dielectric continuations and laser branches are bounded and report dropped
paths; dense splashes can still hit those budgets. The initial simulation domain
is bounded with closed walls, not an overflowing arbitrary-world liquid domain.

Milestones 12–15 remain: viscosity/tension, emitters/drains/forces, secondary
foam/spray/bubbles, sparse simulation, better pressure solvers, async overlap and
two-way rigid-body coupling. Collision-grid boundaries are voxelized, not cut-cell;
particle collisions are projection with substeps, not swept CCD. The default water
material is wired; the future `FluidMaterialDesc` presets are not a completed API.
No Zibra-parity or universal 60 FPS claim is made at this checkpoint.

### Validation results

Windows Release build and all six transport DXIL variants compile. **31 Node
numerical/source tests and five CTests pass.** The final GPU regression set covers
12 liquid-render cases (849 frames), 16 solver cases (2,556 frames), and all five
ordinary gameplay cases (1,200 frames). Each frame evaluated real DLSS. The new
input case renders all surface debug modes and verifies inspection-camera switching,
pause/step/reset and solver-overlay controls through actual window messages.
Explicit camera/debug switches and liquid reset are RR cuts; continuous simulation
does not reset global RR history. Raw captures pass finite-guide, combined-atlas
and photon-energy checks.

Analytic sphere volume estimate: **1.76465 m³** versus **1.76715 m³** analytic;
2 cm slab: **0.07776 m³** versus **0.08000 m³** analytic. Both pass their 2% / 5%
discretization tolerances. Simulated-water reconstruction volume is still a
calibration task: the pool estimate can exceed particle rest volume by about 11%.
This is surfaced in diagnostics, not hidden as a volume-conservation claim.

Uninstrumented renderer-frame medians on the local **RTX 5090**, with 100k particles,
65,536 spectral photons and full fluid/DXR/DLSS rendering:

| Output / DLSS mode | Internal | Frames | Median renderer frame | 95th percentile |
| --- | --- | --- | --- | --- |
| 1280×720 / Quality | 853×480 | 600 | 11.65 ms | 19.89 ms |
| 1920×1080 / Quality | 1280×720 | 300 | 18.00 ms | 36.68 ms |
| 1920×1080 / Balanced | 1114×626 | 300 | 15.35 ms | 29.89 ms |

These are bounded, unpaced runs with a 32-frame warmup, not guaranteed interactive
FPS or worst-case frame times. The longer 720p run contains more settled frames;
it is not a controlled resolution-only A/B comparison. Last-frame query samples
were roughly 2.5 ms simulation, 2.7–3.2 ms reconstruction and 0.09–0.10 ms fluid BLAS
in the Quality runs. Shaping/neighborhood work and dielectric camera continuations
remain optimization targets. Use `--quality=balanced` to select the existing DLSS
Balanced path explicitly; Quality remains the default.

D3D12 debug/GBV was retried and is still unavailable: debug-interface creation
fails with **0x887A002D** before device creation. The normal GPU tests above are
not a debug-layer pass. No OS features, driver settings, mainline source or portable
release were changed.

## Glass pit and initial material dynamics — 2026-09-12

The fluid lab now uses a **closed glass annulus** for its four pit walls, with
the existing neon rim and charted diffuse floor. It is a separate static triangle
BLAS/instance using the transmissive mask, not alpha-blended room geometry. The
corners share a continuous boundary: there are no overlapping glass boxes or
hidden internal faces. Gameplay body indices and the ordinary chamber stay intact.
The glass uses the existing Cauchy dispersion and a deliberately clear neutral
extinction of `0.005 /m` (a chosen scene parameter, not measured glass data).
Camera, photon, and laser transport all recognize its dielectric material.
An analytic shell-membership query handles rays originating within a wall.

Four matching box SDFs join the fluid's collision/reconstruction inputs. The
existing small surface-contact clearance remains: this is not a completed wet
glass/water contact or nested-medium priority solver. Grid interpolation and that
clearance can leave an air layer at the wall; accurate wetting/contact angles and
multilayer specular motion remain open. Extra glass interfaces also add camera
path cost and can reach the existing bounded specular continuation budget.

### Milestone 12, V1

- `FluidSystemDesc` exposes SI density, **kinematic** viscosity, and surface tension.
  Water defaults are `998.207 kg/m³`, `1e-6 m²/s`, and `0.0728 N/m`. CLI overrides:
  `--fluid-viscosity=...`, `--fluid-surface-tension=...`; zero disables either term.
- Viscosity is a constant-coefficient MAC Laplacian before pressure, using GPU
  ping-pong faces. Subcycling bounds `6 ν Δt / h² <= 0.9` per diffusion step.
  The saved pre-force velocity is unchanged, so FLIP receives the viscous delta.
  More than 32 required subcycles is rejected explicitly. This is **not** a full
  coupled viscous stress solve or a claim of realistic viscous buckling/coiling.
- A smoothed particle-volume occupancy field supplies simulation-scale curvature.
  Normalized-gradient/Hessian curvature works on this non-distance scalar; its raw
  Laplacian would not. Curvature is bounded to `±2/h` and suppressed next to solids
  until wetting/contact-line conditions are implemented.
- Both the Jacobi pressure equation and MAC pressure gradient use the same
  `σ κ` free-surface boundary value. This retains the divergence/residual identity.
  This is a full-cell V1 boundary, not a fractional-cell ghost-fluid discretization.
  An explicit capillary-wave timestep gate rejects unstable requested settings
  instead of silently reducing surface tension.
- New passes reuse the existing root/compute/resource abstractions, run entirely
  on the GPU, and carry PIX labels. One additional 16-byte-per-cell material buffer
  holds colour/curvature; simulation uniforms occupy a 512-byte aligned upload.
  No new queue waits or normal-frame particle readbacks are introduced.

The surface-pressure model follows the curvature boundary treatment in
[Bridson/Müller-Fischer's course notes, sections 1.6 and 6.4](https://www.cs.ubc.ca/~rbridson/fluidsimulation/fluids_notes.pdf).
The limitations of component-wise viscosity versus coupled free-surface stresses
are discussed by [Batty and Bridson (2008)](https://www.cs.ubc.ca/labs/imager/tr/2008/Batty_ViscousFluids/).
The implementation is original engine code, not copied middleware.

### Added validation

`test-fluid-material.ps1` exercises zero/nonzero/subcycled diffusion against an
analytic Fourier mode on the **actual GPU MAC faces**, verifies saved FLIP velocity
and non-increasing kinetic energy, and compares GPU sphere curvature against `2/r`.
The measured maximum diffusion/reference error is `1.171e-6 m/s`; maximum curvature
relative error is **0.286%**. Separate 180-frame inviscid, water, viscous, and
high-tension runs retain finite state and compatible pressure residuals.
CPU numerical tests check convex diffusion, curvature scaling/planes, and the
static Laplace-pressure equilibrium. This is not yet a droplet-oscillation or
wetting acceptance test.

The mesh-SDF CTest independently checks the glass shell's manifold edges, volume,
corner interiors and empty cavity. Render probes check ten wall/corner entry/exit
intersections per frame, including material classification and normal orientation.
They ignore the movable prism when it crosses the diagnostic line in the collision
fixture; actual rendered rays still refract through it normally.

This advances M12; emitters/drains/forces, foam/spray, sparse simulation, implicit
viscosity/multigrid, two-way coupling and the remaining optical/contact limitations
are still future work. The original whole-system performance target is unchanged.
