# NVMatrixEngine raygen / spectral-caustics engine lab

The revised [CUDA / multiscale implementation contract](CUDA_FLUID.md) keeps
single-phase backend parity and bounded frame latency ahead of spacetime/two-phase
experiments. The optional CUDA build now runs the complete uniform-grid APIC/FLIP
substep pipeline through the existing renderer. Use `build-cuda.ps1` and
`Play CUDA Fluid Lab.cmd`; normal launchers retain DX12. Sparse/multiscale CUDA
solvers remain unfinished. `--fluid-cuda-graphs=on` (the CUDA launcher's default) reduces launch overhead;
`profile-cuda.ps1` records same-build raw latency, cold start and sampled VRAM.
The 60-FPS p99 gate is not met, and CUDA is not the normal default.
`--fluid-backend=cuda --fluid-cuda-graphs=on --fluid-cuda-pressure=mixed` opts into
the composed two-level MGPCG pressure experiment. `--fluid-cuda-pressure=fine`
uses the same convergence-qualified solver without coarsening; `uniform` retains
the cheaper fixed-Jacobi baseline. This does not yet adapt particle density or
surface reconstruction. The new pressure path is not a demonstrated speedup.
`Play CUDA Mixed Pressure Lab.cmd` opens this separate experiment; the regular
CUDA launcher retains the faster uniform solver.
Captured fine/mixed pressure also has
`--fluid-cuda-pressure-loop=conditional|unrolled`: the device-controlled loop
skips remaining CG iterations after convergence without changing the acceptance
limits. Conditional is the experimental graph path's default; `unrolled` is the
explicit compatibility/reference schedule. This scheduling optimization is not
adaptive particle/surface acceptance. Full conditional-loop sanitizer
qualification remains open on the tested Windows/Blackwell setup; see the
reproducer and measured latency caveats in [CUDA_FLUID.md](CUDA_FLUID.md).
Use `test-cuda-engine.ps1 -CudaGraphs -CudaPressure mixed` for full-engine checks,
and `profile-cuda.ps1 -Comparison pressure` for matched settings/timing evidence.

`--fluid-cuda-context=cig` additionally opts into CUDA-in-Graphics scheduling,
using the renderer's native queue while preserving resource/fence ownership.
It requires a CUDA 13.1+ build and runtime CIG support; unsupported requests fail
explicitly. `primary` remains the default/reference. Use
`test-cuda-engine.ps1 -CudaContext cig` and `profile-cuda.ps1 -Context cig` to
validate/measure it independently of pressure mode. See the CIG integration and
timing checkpoint in [CUDA_FLUID.md](CUDA_FLUID.md).

Experimental rewrite on `engine/ser-spectral-caustics`. The shipped game remains in `native/`, and its portable release is unchanged. This directory builds **NVMatrixFluidLab.exe** into a different Windows build directory. It reuses the camera value type, Streamline RR integration, Bullet game mechanics, RmlUi DX12 backend and audio; device/AS management, shader tables, transport shaders, receiver atlas and synchronization are new.

**Status: runnable spectral engine with an optional NVIDIA ReSTIR PT integration.**
`Play ReSTIR PT Lab.cmd` / `--restir-pt` enables genuine multi-bounce diffuse path
resampling from opaque primary surfaces, using the pinned RTXDI PT kernels.
The regular launcher retains the cheaper two-bounce baseline. Glass/water camera
prefixes remain deterministically split; GI reuse behind refractive primaries and
unified ReSTIR DI are not implemented. See [scope, current research and integration
details](FLUID_RENDER_RESEARCH.md). All traversal remains in DXR ray-generation
shaders; DLSS-RR is still the sole screen-space denoiser/upscaler.

## Build and run

The new GPU liquid subsystem is opt-in: **`Play Fluid Lab.cmd`** or
`NVMatrixFluidLab.exe --fluid`. See [the liquid integration map and milestone
checkpoints](FLUID_IMPLEMENTATION.md). It now reconstructs a continuous anisotropic
scalar field and renders it through procedural DXR, water optics, lasers, caustics
and DLSS-RR/SR. The default optical chamber and mainline game remain unchanged.
`I` inspects the pool (`--fluid-view` starts there); `--fluid-flip` selects FLIP/PIC;
`P` pauses, `.` single-steps, `B` resets, `V/G` control solver overlays, and `N`
cycles surface normals/brick IDs/motion diagnostics. `--fluid-solver-only` retains
the old debug-only test path. `--quality=quality|balanced|performance` selects the
existing DLSS internal-resolution mode; Quality remains the default.

The fluid launcher now opens a **room-wide shallow pool**, with a wall inlet and
bounded GPU foam/bubbles. Click **SPEW WALL WATER**, press **T**, or press **E** near
the wall valve to toggle it. **I** inspects the inlet, **B** restores the initial
pool and closes the valve. Filling adds actual APIC particles; the inlet closes
at the 500k default capacity instead of silently recycling water. Start it running with
`--fluid-emitter`; `--no-whitewater` disables the secondary phase for profiling.
See [the model and real-time limits](ROOM_WATER.md).

Interactive launches now use a **120-degree diagonal equisolid fisheye**. Esc
opens lens/FOV, first-person, lighting preset, ball flashlight, particle capacity
(up to 1M), MAC spacing (10–24 cm), and simulation-rate (60–180 Hz) controls.
Applying liquid quality resets the pool; merely dragging a slider does not.
Tab switches views, Ctrl dives, and E boards/exits the room's powered boat;
W/S applies thrust and A/D steers. The normal-lens option retains DLSS Frame
Generation; fisheye presentation pauses FG while keeping Ray Reconstruction.
See [camera and watercraft implementation notes](EXPERIENCE.md).

`Play Adaptive Fluid Lab.cmd` / `--fluid-room --fluid-resample` enables the
[adaptive architecture checkpoint](ADAPTIVE_IMPLEMENTATION.md): GPU importance,
swept/wake refinement, hysteresis, padded physics/surface LOD work lists and metrics.
**F6** cycles x-ray brick overlays; **F7** freezes decisions without freezing water.
Blue means low importance/coarse request; red means high importance/fine request.
Optional [conservative particle resampling](FLUID_RESAMPLING.md) now consumes
physics requests: dense calm cells can merge samples; disturbances restore them.
Actual samples and conserved rest mass are reported separately. MAC spacing,
surface resolution and ray budgets remain uniform. `--fluid-adaptive` alone
retains the observer-only mode for comparison.
The ordinary launcher does not pay classifier overhead. `test-fluid-complexity.ps1`
validates list coverage, counters, reset/freeze, empty domains and renderer compatibility.

`Play Adaptive Optics Lab.cmd` / `--adaptive-rays` adds the opt-in
[adaptive optical sampling subsystem](ADAPTIVE_OPTICS.md):
raw-radiance variance, geometry/motion confidence and actual photon receiver
importance allocate fresh light/path samples. F4 cycles optical diagnostics and
F5 freezes the sampling decisions. Optical error feeds back into fluid surface
importance without lowering physics fidelity. `--optical-reference` supplies the
matched-cap uniform reference; ordinary launchers keep the existing sample budget.

`--fluid-surface-lod` adds the opt-in [two-level canonical surface sampling
checkpoint](FLUID_SURFACE_LOD.md). It consumes reconstruction error and optical
priors independently of physics LOD. `--fluid-surface-lod-view --fluid-view`
shows actual fine/coarse sampling; `--fluid-surface-lod-fine` supplies the
same-pipeline fine reference. This is not enabled in normal launchers: smooth
analytic fixtures coarsen, but the current gameplay water conservatively stays
fine. Compressed surface storage and a useful gameplay cost reduction remain
open requirements.

The next [pressure hierarchy checkpoint](FLUID_PRESSURE.md) adds optional GPU
active-cell scheduling (`--fluid-pressure=active`) and a two-level Galerkin
correction (`--fluid-pressure=multigrid`). Both solve the existing fine-grid
equation; neither deletes particles or changes MAC resolution. The default is
still `--fluid-pressure=uniform`. Use `--fluid-pressure-iterations=120` for
uniform/active Jacobi, `--fluid-pressure-cycles=3` for multigrid, and bounded
`--fluid-pressure-validate` runs to check the assembled operator and work lists.
The expanded HUD and JSON report expose pressure GPU time separately.

`Play Bulk Inventory Lab.cmd` / `--fluid-bulk` adds the next
[persistent volume/momentum checkpoint](FLUID_BULK.md). **F9** cycles coarse
inventory/speed inspection; magenta volume cells expose local overfill. This is
an independently transported **passive replica**, not additional water or an
authoritative adaptive solver. It preserves global mass/momentum accounting but
still needs geometric capacity and pressure-compatible coupling before
particle-free bulk ownership. Ordinary launchers remain unchanged.

`--fluid-cut-time-centered` adds opt-in [time-centered moving-solid pressure
geometry](FLUID_CUT_TIME.md): shared face openings are integrated over each
simulation interval, while collision and liquid capacity use the actual endpoint.
Combine with `--fluid-bulk-bounded` to inspect its effect on conservative coarse
transport. This is a pressure/transport prerequisite, not completed Narrow Band
FLIP; closing-cell residual inventory and flowing grid/particle ownership remain
open. The normal play presets do not enable it.

`--fluid-bulk-pressure` connects [filled bulk interiors to the actual MAC
pressure solve](FLUID_BULK_PRESSURE.md), supplies missing grid velocity and
capillary occupancy, and implies bounded transport plus time-centered geometry.
Particle mass and the reconstructed free surface remain particle-owned. This is
an opt-in coupling prerequisite, not completed flowing Narrow Band FLIP or a
faster gameplay preset. Strict visual-preservation checks still flag small
brightness/raw-temporal differences; the linked evidence records these failures.

`--fluid-bulk-implicit` opts into [implicit conservative coarse transport](FLUID_BULK_IMPLICIT.md).
It removes the explicit donor cap and supplies temporary pressure escape paths
for partially filled cells covered by moving solids. Do not combine it with
`--fluid-bulk-bounded` or `--fluid-bulk-pressure`: it enables its own pressure
support and replaces the explicit phase limiter. It preserves positive mass and
momentum accounting, but can still overfill interface cells. This is not bounded
VOF, particle-free flowing ownership, or a default gameplay preset.

`--fluid-bulk-air` enables the experimental [coupled air-carrier capacity
solve](FLUID_CARRIER_PROJECTION.md) on top of implicit transport. It preserves
solid and existing liquid/interface fluxes and audits every substep. Focused
hardware fixtures pass, but the moving-room acceptance still fails on a genuine
protected-component overfill. This is a development path, **not a playable
replacement or completed bounded transport**; normal launchers do not enable it.

`--fluid-bulk-coupled` adds the [shared capacity/MAC correction](FLUID_CAPACITY_MAC.md)
before G2P, with FP64 bounded inventory and independent physical/phase audits.
Its inner multigrid work uses one cycle per outer iteration; use
`--fluid-capacity-cycles=4` for the prior scheduling reference (valid range 1–4).
This only changes inner work, never convergence tolerances. The subsystem remains
opt-in; normal launchers do not enable it, and flowing grid/particle ownership,
sparse physical storage and broad temporal-quality acceptance remain open.

`--fluid-owned-particles` connects [precise particle/grid ownership records](FLUID_FLOWING_OWNERSHIP.md)
to live births, velocity updates and fractional-mass P2G/binning. It is a
development integration mode, not yet particle-free flowing bulk or a speed
preset. Do not combine it with the legacy resampler, dormant interior or bulk
replica. It leaves the normal launchers unchanged.

The flowing-band policy in the same document now drives actual GPU exchange and
transport tests, including moving interiors, protected boundaries and blocked
restoration. It is not yet called by the playable simulation; no new faster
gameplay preset is implied.

`--fluid-bulk-projected` adds the [canonical pressure-flux connection](FLUID_BULK_PROJECTED.md):
FP64 shared flux restriction, moving-solid open capacities and audited momentum
sources. It remains passive and reports local excess instead of hiding it;
bounded liquid support and flowing grid/particle ownership are still required.

`--fluid-bulk-capacity` adds [bounded source admission](FLUID_SOURCE_ADMISSION.md).
Initial/inlet volume fits into available cells; unplaced requests retain an
explicit mass/momentum ledger. This does not hide later advection overfill or
replace the particle-owned simulation/rendering surface.

`--fluid-bulk-bounded` additionally enables [shared phase-flux receiver limits](FLUID_PHASE_FLUX.md).
Both remain opt-in replicas; moving-solid closure and flowing grid/particle
ownership are not yet complete. Neither switch is a faster gameplay preset.

`--fluid-interior` opts into [authoritative dormant coarse cells](FLUID_INTERIOR.md):
resting deep cells can replace particle samples while retaining their density,
global pressure participation and continuous DXR liquid coverage. Moving solids
restore particles. **F10** forces fine detail; **G** adds a cyan ownership view.
This remains an APIC-only resting-interior milestone, not a flowing multiresolution
MAC solver. The normal launchers and portable release are unchanged.
The moving-solid peak-density regression is addressed by [substep-aligned SDF
boundaries and GPU error-triggered density repair](FLUID_BOUNDARIES.md). This mode
remains experimental, not a production or faster-render preset.

`--fluid-mac` adds [coupled 2:1 MAC pressure and velocity](FLUID_MAC.md), consuming
importance while retaining full-resolution surfaces and solid contacts. **F10**
forces fine resolution; **F11** displays actual coarse cells and fine/coarse
boundaries. `test-fluid-mac.ps1` checks conservation and the independently
assembled mixed operator. This opt-in increment keeps the fine transfer cache;
it is not yet flowing particle-free bulk or a promised performance preset.

`--fluid-mac-solver=multigrid` selects the [mixed-MAC MGPCG solver](FLUID_MAC_MULTIGRID.md)
with precise pressure/residual evaluation and GPU convergence control. Small
coarse V-cycles and paired density updates reduce dispatch overhead; the default
fluid path is unchanged. This remains an opt-in accuracy/architecture milestone.

`--fluid-sparse-work` adds [compact GPU simulation execution](FLUID_WORK.md)
on either pressure solver: quadratic-support MAC-face tiles and coupled density
tiles replace empty-volume gather/iteration work. **F12** shows executed tiles.
This changes scheduling, not grid resolution, material optics or the pressure
operator. `test-fluid-work.ps1` compares it against uniform execution.

For the current live room/inlet/foam scene, use `profile-room.ps1` or the
interleaved original/candidate `profile-room-paired.ps1`, with FG off. See the
[room performance pass and reproduction steps](ROOM_PERFORMANCE.md); the older
`profile-fluid.ps1` benchmarks the smaller pit and is not a room-water result.

`--fluid-pit` retains the previous closed **glass wall shell**, neon rim and
diffuse caustic-receiving floor for regression tests. This is dielectric DXR geometry, not alpha blending.
`--fluid-viscosity=<m²/s>` and `--fluid-surface-tension=<N/m>` control the initial
viscosity/capillary model (water defaults `0.000001` and `0.0728`). Zero disables
either effect. Viscosity subcycles are stability-bounded; unsupported capillary
timesteps fail explicitly. These V1 models do not yet include accurate wetting or
the coupled viscous free-surface stresses needed for realistic honey coiling.

From Windows PowerShell in this directory:

```powershell
.\setup.ps1
.\build.ps1
& "$env:LOCALAPPDATA/NVMatrixEngine/build/bin/Release/NVMatrixFluidLab.exe"
```

`Play Lab.cmd` now opens a **playable neon test chamber**, not the original black-prism fixture. Roll as a glass ball, manipulate a real triangular prism and throw luminous rigid bodies. Put green spectral flux into the outlined receiver for two seconds, then roll through the cyan portal. The original six-level game and portable release are unchanged.

The chamber now also has two **wavelength-selectable lasers** and a **refractive ripple tank** to the right of the source. Laser spots and water caustics share the spectral photon simulation; side-visible beams use bounded single scattering in light haze. See [the optical model, measured data, units and real-time limits](WATER_OPTICS.md). The ordinary chamber retains its static optical snapshot; `--fluid` replaces it with GPU-simulated procedural liquid.

| Control | Action |
| --- | --- |
| WASD / Space | Roll / grounded jump |
| Right drag (or left drag when empty-handed) / wheel | Orbit / zoom |
| E / left click or X while holding | Grab or release / throw |
| F near the prism | Lock into the cradle and transition to the top-down camera |
| Drag / wheel or Q, C in precision mode | Move on the optical plane / rotate |
| Shift / Enter or F / Escape while tuning | Smaller adjustments / save / cancel and restore |
| U or H / Escape / R | Minimal details and hints / pause and music volume / restart |
| L | Cycle both lasers: green 532 nm → blue 450 nm → red 638 nm |
| F8 | DLSS Frame Generation: toggle 2× / Off |

RmlUi draws the FPS counter in the top-right corner independently of expanded hints. The same Bullet `Game` implementation, RmlUi DX12 backend and quiet music/event sounds as the native game are reused. Movement is fixed-step with collision-aware camera and tuning constraints. This is one gameplay/renderer integration test, not the completed campaign migration.

`--fixture` retains the original isolated optical scene: drag to orbit, wheel to rotate, R to restore angle, Escape to exit. `check.ps1` selects it explicitly so backend/atlas comparisons remain controlled.

After building, `Play Lab.cmd` launches it from Windows Explorer. [Validation results](VALIDATION.md) document the current tested behavior and limitations.

The executable locates its assets using its loaded Windows module path, independently of the command-line filename or working directory. Interactive failures show an error dialog instead of silently exiting. `test-launch.ps1` covers basename/relative/foreign-directory launches and the actual interactive launcher.

The setup uses checksum-pinned official downloads in the ignored `shared/.deps/` cache:

- DirectX Agility SDK **1.619.5**, app-local retail runtime (no experimental OS flags).
- DXC **1.9.2607** for SM 6.9 and payload access qualifiers.
- Official WinPixEventRuntime **1.0.240308001** for command-list event markers.
- Existing Streamline **2.12.0** and pinned NVAPI.
- RTXDI-Library `f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`, matching RTXDI reference revision `a6efab966b7c3b272da0461578eb56ac61c7cbff`.
- CIE 1931 2-degree observer data at 1 nm spacing, DOI [10.25039/CIE.DS.xvudnb9b](https://cie.co.at/datatable/cie-1931-colour-matching-functions-2-degree-observer). The original dataset remains intact in the dependency cache; the renderer loads 380–780 nm. See upstream metadata for its attribution/license.

No DLL downloads, drivers, credentials, generated captures or build directories are committed. This lab is not packaged for redistribution; complete SDK notices/attributions are a release acceptance item.

## DLSS Frame Generation and Reflex

Interactive launches default to **2× Frame Generation** when Streamline reports
support; unsupported systems keep RR/SR without FG. Escape opens the RmlUi setting
for Off / 2× / 3× / 4×, limited by the queried GPU/driver capability. F8 is the
quick Off/2× toggle. `--frame-gen=off|auto|2|3|4` overrides the launch setting.
Bounded validation defaults to Off; request FG explicitly to test it.

The experimental renderer's `src/streamline.*` wrapper loads the signed, pinned
production FG/Reflex/PCL plugins alongside RR. A single frame token covers Reflex
sleep, simulation, rendering, RR and Present. FG consumes internal-resolution
device depth and dense motion vectors plus full-output-resolution tonemapped
HUD-less color and a separate premultiplied RmlUi layer. NVIDIA performs the
interpolation/UI recomposition at intercepted Present, not via a second RR call.
Its input-completion fence protects reused textures and resize/free operations.
See the [pinned NVIDIA integration guide](https://github.com/NVIDIAGameWorks/Streamline/blob/v2.12.0/docs/ProgrammingGuideDLSS_G.md).

FG suspends during pause/completion menus, minimization, solver debug overlays
and presentation changes. Two real startup frames establish history. Selecting
Off unloads the FG presentation hooks and recreates the swapchain, eliminating
the disabled plugin's offscreen-copy overhead; temporary menu suspension retains
resources. Reconfiguration resets RR history. Reflex remains on when supported.
The FPS HUD distinguishes **presented FPS / rendered FPS**, using actual SDK
presentation counts rather than multiplying by the requested setting. FG does
not increase simulation frequency or reduce path-tracing cost.

GPU/driver/OS/HAGS support is queried; Windows settings are never modified.
Explicit unsupported multipliers fail with a diagnostic. The JSON report adds
`frameGeneration` capability, runtime status, actual presents and token/marker
counts. `test-frame-gen.ps1` checks Off, 2×, 3×, 4×, pause/F8/resize and procedural
liquid rendering. `--dred` enables process-local GPU fault breadcrumbs; it does
not require installing the optional Windows debug layer.

Primary-surface motion guides retain the existing limitation for multilayer
reflections/refractions and disoccluded water; FG does not solve that optical
motion ambiguity. Raw captures contain **real engine frames**, not generated
display frames. Display-level pacing and interpolation artifacts still require
interactive inspection or external frame capture.

## Implemented frame

`profile-fluid.ps1 -Tag current -Repeats 2` measures the water close-up and
gameplay camera at 1920×1080 / Balanced with 100k particles, FG explicitly Off,
and the normal HUD. Each run advances 720 fixed simulation substeps over 360
real frames, discarding 32 warmup frames. It refuses to run beside an open lab.
Reports append `fluidSimulation`, `fluidReconstruction`, and `fluidBlas` to
`timingColumns` / `samplesMs` / `medianMs`; the original first six columns retain
their indexes. GPU medians are per pass, and the CPU renderer-frame interval
includes submission/presentation/fence overhead, not the outer game update.
These unpaced raw-frame measurements are not FG display FPS or a guarantee of
interactive refresh rate. [Validation](VALIDATION.md) records the optimization
comparison and unchanged quality settings.

```text
Optional fluid: APIC/FLIP -> sparse anisotropic scalar field -> procedural BLAS/TLAS
  -> Clear photon sums (stationary and fluid-owned)
  -> Beam DispatchRays: two bounded deterministic laser segment trees
  -> Photon DispatchRays: prism / water / laser transport -> texture-space XYZ splats
  -> UAV barrier -> receiver-space EMA (independent animated-fluid history)
  -> Camera DispatchRays: raw noisy diffuse lighting + primary guides
  -> optional ReSTIR PT temporal / spatial DispatchRays (opaque-primary indirect)
  -> UAV barrier -> caustics lookup × diffuse albedo / pi + noisy lighting
  -> transitions -> one DLSS-RR/SR evaluation -> tonemap
  -> separate premultiplied RmlUi layer -> optional DLSS-FG at Present
```

All traversal is in the raygen library. Closest-hit shaders only return a **16-byte payload**; they never trace secondary rays. Primary guides are finished before the camera's secondary-bounce loop. Photon origin/direction differentials are packed as fp16 pairs in raygen continuation state, rather than bloating the hit payload. This is not a claim that the entire continuation fits in 40 bytes; the queried pipeline stack is reported separately.

The fixture has a watertight equilateral triangular prism, four active planar receiver charts, one collimated spectral source aimed through the prism aperture, and two RGB point lights for the diffuse baseline. Photon aperture coordinates and wavelength use three digital-shifted Sobol dimensions. Each photon carries one wavelength, with Cauchy dispersion in micrometres, exact dielectric Fresnel, TIR, Beer–Lambert attenuation and stochastic internal reflections. The original collimator excludes entrance reflection from its caustic path class; new laser/water sources include it. Photon paths now have a twelve-event budget; truncations are counted.

Photon differentials use analytic planar-intersection and Snell derivatives. Elliptical Gaussian splats are discretely normalized, including chart edges, and divided by world-space texel area when resolved to irradiance. Large footprints are bounded and counted. Atlas accumulation is **positive XYZ**, avoiding premature clipping of out-of-gamut spectral RGB; conversion happens at shading. The laser/water integration makes the existing equal-energy normalization an explicit fixed photometric exposure; [WATER_OPTICS.md](WATER_OPTICS.md) documents radiant watts, CIE conversion, internal luminance units and limits. Tone-mapped display brightness is not an absolute photometric calibration.

The playable scene adds a glass-avatar mask (`0x04`) and water mask (`0x08`) alongside prism optics (`0x02`). Broad-source segment A targets their union (`0x0E`) with an opaque-blocker visibility check; subsequent transport and monochromatic laser rays use `0xFF`, including opaque receivers within water. Only charted receivers accept splats. The sphere uses a triangle surface with reconstructed smooth normals and curved-normal differentials; water uses a closed triangulated ripple surface with analytic normal derivatives. Critical-angle derivatives are bounded before fp16 packing, and the minimum reconstruction footprint survives large-footprint shrinking. Caps and truncated transport are reported. The atlas now has a fifth 256² chart for the submerged tank floor.

Camera-visible glass is now a bounded, Fresnel-split **specular prefix**: transmission, entrance reflection, repeated internal reflection, TIR and Beer–Lambert attenuation are traced before any diffuse event. It observes the caustic atlas on visible/refracted/reflected receivers. After a diffuse event, camera paths still terminate at glass. This fixes the former blanket primary-ray termination without retracing the photon-owned light-to-glass-to-receiver segment. Secondary diffuse vertices can also observe deposited caustic light.

The camera currently uses the **550 nm reference IOR for broadband RGB appearance**, not a full spectral camera estimator. The photon caustics remain continuously wavelength-resolved. Specular branches are bounded to 10 interfaces / 48 visits and a 0.001 radiance-weight cutoff; truncations are reported. The [dielectric radiance transport model](https://pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF) supplies exact Fresnel/TIR and the transmission eta-squared factor. These bounded branches are not a claim of converged infinite-bounce optics or ReSTIR PT.

The playable room adds emissive neon geometry, a polished-floor specular lobe, two moving cube area emitters sampled with their face/area PDFs, and un-refracted source illumination with glass-aware visibility. The target measures **raw photon flux in 510–570 nm** crossing its physical rectangle, before splatting/EMA and independently of camera position or prism angle. The game charges at 0.100 simulated radiant watts for two seconds. This is a synthetic optical scene, not a calibrated physical measurement.

The atlas uses a running average during startup, then a 32-frame EMA. It is **not assumed converged after movement**. Source/optical changes and optical-object/blocker/receiver motion reset the atlas, independently of DLSS. A conservative bounding-sphere displacement test ignores less than 0.1 mm of solver jitter; movement is measured from the last invalidation pose, so slow adjustments cannot drift indefinitely without resetting. This is a deliberate sub-texel tolerance, not an exact invariance near a critical angle. TLAS updates and dynamic motion guides still use exact rendered transforms. The untextured ball keeps a fixed tessellation orientation while its Bullet body spins.

**RR resets only for startup, resize and explicit gameplay discontinuities** (restart, teleport, docking/cancel), not ordinary physics or orbit. The previous rendered camera/object transforms provide unjittered motion guides. DLSS receives the projected-image jitter offset (the negative of the ray's sample displacement), and specular hit distance measures the first reflected segment from the primary surface, not a later terminal segment. It is not a complete specular-motion solution for refracted multilayer glass. Photon transport invalidation can feed future reservoirs without forcing a global screen-space reset. No intermediate screen-space denoiser or temporal radiance filter is added.

Interactive rendering uses VSync with a one-frame-latency waitable swapchain, waiting before simulation and waking for window messages. The HUD performs one layout update per frame, only changes visibility/progress properties when necessary, and refreshes FPS at 4 Hz. Interactive sessions no longer accumulate an unbounded timing-sample vector. Bounded validation remains unpaced and retains raw samples.

Orbit uses foreground [Windows raw mouse input](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input) and integrates accumulated displacement once per rendered frame with a short **6 ms** exponential response. This trades a few milliseconds of response time for smoother motion between mouse packets; it is not FPS smoothing or camera prediction. Legacy pointer input remains for absolute devices, registration failure, precision dragging and menus. Displacement is not doubled by legacy move messages, pause/focus loss clears pending movement, and pitch-limit reversal does not inherit a backlog. The message queue is checked again immediately after the display wake, before sampling the camera. The OS window caption is set once; the always-visible HUD owns live FPS, avoiding periodic caption repaints.

The playable lab also opts into a **30 cm sphere-swept follow camera**. Obstructions shorten its distance immediately; clearance recovery uses a 100 ms exponential response capped at 8 m/s, preventing wall-edge pops while rolling. Only camera distance is smoothed—not the interpolated avatar or orbit angles. Recovery advances once per rendered frame, resets on explicit cuts/resize, and supplies the same camera to DLSS history and mouse-plane picking. The shared `Game::camera` API keeps the original point-ray behavior for default/mainline callers.

## Capabilities and controls

| Control | Behavior |
| --- | --- |
| `--ser=auto` | Conservative plain `TraceRay` in raygen for the current split-glass workload; hardware support alone does not opt into reordering |
| `--ser=dxr` | Require standard HitObject; query whether hardware actually reorders |
| `--ser=dxr-off` | Same standard HitObject library, reordering disabled for A/B tests |
| `--ser=nvapi` | Require NVAPI HitObject; query hardware reordering |
| `--ser=off` | Plain DXR `TraceRay`, still in raygen |
| `--atomics=auto` / `float` / `fixed` | Query NVAPI fp32 atomics or use compiled saturating uint accumulation |

Forced unsupported features fail clearly. Explicit standard/NVAPI SER remains available; auto no longer pays HitObject/continuation-reordering overhead on every short visibility ray in the current glass shader. This does not switch back to compute traversal. The fixed-point scale uses a conservative CIE/total-flux bound; saturating compare/exchange additionally detects overflow instead of wrapping. Capability queries, selected paths, stack size, photon counters and bounded-run timing samples are saved in reports. Ampere/Turing execution still needs real hardware testing; selecting their fallback on a 5090 is not equivalent to that test.

Standard SER does not require the complete DXR 1.2 tier if SM 6.9 and the necessary raytracing tier are present. The engine checks SM and `OPTIONS22.ShaderExecutionReorderingActuallyReorders` separately, following the [DirectX SER specification](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/Raytracing.md#ser-device-support).

## Validation

`test-play.ps1` runs the playable initial/solved/blocked/top-down cases plus real window-message movement, jump, mouse/scroll tuning, grab/throw, pause/HUD and portal-completion checks. `ctest -C Release` includes the CPU-only lab physics/interaction test. Bounded `--pose=solved|blocked|tuning` controls are validation-only and unavailable during normal play.

`test-temporal.ps1` runs a 256-frame static → prism motion → settle → orbit → settle regression. `node engine/validate-temporal.mjs <capture-directory>` checks finite guides, atlas invalidation/convergence, independent RR resets, motion-vector jitter exclusion, image response to movement and settled temporal stability. The extra phase captures are diagnostic, not timing measurements. Reports distinguish `historyResets` (atlas) from `rrHistoryResets` (global reconstruction).

`test-orbit.ps1` provides a deterministic 300-frame playable orbit at 1080p, with real DLSS and dense camera motion guides. CTest's `lab_orbit_input` tests the actual C++ input integrator at 60–360 Hz, irregular frame partitions, reset/clamp behavior, and 125 Hz device packets on a 240 Hz render cadence. `test-play.ps1` also injects a real right-button drag and verifies that the complete requested angle is applied once. These checks do not replace a subjective mouse/monitor test.

`test-rolling.ps1` runs 300 rolling/reversal/jump frames at 1080p and requires uninterrupted RR history. CTest's `lab_rolling_camera` exercises the actual Bullet world and camera at 60/144/240 Hz, reproduces the old wall-edge pop, and checks collision-safe retraction, bounded recovery, interpolated-ball anchoring, variable cadence, jumping, pause/restart and overhead transitions. `validate-captures.mjs` also checks rolling motion guides. These are motion/correctness regressions, not FPS benchmarks.

`test-water.ps1` and `node engine/validate-water.mjs <capture-directory>` exercise wavelength, flat-interface, disabled-water/laser/haze and fixed-atomics controls. CTest includes `lab_water_optics`, checking published IAPWS reference indices, measured absorption units and watertight geometry. Photon reports include a radiant-power ledger; the image validator compares focusing on raw receiver irradiance, independently of albedo and DLSS.

`test-fluid.ps1` validates the solver independently. `test-fluid-render.ps1` adds
analytic procedural surfaces, animated liquid, scene-SDF collisions, ray backend
variants and real inspection/debug/pause/reset input. `node --test engine/*.test.mjs`
runs the optical and fluid numerical tests. See the liquid map for measured
performance and outstanding middleware features; this is not yet the whole spec.
`test-fluid-material.ps1` adds GPU Fourier-decay/FLIP-delta checks, analytic sphere
curvature, and inviscid/water/viscous/high-tension runs. Liquid render validation
also probes entry/exit through the glass walls and continuous corner rail.

```powershell
.\check.ps1 -Name standard
.\check.ps1 -Name fixed -Atomics fixed
.\check.ps1 -Name no-reorder -Ser dxr-off
.\check.ps1 -Name nvapi -Ser nvapi
.\check.ps1 -Name fallback -Ser off -Atomics fixed
.\check.ps1 -Name moving -Animate
```

From the repository root, with Node available:

```bash
node --test engine/optics.test.mjs
node engine/validate-captures.mjs /path/to/standard /path/to/fixed
node engine/validate-captures.mjs /path/to/moving
```

Checks are sequential and refuse to overlap another game/lab process. Automated fixtures ignore desktop mouse/keyboard traffic and record their angle/transport hash; the validator rejects mismatched scenes before comparing them. Each process has a 60-second cap and each GPU fence a 15-second timeout. Reports use 32 warmup frames, per-pass GPU queries, and unsmoothed samples. Raw captures contain all RR inputs/output and the atlas; the validator checks every scalar for finiteness, atlas positivity/bounds/age, dynamic motion, and optional float/fixed/SER atlas agreement. CPU tests independently check Snell/TIR, Fresnel, Beer–Lambert, analytic differentials against finite differences, splat normalization and the CIE fixed-point bound.

`-DebugLayer` requires Windows Graphics Tools and fails if unavailable. It was unavailable in the initial Windows test environment; a clean run without it is not a debug-layer validation pass. The lab is deliberately single-queue and one frame in flight until correctness is established.

## Integration corrections and milestones

See [MILESTONES.md](MILESTONES.md) for acceptance gates and remaining work. Important corrections to the proposed stack:

- [Streamline 2.12 RR](https://github.com/NVIDIA-RTX/Streamline/blob/v2.12.0/docs/ProgrammingGuideDLSS_RR.md) accepts a combined **noisy HDR color input** plus separate diffuse/specular albedo guides, normals/roughness, motion, depth and specular-hit-distance or specular-motion information. Do not invent a diffuse/specular color split or blindly demodulate the combined color. RR itself performs reconstruction/upscaling; no second SR evaluation is used.
- Caustics stay in radiance, never albedo. For an irradiance atlas and a Lambertian receiver, the conversion includes **1/pi**. The composite is a real compute workload and naturally provides the post-raygen compute handoff measured in the earlier renderer.
- Caustic illumination can move across a stationary surface. Geometry motion vectors do not encode that lighting motion; invalidate the texture-space estimator, but let RR consume the changed noisy radiance without globally discarding history every moving frame. Rapid light changes, disocclusions and multilayer glass can still show reconstruction lag and require broader visual testing.
- [RTXDI includes ReSTIR PT](https://github.com/NVIDIA-RTX/RTXDI/blob/a6efab966b7c3b272da0461578eb56ac61c7cbff/Doc/RestirPT.md). Use its maintained bridge, hybrid-shift and reservoir interfaces for the implementation, not a home-grown reservoir approximation labelled GRIS.
- FG is integrated through Streamline's swapchain/present lifecycle and required HUD-less/motion/depth tags, not just inserted as an arbitrary compute pass. Follow the [FG guide](https://github.com/NVIDIA-RTX/Streamline/blob/v2.12.0/docs/ProgrammingGuideDLSS_G.md) and Reflex lifecycle before enabling it.
