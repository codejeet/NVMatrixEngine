# Bulk-filled pressure coverage

`--fluid-bulk-pressure` connects transported, capacity-admitted bulk interiors
to the real pressure solver. It implies receiver-bounded phase transfers,
time-centered cut geometry and mixed MAC MGPCG. This is still an ownership
prerequisite, not a completed Narrow Band FLIP implementation.

## Concrete integration map

1. `FluidBulk` supplies the GPU-resident volume/volume-weighted velocity inventory
   **before** its next advection. Pending source requests are excluded.
2. `FluidCutCells` supplies the simulation-start coarse capacity, temporal fine
   support capacity and the exact aperture buffer selected for pressure.
3. After normal particle classification/P2G, `FluidBulkPressure` promotes air
   cells inside filled bulk regions to pressure liquid cells. Existing solid and
   particle classifications are preserved. The same helper fills missing face
   velocities, then restores the renderer's original bindings.
4. `FluidMac` consumes the augmented cells/faces in its existing mixed operator,
   multigrid solve and canonical shared-face flux. This is not a second detached
   pressure solve or a change confined to a diagnostic replica.
   The surface-tension occupancy uses the union of particle occupancy and filled
   bulk volume with its existing spatial filter, not a sum of duplicate water.
5. APIC/FLIP G2P consumes projected velocity as before. `FluidBulk` advects using
   that same canonical flux. No resident mass is deleted, duplicated or moved to
   pending by the coverage passes.
   Positional density correction retains this interior connectivity, with the
   actual endpoint solid boundaries. Its mass/RHS still comes solely from the
   measured particle/solid kernels; coverage does not add duplicate water.
6. Free-surface reconstruction, DXR intersection, photons and DLSS-RR
   retain the particle surface. Conservative moving ownership/reseeding and
   interface-volume reconstruction remain required before removing particles.

Filled means admitted resident volume is at least the coarse simulation-start
open capacity, within a relative `1e-6` floating-point tolerance. It is an
interior-only coverage decision: fractional interface cells and first-order
transport tails do not become artificial full water cells. This does not remove
or ignore their mass in transport/accounting. The particle band remains necessary
near interfaces; bulk-only fraction/level-set surface tracking remains work.

For a face without valid particle/owner P2G weight, adjacent filled cells provide
a temporal-open-volume-weighted mean velocity and a corresponding transfer
weight. Both projected and previous velocity start at that value, before forces,
so FLIP does not receive a fabricated zero previous velocity. Existing P2G faces
are preserved bitwise. This is missing-coverage interpolation, not the complete
mass/momentum coupling scheme of Narrow Band FLIP or its extended variant.

## Validation scope

The subsystem has PIX labels, separate GPU timestamps, per-substep coverage
counters and optional independent final-substep snapshots (every rendered frame
with `--fluid-mac-validate`, or the selected final frame with bulk validation).
`projectionCalls` is a lifetime count, including work before scene resets;
`lastFrameProjectionCalls` distinguishes an idle frame from earlier work.
Idle validation checks zero coverage counters separately, and does not claim a
new projection snapshot. Those snapshots
reconstruct classification and velocity from immutable inputs and verify that
unaffected cells/faces remain bitwise unchanged. The existing independent MAC
matrix/flux audit checks the augmented operator downstream. Global bulk
mass/momentum accounting and particle/optical validation remain separate gates.

Reproduce the bounded validation and paired raw-cost runs from the repository:

```powershell
.\engine\test-bulk-projected.ps1 -Mode bounded -PressureSupport
.\engine\test-bulk-projected.ps1 -Mode bounded -TimeCentered -CaseFilter '^(calm|room|room-no-tension|wake|adaptive)$'
.\engine\test-water-temporal.ps1 -BulkBounded -TimeCentered -Name bulk-pressure-reference
.\engine\test-water-temporal.ps1 -PressureSupport -Name bulk-pressure-temporal
.\engine\profile-bulk-pressure.ps1 -Repeats 3
```

```bash
node --test engine/*.test.mjs
node engine/validate-water-temporal.mjs RUNTIME bulk-pressure-temporal bulk-pressure-reference --preserve
node engine/record-bulk-pressure.mjs RUNTIME
```

Profiling excludes full validation snapshots, runs serially with frame generation
off, and compares against the same time-centered, receiver-bounded configuration
without pressure support. Validation-window FPS is not representative gameplay
performance. Normal launchers and release packaging are unchanged.

An initial paired room check exposed a density-correction coverage gap: the
highest-density cell had **zero binned particle centers**, yet neighboring
quadratic kernels compressed it above rest density. Particle-bin-only density
classification skipped that row. The bulk mode now retains filled-interior
connectivity in the density operator too, including its error-triggered repairs.
The correction still uses endpoint solids and measured particle/solid kernel
mass, not an added bulk density. The unit regression constructs this empty-bin
overlap explicitly; native reports also expose particle count at density peaks.

[Final-build evidence and executable/shader hashes](bulk-pressure-validation.json)
cover a physical/optical capture suite passing 13 scenarios and five
matched reference comparisons, plus 167 unit tests and seven native CTests.
All cases retain global mass accounting and independent pressure/coverage checks.
Matched particle mass is identical; the largest reconstructed-volume difference
is 0.414%, within the pre-existing 1% gate. Post-correction peak density changes
are also within the original reference-plus-0.01 gate (not a new relaxed limit).

| Final resident excess | Time-centered reference | With bulk pressure support |
| --- | ---: | ---: |
| Calm pool | 0 m³ | 0 m³ |
| Room/inlet | 0.00594653 m³ | 0.0000000299478 m³ |
| Room without surface tension | 0.00594781 m³ | 0.0000000242144 m³ |
| Moving wake | 0.00468072 m³ | 0.0000471673 m³ |
| Adaptive room | 0.00653923 m³ | 0.0000000151631 m³ |

Room peak post-correction density improves from 1.06294 to 1.01840 times rest
density; adaptive-room density improves from 1.07516 to 1.00874. Wake density
changes from 1.00133 to 1.00594, still within its unchanged gate. Wake excess is
about 99% lower but not zero: `0.000047035 m³` remains in completely closed
cells. The explicit donor cap and interface consistency remain unresolved;
none of this residual is clipped, reassigned to pending sources or excluded
from accounting. The short eight-frame room also retains `0.000103897 m³`
of excess, so this is not a blanket bounded-volume guarantee.

## Temporal acceptance remains open

The [recorded temporal comparison](bulk-pressure-temporal.json) explicitly marks
preservation as failed. Both 320-frame sequences complete with finite captures, correct photon accounting,
one RR initialization reset, and no camera-only reset of the water caustic history.
However, the existing `--preserve` comparison **does not pass**: static selected
water brightness differs by -1.382% (limit 1%), and top-down raw frame differences
increase by 5.236% (limit 5%). All RR frame-difference changes remain within 5%.
The physical trajectory and selected water-pixel population differ between these
simulations; those are possible contributors, not proof that the failures are
harmless. No brightness adjustment, lighting blur, threshold relaxation or default
promotion is used to hide them. Same-fluid-state optical replay and broader
temporal/error assessment remain required.

## Incremental raw cost

Three interleaved 300-frame room/inlet/orbit runs per mode use 1080p Balanced,
normal bins/float atomics, foam enabled, frame generation off, and no full audit
snapshots. Both modes use the same bounded bulk and time-centered cut geometry.

| Median across runs | Reference | Bulk pressure support |
| --- | ---: | ---: |
| Raw GPU frame | 22.9368 ms | 23.1009 ms |
| Fluid GPU time | 11.1320 ms | 11.4775 ms |
| Mixed pressure mean | 3.85114 ms | 4.21835 ms |
| New coverage passes mean | 0 ms | 0.0122636 ms |

The raw median difference is +0.1641 ms (+0.72%), within the observed run-to-run
spread; paired raw differences are +0.1641, +0.0115 and +0.0046 ms. Fluid/pressure
work is somewhat higher because the coupled problem changes. This is not an
established speedup or raw 60-FPS acceptance. Coverage storage adds 496,156
logical GPU bytes in this room (one float per fine cell plus counters), excluding
heap alignment, query storage and validation/readback buffers.

No new GPU-based-validation claim is made: the earlier Windows Graphics Tools
initialization probe failed with `0x887A002D`. No OS or driver settings changed.

Sparse physical MAC allocation, flowing particle-free ownership, conservative
fine/coarse exchange, robust closing-cell phase transport, moving-interface
geometry and the rest of `ADAPTIVE_REQUIREMENTS.md` remain on the full goal.
