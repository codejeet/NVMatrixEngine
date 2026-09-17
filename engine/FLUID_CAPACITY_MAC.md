# Coupled capacity / mixed-MAC correction — in progress

The `fbf3416` air-only prototype demonstrated that protected liquid/interface
faces can make transport genuinely unbounded. The next mode is
`--fluid-bulk-coupled`; defaults and launchers remain unchanged.

## Integration

- `FluidMac::constraintView()` lends the actual leaf map, coarse state,
  relaxation rows and precise cut-cell operator. No duplicate pressure grid or
  altered physical liquid classification is created.
- `FluidCarrierProjection` solves trial phase fractions and coarse capacity
  potentials together with liquid-row multipliers. Fine-face pressure gradients
  use the actual mixed patch widths, 2:1 averaging, temporal cut apertures and
  clipped cell distances. Solid and outer-domain faces remain prescribed.
- Mixed-row multiplier correction uses one V cycle per outer iteration from
  the existing MAC multigrid preconditioner, borrowing its current rows, lists
  and bottom factor. `--fluid-capacity-cycles=4` retains the original inner-work
  schedule for comparison; values 1–4 are accepted by both CLI and subsystem API.
  Only residual/correction scratch is reused; physical pressure and captured
  physical-solve metrics remain untouched. GPU
  residual checks include nonlinear phase conservation and the **final physical
  divergence**, including the original pressure residual and moving-solid volume
  source. Freezing the original residual made full-cell constraints inconsistent
  at tighter capacity tolerances. The coupled target is `1e-6/s` final divergence;
  the original physical solve still has its independent `1e-4/s` gate.
- Liquid multipliers now store **total liquid correction pressure**; coarse
  capacity pressure acts only on air children. Fully liquid parents fix their
  redundant capacity potential to zero. Face coefficients separately measure
  left/right air support. The capacity diagonal includes internal air/liquid
  faces, not just coarse exterior faces. A proximal iteration term stabilizes
  that coupling and vanishes at the fixed point; it is never an exported flux.
- Convergence additionally requires a signed phase supersolution residual
  `A*c - qold >= -1e-12 * rowScale`. Unsigned phase error remains bounded by
  `5e-7`. A small positive root margin adjusts flux, not resident inventory.
  This prevents repeated acceptance of negative phase residuals that can
  accumulate overfill even when physical pressure has converged.
- Only this coupled mode uses FP64 coarse capacities, resident volume/momentum,
  source ledgers/pending requests and shared transport transfers. Fine geometry,
  particle data, optical surface data and APIC velocity caches remain FP32.
  Capacities are summed directly from fine volumes in FP64, with no intermediate
  rounded coarse total. Implicit transport uses a `2e-13` normalized tolerance;
  all other modes retain their existing numeric formats/tolerances. CPU
  snapshots decode the actual storage precision, not a rounded display copy.
- Coupled liquid retains its last simulated collider pose on paused/zero-step
  frames. The next actual substep sweeps the pending rigid motion, keeping
  capacity changes paired with transport. Explicit reset establishes a new
  anchor. Legacy pause-placement behavior remains unchanged in other modes.
  Live collider replacement/topology changes still need a separate conservative
  policy; this is not permission to silently remap resident liquid.
- The correction runs **after the real pressure solve, before extrapolation and
  G2P**, only inside actual transport substeps. Paused/reset display-only grid
  rebuilds do not run capacity transport. `FluidMac::applyCapacity()` writes the shared FP64 canonical flux and
  FP32 transfer velocities, retaining FLIP's pre-force reference and weights.
  The existing compatible coarse-interior prolongation rebuilds internal faces.
  The divergence debug channel is refreshed afterward; its pressure channel
  remains explicitly the original physical solve's pressure.
- Bulk implicit transport subsequently uses that same canonical buffer. It does
  not run a second capacity projection after particles have already advanced.
  Gravity, viscosity and capillarity are not applied a second time.
- Every-substep immutable snapshots independently assemble fine-face corrections,
  check original/proposed/applied physical divergence, coarse restriction and
  nonlinear balance, and audit actual APIC velocity/reference/weight buffers.
  GPU convergence histories distinguish stalls from oscillations. Any
  unconverged proposal preserves input flux and fails at the existing frame
  fence. There is no validation-only alternate simulation or CPU solver feedback.

Conceptually, with physical mixed divergence `B`, open
face metric `W`, integrated canonical flux `Q`, total liquid correction
pressure `lambda`, coarse air potential `p`, and mixed face gradient `G`:

```text
pressure = liquid ? lambda[leaf] : p[parent]
deltaQ = W G(pressure)
B (Q + deltaQ) + sweptVolume = 0
A(Q + deltaQ) cTrial >= qold - signedTolerance
0 <= cTrial <= 1; p >= 0; p*(1-cTrial) = 0
A(Q + deltaQ) cTransport = qold
newInventory = endpointCapacity * cTransport
```

The implementation uses mixed-face patch averaging, not a fine-only Laplacian.
It does not relax the physical filled-cell threshold or rewrite inventory.
The coupled test harness now requires actual peak per-cell transport excess
below `1e-11 m³` and coarse volume restriction error below `1e-14 m³`.
The signed certificate, applied physical divergence and actual transport bounds
are checked independently; passing one is not a substitute for the others.

This mode is not yet accepted for production, flowing particle/grid ownership
or performance. Earlier failures have exposed distinct problems: redundant
liquid/capacity coordinates, missing internal-face relaxation energy, signed
transport error, FP32 restriction mismatch, and paused capacity changes without
transport. These are fixed separately rather than waived by a broader tolerance.
Broad physical/temporal/optical acceptance and a much lower-overhead schedule
remain necessary before default promotion. See the scoped scheduling profile
below; it is not general real-time or full adaptive-system acceptance.

An independent hardware fixture verifies the borrowed V cycle against a dense
two-row reference and checks that original physical pressure is bit-identical.
That fixture proves reuse integrity, not moving-room acceptance.

The precision regression reproduces the failure mechanism without a GPU:
independently rounded coarse endpoints disagree with the sum of fine swept
volumes. A second test retains a tiny positive cut cell that a float coarse sum
loses. No tiny cells are deleted or altered to force a representable total.
Native collider tests separately exercise both legacy pause placement and
conservative paused/single-step/zero-step/reset boundary handling.

## Precision checkpoint validation (`4110327`)

`capacity-precision-validation.json` records this follow-up separately from the
historical receipt below. Its read-only collector rejects stale reports and
captures, checks all ten finite radiance/guide/caustic channels, and records
source, shader, executable and capture hashes.

- 199 CPU tests, seven native CTests, six hardware fixtures.
- All 14 coupled scenarios pass: 933 rendered frames covering initial/empty
  states, gravity start, calm water, falling water, room inlets with and without
  surface tension, moving-solid wakes/closure, controls, pause/single-step/reset,
  adaptive particle/ray work and the bulk overlay.
- Peak independently transported per-cell excess is `5.90292e-14 m³`;
  maximum applied physical divergence is `8.13199e-7/s`. The signed phase
  certificate remains below its `1e-12` normalized gate.
- Old air initial/controls and the implicit short baseline pass. The latter
  still reports its historical `0.00625802 m³` excess, not a bounded-liquid claim.
- Normal launcher checks cover three 48-frame launches and an interactive
  resize/minimize/restore/shutdown run. Single captures have been visually
  inspected; no temporal image-quality or raw performance acceptance is implied.

Instrumented room runs still spend approximately 56 ms/frame in the capacity
correction. This is a correctness checkpoint, not a real-time performance
milestone. Next work should reduce that schedule/iteration overhead, then
establish conservative flowing particle/grid handoff. Sparse physical storage,
geometric free-surface transport, broad long-run/temporal acceptance and the
rest of the adaptive-system specification remain open.

At that checkpoint the next task was to inspect warm starts, inner-cycle work and predicated dispatch/barrier cost before
micro-optimizing math. The compiled `CarrierLiquidRhs` already branches around
non-liquid/non-leaf work; changing that source ternary alone is not an established
optimization. No raw A/B performance profile was performed for that checkpoint.

## Lower-overhead coupled scheduling

The liquid block now performs one borrowed V cycle before revisiting capacity,
instead of four. Final signed phase and physical divergence checks are unchanged,
as are the 256-outer-iteration safety cap and rejection of unconverged proposals.
This is an inexact block solve with the same acceptance test, not a lower-quality
pressure tolerance. No shader, medium, caustic, surface or particle-density
setting changes. Source and launch defaults outside the opt-in coupled mode
remain unchanged.

`pressureCyclesPerIteration`, `multigridCycles`, `recordedMultigridCycles` and
`maxOuterIterations` distinguish executed numerical work from the full predicated
schedule recorded by the CPU. Fewer inner cycles reduce both the active V-cycle
work and the dispatch/barrier stream for iterations that are skipped after
convergence. The current measurements do not isolate those two savings.

Reproduce controlled raw runs (no CPU fluid audits, FG off):

```powershell
./engine/profile-fluid-capacity.ps1 -Label reference -Cycles 4
./engine/profile-fluid-capacity.ps1 -Label optimized -Cycles 1
```

The acceptance receipt `capacity-schedule-validation.json` uses three alternating
4/1 pairs on the same executable and shaders, plus fresh every-substep captures
from all 14 conservation scenarios. The read-only collector
`record-capacity-schedule.mjs` reuses the complete precision acceptance checks,
checks profile workload equivalence and validates all ten optical channels.
The prior precision receipt remains a historical checkpoint rather than being
overwritten by a performance run.

RTX 5090 room/inlet/orbit results at 1920×1080, DLSS Balanced, frame generation
off (median of three runs; 120 frames each, 32-frame raw-timing warm-up):

| Measurement | Four-cycle reference | One-cycle schedule | Reduction |
| --- | ---: | ---: | ---: |
| Raw frame median | 95.71 ms | 54.02 ms | 43.56% |
| Fluid GPU median | 69.45 ms | 37.09 ms | 46.60% |
| Capacity correction GPU mean | 57.35 ms | 25.37 ms | 55.77% |

Camera tracing remains about 4.9 ms and photons about 0.66 ms: pressure work,
not water light transport, dominates this coupled workload. Active V-cycle
counts decrease despite more outer iterations; recorded V-cycle slots drop
from 245,760 to 61,440 per 240-substep run. This is a scheduling-only comparison
with unchanged shader hashes, not a claim that each predicated slot executes or
that the current coupled path reaches 60 FPS.

Fresh correctness acceptance: 199 CPU tests, seven native CTests, six hardware
fixtures plus two pre-allocation API range rejections. All 14 coupled cases pass
again (933 frames); maximum audited applied divergence is `9.85494e-7/s`, signed
phase deficit `9.99977e-13`, and peak transported per-cell excess
`6.55587e-14 m³`. All ten optical channels are finite with checked photon energy
accounting. The independent legacy air/implicit baselines, three launch paths
and 197-frame interactive resize/minimize/restore/shutdown check also pass.
The room capture was inspected for a single-frame sanity check; broad temporal
quality and debug-layer/GBV validation are still not claimed.

A resident-fraction initial guess was also tried, but its single raw room run
was within baseline variability; it was removed. There is no new temporal warm
start or solver history to invalidate on collision/reset changes.

## Historical checkpoint validation (`d19a3f9`)

`capacity-mac-validation.json` records source, shader, executable and report
hashes, with the rejected-room diagnostic alongside passing results:

- 191 CPU tests, seven native CTests, six hardware fixtures.
- Initial pit/room, empty, gravity-start, 120-frame calm and pause/reset controls.
- Gravity-start changes 77,874 faces, with final physical divergence
  `8.53e-7/s` and independently transported excess `3.73e-8 m³`.
- Controls audit seven real transport substeps; idle grid rebuilds add none.
- Old air initial/controls pass. The implicit baseline still reports its known
  `0.00625802 m³` excess; that is baseline preservation, not bounded acceptance.
- Normal launch paths pass 48 frames each; the interactive launcher passes 202
  frames, resize, minimize/restore and clean shutdown.

This is not a GPU-debug-layer/GBV clean claim, temporal image-quality acceptance,
or a performance profile. Mainline, normal launch flags and portable release
remain unchanged.
