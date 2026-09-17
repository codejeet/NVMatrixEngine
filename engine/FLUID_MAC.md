# Adaptive MAC projection integration

This increment adds an opt-in two-resolution pressure/velocity discretization,
not another passive bulk copy. The initial fine MAC allocation remains as the
APIC transfer and rendering cache; particle-free *flowing* bulk and sparse fine
allocation are subsequent work.

Connections:

- `FluidSystem::projectGrid`: after current classification, solid velocities,
  gravity and material forces, substitute the coupled mixed-grid projection.
- `FluidComplexity`: previous-frame importance requests coarsening. Current
  liquid/solid support and velocity error can always override stale requests.
- `gpu_resources.h`: persistent UAVs, existing resource transitions, PIX events,
  indirect work lists and renderer-owned fence/timestamp readback.
- Existing P2G/G2P: restrict coarse boundary fluxes, solve one pressure per leaf,
  then prolongate into the fine MAC cache with compatible child divergence.
- Surface reconstruction, DXR BLAS, dielectric and photon transport continue
  consuming their existing canonical surface. No second fluid rendering path.

The interior T-junction follows the coarse-face-centered discretization discussed
in [Ando and Batty 2020](https://cs.uwaterloo.ca/~c2batty/papers/Ando2020/Ando2020.pdf):
one normal velocity per large face, area-averaged fine pressures, and adjoint
gradient/divergence. At a 2:1 junction the gradient is
`[-1, 1/4, 1/4, 1/4, 1/4] / (1.5 h)` with dual volume `6 h^3`.
Free surfaces and solids remain fine with a guard band; this does not implement
the paper's free-surface T-junction extension.

`adaptive-mac-reference.mjs` independently assembles the operator in double
precision. Tests cover affine/hydrostatic pressure, adjoints, symmetry, positive
semidefiniteness, energy-reducing projection, and flux-preserving prolongation.
The GPU solver uses absolute-row-sum relaxation on the actual mixed operator;
the existing fine-grid Galerkin solver is not relabelled as a mixed-grid solver.

## Execution and controls

`--fluid-mac` enables classification and coupled pressure. `--fluid-mac-validate`
additionally audits every advancing rendered frame through the existing fence.
Ordinary `--fluid-validate` audits the final substep, without changing the solver.
Do not combine this operator with `--fluid-pressure=active/multigrid`: those solve
a different (uniform-grid) operator and the CLI rejects the combination.

- **F10** toggles forced-fine MAC resolution (also restores dormant owners when
  that separate subsystem is enabled).
- **F11** toggles actual coarse cells (green) and adjacent fine cells (orange).
  `--fluid-mac-view` starts with this overlay visible.
- **F7** freezes requested importance, not the collision/topology safety checks.

Coarsening requires a full two-cell liquid/solid guard, safe current velocity
variation, prior-frame importance, and eight eligible simulation substeps.
Loss of support or a disturbance promotes immediately. The thresholds differ
between promotion and demotion. Counters report actual leaves, T junctions and
promotions/demotions since reset, independently of requested importance LODs.

The pressure matrix is `A = D V^-1 D^T`. At a junction it can have positive
off-diagonal entries, so ordinary Jacobi is not generally justified. For
`H_ii = sum_j abs(A_ij)`, Gershgorin bounds the spectrum of `H^-1 A` to `[0,1]`.
The GPU uses `p += 1.8 H^-1 (b - A p)`: a bounded energy-reducing relaxation.
Uniform interior rows therefore use `0.9 / diagonal`, avoiding an unnecessary
global one-third damping factor. When there are **no coarse leaves**, the GPU
retains the original fine stencil and pressure update exactly; it does not
change the shallow room's pressure equation merely because adaptation is enabled.

After projection, a local eight-cell Neumann solve extends boundary fluxes into
coarse internal transfer faces. Its exact Walsh-basis solve preserves every
external flux and gives all eight children the parent mean divergence.

## Validation and remaining scope

`test-fluid-mac.ps1` runs serial, bounded tests with no user-owned process
termination. It compares calm, falling, moving-solid and room cases against
same-build fine-grid runs; audits the mixed matrix, pressure gradient and
prolongation independently; exercises actual F10/F11 inputs and LOD transitions;
and checks compatibility with particle-free resting owners, DLSS FG and ReSTIR PT.
The moving-solid case audits every frame, including partially refined wakes.

Reference runs use `--fluid-deterministic-bins`, an optional GPU per-cell ID
heapsort after scatter. This makes all particle neighborhood gathers reproducible
without CPU particle work, extra storage or a fixed occupancy cap. It also runs
after conditional density/interior rebinning, reusing the existing cell-dispatch
arguments. Normal play does not pay for this test mode. In the 120-frame impact
comparison, deterministic all-fine and adaptive-fallback runs give identical
density, volume and divergence; unordered atomic scatter had obscured that check.

The large falling-block impact already exceeds 5% local compression on the
original solver. That is **not resolved by this checkpoint**: it has a relative
same-build regression gate, while calm/wake cases retain the absolute 5% gate.

Remaining: sparse fine allocation, flowing Eulerian ownership/advection, robust
cross-LOD APIC sampling, a multilevel solver for this actual mixed operator,
adaptive optical work, and controlled end-to-end speedups. Dense matrix capacity
and a stationary iterative solver are correctness-first scaffolding, not the
final high-particle-count optimization. The default launchers remain unchanged.

## Checkpoint evidence (RTX 5090, Windows)

See [`mac-validation.json`](mac-validation.json), regenerated by
`node engine/record-fluid-mac.mjs <runtime-directory>`, and
[`mac-temporal.json`](mac-temporal.json). Reports older than the executable are
rejected; the summary includes executable and compiled-shader hashes.

- 82 Node tests and 7 Windows CTest tests passed.
- 16 mixed-MAC/reference GPU cases, 6 ordinary solver cases (including 2,400
  substeps), and 2 existing collision/render-control cases passed.
- Calm water: 96 coarse pressure leaves and 128 T faces; unchanged volume and
  density. F10's fine/adaptive cycle also leaves those metrics unchanged.
- Moving ball: all 96 coarse leaves refine; the entire wake sequence's independent
  audit passes. Final volume differs from the deterministic fine reference by
  0.0148%, and peak relative density by 0.00012.
- The two 320-frame room/orbit/rolling sequences retain one RR history reset.
  Maximum brightness change is 0.028%; worst raw/RR consecutive-frame-delta
  increases are 0.195% / 0.330%. These are preservation checks, not proof of
  improved caustic sampling or optical LOD.
- One same-build 1080p Balanced, FG-off room/inlet/foam/orbit run per mode:
  raw median 13.685 ms reference / 13.628 ms adaptive; simulation 2.980 / 3.015 ms.
  The shallow room has **zero eligible coarse leaves**. This is effectively
  unchanged performance, not evidence of an end-to-end adaptive speedup.

D3D12 debug-layer/GBV initialization remains unavailable on this setup
(`0x887A002D`); do not interpret these numerical/rendering audits as a GBV pass.
