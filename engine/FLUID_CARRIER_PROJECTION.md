# Capacity-constrained air carrier extension — implementation in progress

The next flowing-ownership dependency is a bounded coarse phase field. The
implicit transport checkpoint removes donor residue, but its upwind matrix is
not upper-bounded when the carrier compresses through particle-defined air.

## Integration map

- `FluidMac::projectedVolumeFlux()` remains the immutable physical pressure
  output. `FluidCarrierProjection` consumes it, the current `Cells` classification,
  cut-cell capacities/apertures and admitted `FluidBulk` volume.
- Only open **air-to-air** fine faces crossing coarse-cell boundaries provide
  correction degrees of freedom. Liquid/interface faces, solid boundaries,
  closed domain edges and the original MAC buffer are protected.
- In this mode, `FluidBulkPressure` also retains pressure support when a shrinking
  partially filled coarse cell's endpoint capacity falls below its resident
  volume, not only once capacity reaches zero. That physical evacuation happens
  in the real MAC solve, before the protected-flux extension.
- An independent extended fine-face buffer feeds `FluidBulk` restriction and
  `FluidImplicitTransport`. This is an extension of canonical pressure flux,
  not a second liquid pressure solution or permission to modify solved faces.
- Persistent DX12 resources, root descriptors, PSOs, GPU-predicated red/black
  iterations, UAV barriers, PIX events and existing-fence audits follow current
  engine patterns. No particle readback or per-particle CPU work is added.
- The particle G2P path and rendered DXR scalar field still own physical liquid.
  Their eventual conservative ownership exchange is not implemented by this pass.

## Discrete problem

With integrated canonical carrier flow `Q = dt * F`, cell-face divergence `D`,
and nonnegative air-only conductances `W`, jointly solve a trial liquid fraction
`c` and nonnegative capacity potential `p`:

```text
A(Q + deltaQ) c = qold.w
deltaQ = W D^T p
0 <= c <= 1; p >= 0
p * (1 - c) = 0
```

`A` is the backward-Euler upwind transport matrix using endpoint capacities and
the **corrected** carrier flow. This is an original discrete nonlinear capacity
extension, not geometric VOF or a complete free-surface pressure formulation.
Finite pressure/FP32 capacity error remains part of its measured tolerance.

Each red/black update first tries `p=0`, solving the local upwind fraction. If
that fraction would exceed one, the trial fraction stays at one and a scalar
piecewise-linear solve determines the outward correction potential. The local
equation has at most six flow-reversal breakpoints. Eight Newton steps from a
conservative upper bracket solve it without local root-search loops. Corrected
flow selects the donor, including when correction reverses a face. The global
iteration requires a measured conservation residual, not just stable iterates.

The fraction is only a trial solution; no resident inventory is clamped. The
existing implicit solver separately transports volume and momentum using the
exported shared carrier flux. Its residual, conservation and actual capacity
bounds remain separate acceptance checks. On failure the canonical input flux
is preserved and the existing frame-fence collection reports the failure.

Earlier all-ones and fixed-endpoint upper-envelope certificates were too strong:
they falsely rejected a room region whose independently solved fraction was
below 0.998083. The CPU tests retain counterexamples to those rejected approaches.
The coupled formulation advances past that failure, but it cannot fix genuine
overfill when protected liquid/interface flows leave no air-side escape route.
That requires physical pressure/fraction coupling, not inventory clipping or
opening solids. No minimum-energy or general convergence claim is made here.
Independent immutable snapshots of **each substep** assemble individual fine faces, reconstruct the
coarse matrix and nonlinear phase balance, verify complementarity and check
protected faces bit-for-bit. CPU references use bisection and an independent
dense transport solve; the local Newton update is also checked against bisection
on 1,000 randomized six-face cases.

`--fluid-bulk-air` enables this experimental path and its implicit prerequisites.
It is separate from `--fluid-bulk-implicit` for same-build comparisons. The
normal launchers remain unchanged. Runtime acceptance is still in progress.

## Validation and current limit

Current-build hashes, passing reports and the unwaived room failure are recorded
in [`carrier-validation.json`](carrier-validation.json).

The short moving-room test now reaches frame 2, substep 1. There is a genuine
protected-component overfill there: the independent unextended transport solve
has residual `4.72e-12`, maximum fraction `1.01124` and component excess
`0.000321976 m³`. Its two cells began at fractions `0.999864` and `0.999828`.
This demonstrates that nearly filled bulk loses the existing strict filled-cell
pressure support before air-only transport can provide a feasible correction.
The unchanged upper-bound test rejects it. This path is **not ready for normal
launchers, flowing ownership, or a speedup claim**.

`NVMatrixEngineCarrierGpuTest <runtime-folder>` runs isolated analytical fixtures on
the actual hardware kernels: valid mixed flow, compression, flow reversal,
closing capacity and mandatory rejection of protected-face overfill. Build it
explicitly and run serially with the lab closed; it is intentionally outside
the hardware-independent CTest suite. The regular room/pit tests remain in
`test-bulk-projected.ps1 -Mode air` and are not replaced by these fixtures.

This checkpoint's checks: 188 Node tests, seven native CTests, all five focused
hardware cases, the initial pit/room/empty cases, a 120-frame calm pool and the
controls case pass. The same-build old implicit short case retains its previous
`0.00625802 m³` excess (reported, not a bounded-transport pass). Standard launch
checks pass through basename/relative/foreign working directories; the unchanged
Explorer launcher rendered 205 interactive frames and closed normally after
resize/minimize/restore. No new performance claim or GPU-validation-layer-clean
claim is made; the prior Graphics Tools/GBV availability issue remains unresolved.

Next integration: couple the real pressure solve to capacity activation while
retaining physical divergence constraints. Do not substitute a looser fill
threshold, repeated gravity application, or silent mass redistribution for
that coupling. A CPU counterexample demonstrates why paired changes to
liquid/interface fluxes can restore phase bounds while keeping the solved
liquid row's divergence exactly unchanged. The next correction must respect
the actual mixed-MAC constraints (`D_mixed deltaQ = 0`), moving-solid apertures
and boundary conditions, not merely alter a coarse replica. Reuse the existing
MAC operator and avoid applying external forces twice during capacity activation.
This coupled physical correction is not implemented by the air-only kernel.
The particle/grid ownership and sparse-grid requirements remain.

Velocity extension outside a free surface is a standard liquid-solver concern;
see [Enright et al., §3.3](https://graphics.stanford.edu/papers/water-sg02/water.pdf)
and [Bridson's course notes, §6.3](https://www.cs.ubc.ca/~rbridson/fluidsimulation/fluids_notes.pdf).
Those references do not establish this engine's discrete capacity constraints;
the implementation and its independent audits are original.
