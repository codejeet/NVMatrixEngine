# Mixed-MAC multigrid pressure

This is an opt-in pressure-solver milestone for the adaptive fluid subsystem,
not completion of the full adaptive simulation/rendering specification.

Run the lab with `--fluid-mac-solver=multigrid`. The normal launcher and
`--fluid-mac` keep their existing paths. `--fluid-mac-solver=relaxation` explicitly
selects the earlier mixed-MAC relaxation solver. Do not combine either mixed
solver with the separate `--fluid-pressure=active/multigrid` fine-grid operators.

## Integration

`FluidSystem::projectGrid` → `FluidMac::solve` assembles the existing coupled
fine/2h MAC operator → `FluidMacPressure` solves that operator → the existing
MAC projection and compatible child-face prolongation feed APIC/FLIP, density
repair, surface reconstruction, DXR water, caustics and DLSS unchanged.

The new solver reuses the engine's persistent GPU buffers, shader compiler,
root-descriptor convention, indirect dispatch, PIX events and frame fence.
There are no iteration-level CPU readbacks or new rendering frameworks.

## Numerical contract

- CG operates on the actual symmetric mixed MAC matrix, including positive
  sibling couplings at T junctions. The pressure equations are not replaced by
  a coarse approximation.
- Geometric aggregates start at 4h (so each mixed 2h leaf is wholly contained),
  then double to a bottom grid of at most 64 cells. Restriction is **summation**
  `Pᵀ`, not averaging; these equations represent integrated face flux.
- Coarse operators are `Pᵀ A P`. Fine T-junction sibling terms cancel within an
  aggregate. Shared coarse-face conductances are canonicalized for bit-identical
  symmetry; air-boundary diagonals are accumulated separately to avoid an
  artificial pressure leak from float cancellation.
- A symmetric V-cycle uses two pre/post damped-Jacobi sweeps per level, bounded
  by the actual mixed absolute row sum on the base grid. The bottom solve is
  a GPU Cholesky factorization and triangular solve.
- A small diagonal shift regularizes **only the bottom preconditioner**.
  The physical mixed pressure operator remains unshifted, including its closed
  Neumann null modes.
- The true residual `b − A p` is recomputed after each CG update. Relative norm
  and maximum physical divergence both control convergence; the iteration cap
  is 32. Capped solves are reported explicitly, not labelled converged.
- Pressure accumulation, true residual evaluation, RHS flux summation and the
  final pressure-gradient subtraction use FP64. The matrix, Krylov vectors,
  dot reductions, smoothers and Galerkin hierarchy remain FP32. The additional
  persistent pressure buffer costs eight bytes per base-grid capacity cell.
  `DoublePrecisionFloatShaderOps` is capability-checked before creating this
  opt-in solver; the unchanged relaxation mode remains available otherwise.
- Dot products use fixed geometric reduction order. Other iterative work uses
  GPU-compacted active lists. This prevents atomic append ordering from changing
  the floating-point reduction tree.
- GPU convergence zeros indirect dispatches. DX12 predication additionally
  skips remaining dispatch command work. Predication is disabled before the
  projection, diagnostic copies and the rest of rendering.
- Read-only matrix applications address buffer members directly. DXIL checks
  confirmed that this removes two private 24-element arrays and eager loads of
  unused neighbor slots from the hot smoother and matrix-vector kernels.
- Hierarchies with at most 4,096 coarse-capacity cells execute their complete
  coarse V-cycle in one 128-thread group. Device/group barriers order global
  buffer accesses; there are no inter-group spin waits. Larger hierarchies use
  the earlier distributed schedule. `--fluid-mac-split-coarse` forces that
  schedule for same-accuracy comparisons.

## Impact precision and density correction

The old impact failure was an FP32 precision floor, not an isolated incompatible
Neumann component. A GPU-selected snapshot of capped substep 75 showed one
11,971-cell connected wet region with air boundaries and pressures around
366 kPa. Its residual initially fell, stalled around `5e-4/s`, then grew to
`0.0346/s` by iteration 32. Increasing the iteration cap would not fix the
cancellation of neighboring large pressures. The precise accumulator and true
residual remove that failure without weakening the `1e-4/s` divergence target
or increasing the cap. The face-based independent audit also retains its limits.

`--fluid-mac-validate` now preserves the actual capped substep, if one occurs,
using GPU-predicated diagnostic copies. The existing frame fence makes it safe
to save `mgpcg-failure-<substep>.bin`; no iteration waits or non-audit snapshots
are introduced. `inspect-mac-pressure.mjs <file>` reads the versioned snapshot,
reports connected components and worst cells, and prints the residual trace.

More accurate velocity projection exposed an under-converged positional density
solve in the room/inlet fixture. Keeping 60 density sweeps gave peak density
1.13793; 120 gave 1.11162 and 240 gave 1.10540. The opt-in precise path now uses
120 by default, and its fine reference uses the same density count. The original
unilateral RHS, collision handling, displacement trust region and mass are
unchanged. `--fluid-density-iterations=0..1000` explicitly controls the count.

Pairs of density sweeps use the existing pressure subsystem's 8×4×4 tile/halo
scheme. This computes the same two global Jacobi iterates per dispatch, including
across tile boundaries; an odd final sweep uses the original kernel.
`--fluid-density-scalar` retains separate sweeps for validation/profiling.
The GPU error flag still skips an unnecessary repair. Tile allocation/scheduling
is dense at this checkpoint, not yet spatially compact. Dispatches use two group
dimensions when necessary, respecting DX12's 65,535-groups-per-axis limit.
Normal launchers retain the original 60-sweep scalar density schedule.

`multigrid.lastIterations`, `peakIterations`, `meanIterations`,
`exhaustedSolves`, `peakFinalDivergence`, `cappedMaxDivergence`, and
`activePerLevel` expose the actual work and numerical limits. The parent
`iterations` field remains the configured legacy relaxation count; it is not
the CG iteration count.

## Validation

`node --test engine/mac-multigrid.test.mjs` independently checks Galerkin energy,
matrix symmetry, V-cycle linearity/positive definiteness, manufactured mixed
solutions, irregular wet regions and a closed Neumann domain.

`test-fluid-mac.ps1 -Solver multigrid` runs empty, calm, LOD cycling, falling
water, moving-solid wake, dormant ownership, room/inlet, FG and ReSTIR PT cases.
Its fine-grid reference uses 1,000 sweeps: the interactive 120-sweep solve is
deliberately bounded and is not a converged reference for the new solver.
The existing comparison limits (1% reconstructed volume, +0.01 peak density)
are unchanged. Falling-block impact compression is an existing solver-wide
limitation; this milestone does not assert a 5% absolute impact density bound.

The GPU audit independently aggregates captured base rows in double precision,
compares all coarse coefficients, checks exact shared-face symmetry, verifies
the Cholesky factor and recomputes the final physical residual. The existing
independent face-based MAC audit still checks the base operator, prescribed
boundary flux and actual projected divergence. Snapshot work is validation-only.
Every substep must now meet the convergence gate: the test rejects any capped
solve or peak final divergence above `0.000101/s`, not only a good last snapshot.
`test-fluid-mac-schedules.ps1` separately compares paired/scalar density and
fused/distributed coarse scheduling for room, impact and odd-iteration cases.

`profile-room.ps1 -MacMultigrid` profiles uninstrumented raw frames; use
`test-water-temporal.ps1 -MacMultigrid` for the orbit/rolling optical sequence.
Do not interpret validation-snapshot timings as interactive performance.

Checkpoint evidence is recorded by `record-mac-multigrid.mjs` in
`mac-multigrid-validation.json`; optical evidence is in
`mac-multigrid-temporal.json`. Reports must be newer than both the executable
and all compiled shaders. Validation snapshot timings are not profile timings.
The unchanged uniform path and all precise scheduler variants are profiled
sequentially with FG off, without snapshots or deterministic particle sorting.
These are operating-cost observations, not a statistically established speedup.

The current checkpoint passes 90 Node tests, seven Windows tests, 16 mixed-MAC
GPU/reference cases, nine scheduler comparisons, six ordinary-fluid cases,
four legacy-MAC checks and two 320-frame optical sequences. No tested MGPCG
substep exhausted its cap; peak iteration count was 17 and peak final divergence
was `9.97113e-5/s`. Maximum reconstructed-volume difference from the matched
fine reference was 0.543%; the largest positive peak-density difference was
0.00563. The falling-block reference itself still has localized impact
compression (1.38089 with 120 density sweeps, versus 1.20912 for MGPCG); this is
not a claim of globally bounded 5% compression or a production-complete solver.
All nine scheduler comparisons matched reported volume and density exactly.
Temporal brightness changed by at most 0.383%; all existing raw/RR limits passed.

| Same-build room/orbit mode | Raw frame median | Fluid median | Raw p95 |
| --- | ---: | ---: | ---: |
| Unchanged uniform, 60 scalar density sweeps | 12.046 ms | 2.942 ms | 14.401 ms |
| Precise MGPCG, distributed coarse / paired density | 17.287 ms | 7.299 ms | 19.691 ms |
| Precise MGPCG, fused coarse / paired density | 16.559 ms | 6.683 ms | 19.058 ms |
| Precise MGPCG, fused coarse / scalar density | 16.440 ms | 6.644 ms | 19.708 ms |

All precise modes use 120 density sweeps. Fusing the coarse cycle reduced fluid
time by 8.4% in this single comparison; paired density showed no established
GPU-time advantage despite fewer dispatches. Compact active density tiles and
further pressure scheduling work remain worthwhile. The uniform path is still
substantially faster, so neither the solver nor the normal launchers are promoted.
No D3D12 GPU-based-validation pass is claimed: debug-layer initialization remains
unavailable on this installation (previous HRESULT `0x887A002D`).

## Scope still outstanding

The hierarchy currently accelerates pressure correction; it does not add
sparse fine-grid allocation or general particle-free flowing bulk. The dense
fine transfer cache and base matrix capacity remain. Variable-resolution
surfaces, optical importance, caustic reservoirs, asynchronous simulation,
multirate stepping and unified budget feedback remain separate milestones.
The solver must earn a default-path change through accuracy **and** speed
measurements, not merely lower iteration counts.

## Primary references

- [McAdams, Sifakis and Teran: parallel multigrid Poisson solver (2010)](https://graphics.cs.wisc.edu/Papers/2010/MST10/): multigrid as a CG preconditioner for irregular fluid domains.
- [Microsoft DX12 predication](https://learn.microsoft.com/en-us/windows/win32/direct3d12/predication): GPU-side conditional execution and snapshot semantics. The 64-bit predicate uses the indirect/predication resource state and is not a CPU convergence query.
- [Microsoft D3D12 feature options](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_d3d12_options): the double-precision shader capability used by this mixed-precision path.

This is original engine-integrated code. Published CPU speedups are not claimed
as measurements of this DX12 implementation.
