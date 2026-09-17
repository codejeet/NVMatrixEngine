# Adaptive checkpoint 2: pressure scheduling and coarse correction

This is an opt-in solver milestone, not narrow-band particle deletion or a
multiresolution MAC grid. The existing uniform solver remains the default.
Pressure hierarchy work was brought forward because the eventual grid/particle
handoff needs global pressure coupling. Persistent bulk mass and momentum storage
must still precede any removal of deep-water particles.

## Engine connections

- `FluidSystem::projectGrid` assembles its existing fine pressure stencil, then
  delegates only the pressure iteration to `FluidPressure`. Projection, moving
  solid velocities, capillary ghosts and divergence measurement stay unchanged.
- `fluid_pressure.*` owns GPU work lists, indirect arguments, coarse coefficients,
  corrections and timing queries. It reuses `gpu::Buffer`, barriers, PIX events,
  the shader compiler and the existing post-submission frame fence. No extra
  queue waits, per-particle CPU work or CPU scheduling readback are added.
- `pressure-hierarchy.hlsl` compacts solvable liquid cells and occupied pressure
  tiles. These are separate from the surface/importance bricks: every solvable
  liquid cell participates, including low-importance or hidden regions.
- Fine pressure/residual validation snapshots are captured before density
  correction reuses the stencil and pressure scratch buffers. Full snapshots are
  allocated and copied only for requested bounded validation runs. Ordinary
  readback is timestamps plus 32 bytes of counters/residual maxima.
- Rendering consumes the same reconstructed fluid surface through existing DXR,
  dielectric transport, caustics, whitewater and DLSS. No ray budgets, filtering,
  particle counts, density-correction sweeps or optical precision are reduced.

## Solvers

| Mode | Executed work |
| --- | --- |
| `uniform` (default) | Original full-grid Jacobi; no new pressure-module allocation. |
| `active` | Same Jacobi equation and sweep count on occupied 8×4×4 tiles; two exact sweeps per dispatch and a single-cell-list fallback for an odd final sweep. |
| `multigrid` | Two-level aggregation/Galerkin correction of the original fine equation, with weighted fine and coarse smoothing. |

The tiled kernel computes the first sweep for the tile **and its halo** from the
same global input iterate. After a group barrier it computes the second sweep
from those shared values. Halo computation itself reads global neighbors, so
cross-tile dependencies are not truncated. This is not block Gauss–Seidel or an
approximation using stale halo values. Partial boundary tiles and solids are
masked explicitly; only active tiles are dispatched.

For multigrid, constant prolongation `P` assigns one coarse correction to each
active child of a 2³ aggregate; restriction is the **sum** `R=Pᵀ`, not the mean.
The coarse operator is `Ac=Pᵀ A P`. Internal liquid connections cancel from the
aggregate diagonal; crossing liquid connections become coarse off-diagonals.
Free-surface Dirichlet terms remain diagonal, and solid Neumann links remain
absent. The Galerkin construction follows the standard
[RAP operator relationship](https://petsc.org/release/manualpages/PC/PCMGSetGalerkin/);
there is no PETSc dependency or imported solver code.

Each default cycle executes four fine weighted-Jacobi sweeps, residual
restriction, 24 coarse weighted-Jacobi sweeps, constant prolongation and four
fine sweeps. Three cycles are followed by 48 fine cleanup sweeps; damping is
2/3. The cleanup is intentional: constant aggregates poorly represent some
two-cell-deep free-surface modes. These counts are reported explicitly, not
presented as the legacy `pressureIterations` value. A closed aggregate's pressure
nullspace is not hidden with an artificial pressure anchor.

This correction space may aggregate disconnected fine children. The fine
operator still retains their topology, but coarse convergence can suffer.
Geometry-aware aggregation, additional levels and MG-preconditioned CG remain
future work. This is not a guarantee of better residuals for every water state.

## Reproduce

```powershell
.\test-fluid-pressure.ps1
.\profile-fluid-pressure.ps1 -Tag pressure-tiled
.\test-water-temporal.ps1 -Name water-temporal-pressure-active -Whitewater -Adaptive -Pressure active
```

```bash
node --test engine/*.test.mjs
node engine/validate-water-temporal.mjs RUNTIME water-temporal-pressure-active water-temporal-adaptive --preserve
```

Close interactive games before these serial, bounded GPU runs. The temporal
comparison requires saved checkpoint-1 captures. Profiling uses 1080p Balanced,
room/inlet/foam, an orbiting camera, frame generation off, three rotated mode
orders and 300 frames per run. Frame/fluid medians omit 32 warm-up frames;
the separately reported pressure mean includes all frames. No full validation
readback is requested during profiling.

## Validation and measured cost — RTX 5090, 2026-09-12

- 59 Node tests and six Windows CTests pass. New numerical tests cover operator
  symmetry/energy, odd and partial grids, air/solid boundaries, exact tiled Jacobi
  equivalence, smooth/alternating pressure error and empty/Neumann domains.
- Windows Release and all transport/compute/debug shader variants compile.
  Only existing external Bullet/RmlUi warnings remain.
- Eighteen bounded GPU cases pass across active and multigrid: compression,
  empty, falling water, room/inlet/foam, 119 Jacobi sweeps, FLIP, zero capillarity,
  frame generation and ReSTIR PT. GPU snapshots validate complete unique work
  lists, tile coverage, coarse coefficients, indirect arguments and residuals.
  Coupled cases additionally run existing particle/collision, projection and
  procedural-fluid/whitewater optical checks.
- In the controlled compression fixture, RMS divergence starts at 3.0 and ends
  at 0.775153 with active Jacobi or 0.678938 with the default coarse correction.
  This is one convergence test, not proof of superior general fluid accuracy.
- The three interleaved room profiles all retain 105,899 particles and 600
  simulation substeps, with no dropped simulation time. Neither mode is enabled
  by default: active scheduling did not improve total fluid time here; multigrid
  costs about 0.15 ms more per frame. Raw total-frame variation is too large to
  attribute an FPS improvement to pressure scheduling.

| Solver | Fluid ms, three run medians | Pressure ms, three run means |
| --- | --- | --- |
| Uniform | 1.93869 / 1.95014 / 1.93830 | Not separately instrumented in legacy path |
| Active tiled | 1.94426 / 1.95120 / 1.94429 | 0.301095 / 0.302709 / 0.305297 |
| Two-level | 2.08499 / 2.08688 / 2.09523 | 0.420613 / 0.421611 / 0.423384 |

An untiled active-list prototype did not improve fluid time either. The final
8×4×4 tile reduced that prototype's pressure mean from about 0.313 to 0.303 ms,
but this is **not** an improvement over the legacy uniform solver. An 8×2×8 tile
trial cost more and was rejected. No speedup is inferred from fewer dispatches.

The 320-frame whitewater temporal sequence passes preservation gates against
checkpoint 1 (`water-temporal-adaptive`), including top-down orbit and rolling.
Across all five phases, mean brightness changes by less than 0.028%, raw temporal
delta increases by at most 0.087%, and RR delta increases by at most 0.492%.
Camera-only phases do not advance fluid or reset caustic history; RR resets only
once. These are preservation metrics, not a claim of another denoising improvement.
See [the compact measurements and runtime fingerprints](pressure-validation.json).

D3D12 debug/GBV remains unavailable in this installation (`0x887A002D` on the
previous explicit attempt). Engine invariant checks are not a clean debug-layer
certification. No OS/driver settings were changed.

## Remaining scope

No coarse liquid fraction/mass inventory, conservative particle reseeding,
second MAC velocity level, LOD face-flux transfer, variable surface resolution,
adaptive optical sampling or async double-buffered simulation is implemented in
this checkpoint. The importance classifier still reports requested LODs only.
