# Implicit coarse-liquid transport — opt-in checkpoint

Full-goal dependency: flowing Eulerian ownership needs transport that can empty
a closing cell, not just pressure coverage. The current explicit donor cap
leaves at least 5% behind. This work replaces that transport update in a separate
opt-in path; it does not remove particles or declare Narrow Band FLIP complete.

## Integration map

- `FluidBulk` keeps source admission, the resident/pending ledger, canonical
  FP64 pressure-flux restriction and the existing force update.
- `FluidImplicitTransport` consumes those resident quantities, shared face rates
  and actual end-of-substep cut-cell capacities. It solves a backward-Euler
  upwind finite-volume system for volume and all three momentum components.
- Persistent GPU buffers, root descriptors, compute PSOs, UAV barriers,
  predicated iteration, existing-fence readback and PIX events follow the
  engine's current pressure/phase-limiter abstractions.
- The same shared transfers feed the existing independent bulk balance audit;
  a separate immutable snapshot reconstructs the implicit matrix, checks its
  residual, output, conservation, positivity and kinetic-energy dissipation.
- `FluidBulkPressure`, MAC projection, positional density correction and the
  particle-owned DXR surface remain connected as before. The new inventory is
  not added to particle mass or rendered as a second fluid.
- In this mode only, a partially filled coarse cell that disappears during the
  substep retains pressure support until its liquid can escape. Ordinary open
  fractional tails are still particle-defined. Capillary support and missing
  face weights use the actual starting fraction, not an invented full cell.
  The room placement regression exposed a zero transport row with nonzero
  resident mass when that pressure escape path was absent.

For oriented integrated volume flow `Q = dt * canonicalRate`, the unknown
`c_i` has volume fraction in W and momentum/open-volume in XYZ:

```text
(Cnew_i + sum(outgoing Q)) c_i - sum(incoming Q * c_neighbor) = qold_i
qnew_i = Cnew_i * c_i
```

Summing the equations cancels shared face transfers. A closing cell may have
zero final capacity but positive outflow, so it remains a transport unknown and
can pass its mass onward. Global mass/momentum error must match solver residual
and FP32 storage error, not an invented source correction. A singular/trapped
system or an exhausted solve must be reported, never silently discard mass.

This implicit update is monotone for nonnegative source mass when its transport
matrix is invertible. An upper volume-fraction bound additionally needs compatible
pressure/free-surface support and geometric conservation; unconditional positivity
must not be mislabeled unconditional bounded VOF. First-order diffusion,
partial-interface support, conservative moving particle/grid exchange, sparse
physical allocation and the full adaptive specification remain required.

The implementation is original. Geometric conservation and particle/grid coupling
are separate requirements; the [Narrow Band FLIP authors](https://visualcomputing.ist.ac.at/publications/2016/NarrowBandFLIP/)
specifically identify instability in naive coupling. This transport solver is not
their full coupling scheme. `FLUID_BULK_PRESSURE.md` retains the previous
checkpoint's unresolved temporal-preservation results.

## Next numerical dependency

For this particular matrix, a sufficient upper-bound condition is
`A * 1 >= qold.w` component-wise: the monotone inverse then implies `c.w <= 1`.
Geometric conservation makes `A * 1` equal the previous open capacity when the
carrier flux is incompressible throughout the transported support. Partial
particle/bulk interface support does not yet establish that condition here.
Increasing the linear iteration count cannot repair this missing compatibility.

[The OpenFOAM authors' semi-implicit MULES description](https://cfd.direct/openfoam/free-software/mules/)
is a useful next-stage reference: it limits a higher-order flux correction after
a bounded implicit upwind predictor. That is not permission to apply our old
explicit donor/receiver limiter directly to implicit transfers, or to claim
boundedness when this engine's predictor is already overfilled. No OpenFOAM code
is imported. A compatible bounded base state remains the prerequisite for that
kind of accuracy correction and for conservative particle/grid handoff.

## Validation and limits (RTX 5090)

[`bulk-implicit-validation.json`](bulk-implicit-validation.json) records current
executable/shader hashes, all 13 deterministic GPU scenarios, five same-build
explicit-pressure references and three interleaved raw timing pairs.

- All implicit cases pass every-substep convergence/positivity, independent
  final-substep matrix/balance/energy checks, canonical flux restriction,
  pressure convergence, source admission and finite optical-input checks.
- The wake exercises 768 closing-water updates; the room placement/reset test
  exercises 17. Both finish with zero inventory in fully closed cells. The
  reference wake retains `0.000047035 m³` there.
- The five comparisons preserve particle mass exactly, stay within the existing
  density gate and change reconstructed surface volume by at most `0.19631%`.
  These are physical endpoint comparisons, not a temporal image-quality proof.
- 174 Node tests and all 7 native CTests pass. The Release build and changed
  compute kernels compile. Three module-relative launch tests and the normal
  `Play Lab.cmd` resize/minimize/restore/shutdown test pass (189 interactive frames).
- Seven cases still exceed local endpoint capacity; e.g. the 120-frame room
  has `0.00254093 m³` total excess versus `2.99478e-8 m³` in the explicit-pressure
  reference. **Upper-bound acceptance fails.** No clipping hides that difference.

At 1920×1080 output, Balanced RR and frame generation off, medians across three
300-frame runs change from `22.8818` to `23.5105 ms` raw and from `11.4634` to
`11.8489 ms` fluid time. This is a correctness prerequisite with added cost,
not an optimization or default-promotion result. The implicit helper averages
`0.919246 ms`; the reference phase-limiter helper is not the entire explicit
transport update. The first timing pair was repeated with the capture collector
idle; no build/validation work overlaps the retained timing runs.

Reproduce with `test-bulk-projected.ps1 -Mode implicit`; obtain matched references
using `-Mode bounded -PressureSupport` and case filter
`^(calm|room|room-no-tension|wake|adaptive)$`. Run `profile-bulk-implicit.ps1`
serially with no other GPU workload, then use `record-bulk-implicit.mjs RUNTIME
--profiles` to validate and collect evidence. Scripts close only their owned
test processes. No normal launcher, mainline native engine or portable release
is changed. GPU validation is not claimed: the earlier Graphics Tools probe
failed with `0x887A002D`. The full adaptive-system goal remains incomplete.
