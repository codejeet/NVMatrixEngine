# Adaptive fluid integration map

This branch keeps its current solver and optical surface as the correctness baseline.
Checkpoint 1 supplies **importance/work-list infrastructure**; checkpoint 2 adds
optional active pressure scheduling and a coarse correction space. Neither is
an adaptive-resolution fluid solver. Requested LODs must not be confused with
executed resolution. Checkpoint 3 adds an independently transported passive bulk
inventory, not yet a grid/particle ownership handoff.
Checkpoint 4 adds [mass-preserving importance-driven particle resampling](FLUID_RESAMPLING.md).
It reduces actual sample counts and restores detail under impacts, but retains
particle coverage in every liquid cell and uniform MAC/surface resolution.

## Existing connections

| System | Current implementation | Adaptive connection |
| --- | --- | --- |
| DX12 resources / compute | `src/gpu_resources.h`, explicit root signatures and PSOs | Reuse buffers, transitions, UAV barriers, PIX events and shader compiler. |
| Frame scheduling | `Renderer::render`, one direct queue and frame fence; no frame graph or async compute | Record importance after fluid/surface updates. Consume counters only after the existing fence. No new waits. |
| Particles / bins | `fluid_system.*`, 80-byte APIC particles, histogram / hierarchical scan / scatter | Optional `FluidResampling` uses weighted rest mass, GPU recycled IDs, conservative merge/split and current safety guards. |
| MAC / pressure | Dense staggered faces; uniform or active Jacobi / optional two-level pressure correction; Jacobi density correction | Read projected velocities. Future global coarse inventory must precede particle deletion. |
| Surface | `fluid_surface.*`, anisotropic scalar reconstruction, compact active pages | Share the padded brick lattice (8 render cells = 4 MAC cells). Reuse canonical crossing masks, not an assumed distance bound. |
| Sparse addressing | Bounded linear page table and compact lists | Reuse direct spatial IDs; an unbounded hash allocator is unnecessary for the current room. |
| GPU scheduling | Surface `ExecuteIndirect` reconstruction | Also dispatch bounds from the active count; generate per-LOD dispatch and debug-draw arguments. |
| DXR | GPU AABBs, fixed-capacity procedural BLAS, renderer TLAS | Preserve topology handling and precise trilinear root intersection. No CPU brick-position readback. |
| Water optics | `transport.hlsl`, fluid intersection / medium / caustics | Preserve dielectric transport and photon ownership. Surface/motion importance is initially only an optical *prior*. |
| Temporal / reuse | Caustic EMA, DLSS RR/SR; optional RTXDI diffuse-primary PT reuse | No change to sampling, history validity, or guide buffers in the first checkpoint. Refractive caustic reservoirs are not implemented. |
| UI / profiling | RmlUi HUD; timestamp queries and bounded JSON reports | Add false-color brick views, freeze, independent timing/counters; retain existing benchmark column indices. |

## Checkpoint 1: implemented GPU importance and inspectable requested LODs

- Separate `FluidComplexity` subsystem, enabled with `--fluid-adaptive`.
- Current projected MAC velocity, free-surface masks and particle counts feed
  physics importance. Moving collider swept/wake bounds add immediate refinement.
- Physical-unit vorticity/velocity-gradient metrics, time-based smoothing,
  asymmetric promotion/demotion thresholds and one-brick padding.
- Visibility may raise surface/optical requests; it never removes physical detail.
- Separate physics and surface LOD lists, GPU counters and indirect arguments.
- F6 cycles inspection overlays; F7 freezes decisions (not simulation). Reset
  invalidates frozen decisions. Production simulation and surface spacing stay uniform.
- Surface-bounds work uses only the GPU active count, without changing any field samples.

The GPU view exports persistent 64-byte importance records, the compact active
union, eight requested-LOD partitions and eight dispatch arguments. The active
union drives an indirect wire-brick draw. **The physics/surface partitions do not
yet drive reduced-resolution simulation or reconstruction.** There is no new
screen-space variance buffer, adaptive path count or caustic-guiding feedback.

The current pool is only about two MAC cells deep. Almost all wet bricks are
therefore surface bricks or refinement padding. Requesting fine physics there
is intentional; the classifier must not invent a deep-water optimization in a
shallow scene. A moving ball adds local predicted swept/wake requests. Dry props
do not create fluid work, and off-center mesh SDF bounds are transformed correctly.

### Validation — RTX 5090, Windows Release, 2026-09-12

- 54 existing Node tests and six Windows CTests pass, including the shared
  HLSL/host hysteresis policy test and padded non-cubic MAC coverage.
- All six transport shader variants and all compute/debug kernels compile.
  Only existing Bullet/RmlUi external warnings remain.
- Fourteen bounded GPU cases pass: disabled/observer, rolling, empty, freeze,
  input/reset/view cycle, overlay, zero surface tension, sphere, thin sheet,
  full fluid/whitewater/DXR validation, fixed atomics/SER off, FG and ReSTIR PT.
- GPU validation checks unique and complete partitions, particle accounting,
  canonical surface counts, finite state, LOD ranges and indirect arguments.
  Empty fluid produces zero active importance work. Freeze holds decisions;
  reset overrides freeze. The rolling case marks 158 moving-solid/wake bricks.
- Importance generation plus padding/compaction averages **0.021–0.024 ms** in
  these live room cases (without the optional wire overlay). The subsystem uses
  729,920 logical buffer bytes including its validation staging buffer; this is
  not a measurement of committed heap alignment overhead.
- Surface bounds now launch 956 groups instead of 2,760 in the 96-frame inlet
  capture: 65.4% fewer groups for **that bounds pass**, not a 65.4% frame speedup.
  This checkpoint does not establish a statistically significant overall FPS gain.
- Static sphere/sheet before/after captures preserve all guide buffers exactly;
  radiance/atlas relative L1 differences are below `1e-6` (float-atomic ordering).
- The moving 96-frame inlet/orbit capture differs from the baseline by 0.479%
  raw radiance, 0.327% RR output, 1.287% water-caustic irradiance and 0.0044%
  reconstructed volume. Repeating the **unchanged baseline** differs by 0.565%,
  0.359%, 1.606% and 0.0448%, respectively. GPU particle bin/scatter order is not
  bit deterministic. The comparison excludes atlas history age from irradiance.
- The 320-frame whitewater temporal sequence passes the existing preservation
  gates against `water-temporal-performance-foam`: all phase brightness changes
  below 0.027%, raw temporal differences within 0.7%, RR differences within 2.1%.
  Top-down orbit and rolling retain the previous history behavior.
- D3D12 debug/GBV was explicitly attempted but unavailable (`0x887A002D`). No
  OS/driver settings were changed. The above GPU tests are engine-level invariants,
  not a claim of a clean debug-layer run.

Reproduce after building:

```powershell
.\test-fluid-complexity.ps1
.\test-water-temporal.ps1 -Name water-temporal-adaptive -Whitewater -Adaptive
```

```bash
node --test engine/*.test.mjs
node engine/validate-fluid-complexity.mjs RUNTIME adaptive-baseline adaptive-observe adaptive-baseline-repeat
node engine/validate-water-temporal.mjs RUNTIME water-temporal-adaptive water-temporal-performance-foam --preserve
```

The comparison commands require the saved local reference captures. Baseline
executable revision: `12c2c80`; generated executables, DLLs and captures remain
outside Git. See [the compact validation record](adaptive-validation.json).

## Remaining sequence and correctness gates

The [bulk inventory checkpoint](FLUID_BULK.md) now provides persistent rest volume
and momentum, conservative shared-face transport and source-ledger validation.
It remains a passive replica: local overfill and unsupported moving-solid volume
displacement are explicit blockers to using it as particle-free physical water.
No particles are removed or reseeded from this field.

The [pressure hierarchy checkpoint](FLUID_PRESSURE.md) brings forward a part of
step 3: an optional two-level Galerkin correction of the current fine equation,
plus tiled active Jacobi and operator validation. This prepares global pressure
coupling without pretending that coarse mass or coarse MAC velocity exists.
The uniform pressure path remains available and is still the default.

1. Turn the passive coarse inventory into pressure-compatible bulk support with
   geometric liquid/solid capacities and conservative ownership transfer, then
   particle-free bulk ownership and new fine reseeding. Weighted merging/splitting
   now preserves occupied-cell mass, but removing every sample from an interior
   cell would still delete that liquid from pressure classification.
2. Two MAC levels with conservative face-flux restriction/prolongation. Validate
   translation, rotation, pressure continuity and closed-container mass before LOD transitions.
3. Matched multilevel divergence/gradient and multigrid correction. Compare residuals,
   volume and energy with uniform projection; no independent fine-region pressure solves.
4. Surface resolution transitions with shared boundary samples and conservative DXR
   bounds. Validate thin sheets, roots, normals and caustics before skipping updates.
5. Screen optical error/variance/disocclusion, receiver photon importance and unbiased
   adaptive sampling. Extend existing reservoirs only with valid PDFs/shift mappings;
   do not relabel photon accumulation as ReSTIR caustic reuse.
6. Bidirectional importance feedback, conservative multirate integration, then timing
   budget control. Async overlap requires double-buffered simulation/surface ownership.

The initial classifier is a replaceable heuristic, not a physical error estimator.
No FPS improvement or adaptive mass conservation is implied by requested LOD counts.

## Algorithm references

- [Narrow Band FLIP (2016)](https://research-explorer.ista.ac.at/record/parent-system/pubrep/611)
  motivates retaining bulk grid fluid while concentrating particles at interfaces.
- [Extended Narrow Band FLIP (2018)](https://research-explorer.ista.ac.at/record/135)
  motivates smooth transitions between grid and particle representations.
- [SPGrid (2014)](https://orionquest.github.io/papers/SSPGASS/paper.html)
  informs sparse hierarchy design; its CPU virtual-memory implementation is not
  a drop-in DX12 allocator.
- [Adaptive octree liquids (2020)](https://cs.uwaterloo.ca/~c2batty/papers/Ando2020/Ando2020.pdf)
  informs future pressure and surface transitions, not an already implemented solver.
