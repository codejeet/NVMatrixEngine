# Conservative receiver-limited phase transport

`--fluid-bulk-bounded` extends `--fluid-bulk-capacity` with receiver limits on
the actual shared liquid transfers. It remains opt-in and particle-owned, not
the completed flowing Narrow Band FLIP system or a new default play preset.

## Integration map

1. `FluidCutCells` supplies current coarse open capacities.
2. `FluidBulk` restricts the existing FP64 canonical pressure flux, applies the
   existing momentum sources, and generates donor-positive candidate transfers.
3. `FluidFluxLimiter` limits those **shared faces**, before `BulkUpdate`. It uses
   the existing DX12 buffer, barrier, shader compilation, PIX and frame-fence
   abstractions. It neither changes pressure velocities nor mutates particles.
4. `BulkUpdate` applies each limited face with opposite signs at its neighbors.
5. Every-substep GPU audits, independent FP64 final-substep snapshots and the
   existing global inventory ledger check distinct invariants.
6. DXR surface reconstruction, photons, ReSTIR and DLSS-RR retain their existing
   authoritative particle inputs. Optical isolation is tested separately.

No new queue, CPU particle processing, full-grid normal readback or fence is
introduced. The limiter has GPU-controlled indirect face/cell dispatches and
separate timestamp accounting. As in the existing MGPCG implementation,
[DX12 predication](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-setpredication)
skips both work and prepare dispatches after convergence. The argument buffer
contains an aligned 64-bit predicate; the snapshot is refreshed each iteration,
and predication is disabled before the unconditional final audit. The bounded
command schedule still has overhead after convergence; skipped dispatches do
not mean zero cost.

## Algorithm and limits

For a cell, let `I` and `O` be its current candidate incoming/outgoing liquid
volumes, `V` its initial resident volume, and `C` current geometric capacity.
The receiver factor is bounded by `(max(C,V) - V + O) / I`. Apply a factor only
when inflow exceeds that room (with a documented floating-point tolerance).
Scale all four conserved components of each incoming shared face together.
Repeat, since reducing one cell's inflow reduces its donor's outflow too.

This monotonically removes candidate transfers. Donor positivity is preserved,
the same shared transfer conserves mass and linear momentum, and convex mixing
cannot increase kinetic energy except for floating-point error. Already feasible
circulation through completely full cells is unchanged: outflow supplies room
for simultaneous inflow. A naive free-space-only receiver clamp would stop that
circulation and is not used here.

The target bound is `max(C,V)`, not just `C`: existing excess from closing solid
geometry stays explicit. The limiter cannot manufacture a swept-volume escape
path, restore free-surface pressure support, or silently discard water. Limiting
the liquid transfer also does not preserve the original pressure flux's
divergence in general. Closing-cell treatment, pressure/phase consistency,
interface reconstruction and conservative grid/particle handoff remain required
before physical ownership can move to this representation.

Convergence uses `max(1e-12 m³, C × 2e-7)` with a small downward rounding margin.
Every substep also checks the actual FP32 `BulkUpdate` summation against
`max(1e-11 m³, C × 1e-6)`. These are numerical tolerances, not volume clamps.
Unresolved iteration exhaustion is a failing audit, not an accepted bounded
update. The initial maximum is 128 iterations; this is not a proof of finite
convergence for every possible scene graph. Geometry/pressure consistency and
an improved schedule remain necessary for production-scale ownership.

This original implementation uses conservative face limiting as a building
block. [OpenFOAM's MULES documentation](https://api.openfoam.com/2506/MULES_8H_source.html)
is a related reference for bounding transported phase variables through fluxes;
this implementation is not MULES, a geometric VOF interface advection scheme,
or a copied reference solver.

## Validation commands

```powershell
.\test-bulk-projected.ps1 -Mode bounded
.\test-bulk-projected.ps1 -Mode capacity -CaseFilter '^(calm|room|wake|adaptive)$'
.\profile-fluid-bulk.ps1 -Bounded -Tag phase-flux-cost -Repeats 3
```

```bash
node --test engine/*.test.mjs
node engine/check-bulk-projected.mjs RUNTIME --bounded
```

Reference cases include competing inflows, full circulation, upstream
backpressure, insufficient iteration budget, preexisting closed-cell excess,
GCL-compatible transport, tiny signed transfers and randomized conservation /
kinetic-energy tests.

The final build passes 154 Node/reference tests, seven Windows CTests and twelve
GPU scenarios. Every simulated substep passed the convergence/bounds checks.
The largest measured new per-cell FP32 excess was `7.45e-9 m³`; independent
final-substep flux replay differed by at most `1.99e-10` in volume or
volume-weighted momentum components. No kinetic-energy increase was observed
in those snapshots. Pause/reset and empty simulation are included, not inferred
from a flowing-room run.

Six same-build pairs (initial calm/room, calm, room/inlet, moving wake and
adaptive rendering) match exactly in authoritative particle diagnostics and all
ten captured lighting/guide channels: [isolation evidence](phase-flux-parity.json).
This demonstrates that the passive transport change does not alter those
captured renders. It is not a new full temporal-sequence audit or evidence that
the bulk replica's liquid geometry is ready to replace the particle surface.

In the deterministic room/inlet scene, resident excess after 120 frames falls
from approximately `2.21187` to `0.00544144 m³`. The calm pool has zero excess;
the falling-water scene ends at `7.04e-9 m³`. This is not full closure acceptance:
the moving-solid wake still ends with `0.0125912 m³` resident in closed cells.
The reset scene retains `1.53e-6 m³` of excess. These defects are reported rather
than redistributed or removed by the limiter.

No GBV-clean claim is made: the preceding Windows Graphics Tools initialization
probe failed with `0x887A002D`. This change does not modify the OS or driver.

## Raw-frame cost

[Final-build evidence](phase-flux-validation.json) includes three interleaved
300-frame room/inlet/orbit/foam runs per mode, 1080p Balanced, FG off, normal
bins/float atomics, and no full validation snapshots. Both sides enable projected
bulk and source admission; only the new phase limiter differs.

| Median across runs | Source admission baseline | + phase limiter |
| --- | ---: | ---: |
| Raw GPU frame | 22.0485 ms | 23.0544 ms |
| Fluid GPU time | 10.5403 ms | 11.2171 ms |
| Limiter mean | — | 0.71005 ms |

The limiter adds 67,392 logical GPU buffer bytes in the room, excluding reused
bulk resources and readbacks. Predication reduced the early no-work fixture
measurement from about 0.94 to 0.57 ms, but the remaining command/barrier schedule
still costs too much for a mostly inactive subsystem. This is a correctness
prerequisite, not an FPS improvement, a 60-FPS raw-render result or evidence that
the experimental mode should replace the gameplay default. Sparse active work,
a lower-overhead convergent schedule, coupled pressure/phase transport and
particle-free bulk ownership remain on the full implementation path.
