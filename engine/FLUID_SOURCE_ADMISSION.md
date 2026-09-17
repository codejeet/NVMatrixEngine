# Capacity-bounded liquid source admission

`--fluid-bulk-capacity` extends projected bulk with a GPU source-admission
transaction. Initial water and inlet births first enter a persistent **pending**
volume/momentum buffer. A cell admits only what fits its current open capacity.
Unplaced requests are routed across the sampled coarse open-face graph, or stay
pending if capacity/connectivity or the iteration budget is insufficient.

This closes the initial/source-overfill prerequisite without clipping mass or
rewriting existing water. It does **not** make bulk advection bounded or transfer
physical ownership away from particles. Pending and resident bulk are both
replicas until the separate grid/particle handoff is implemented. Existing
launchers, APIC physics and the canonical DXR surface remain particle-owned.

## Integration map

| Existing system | Connection |
| --- | --- |
| `BulkSources` and particle bins | Inject each birth range once into pending requests and the independent source ledger. Reset clears resident inventory and replaces pending requests. |
| `FluidSystem` collider timeline | Admission runs at the simulation-start endpoint, before moving-solid geometry and pressure for the first substep. Current bins are reused by P2G. |
| `FluidCutCells` | Current coarse open volumes and shared open-face areas define admission capacity and graph connectivity. |
| `FluidVolumeAllocator` | Persistent pending quantities, aperture weights, shared transfers, GPU-controlled indirect dispatch, metrics and independent snapshots. Reuses the engine's resource/barrier/PIX abstractions. |
| `FluidBulk` | Existing resident buffers receive admitted quantities; subsequent force/advection passes touch residents only. Global accounting includes resident **plus pending**, while `gpuView().inventory` remains resident-only and `pendingSources` exposes the separate request ledger. |
| Frame fence/profiling | Uses the existing frame fence, small asynchronous reductions and the bulk timestamp heap. No new synchronization or per-particle CPU processing. |
| DXR, photons, ReSTIR, RR | Unchanged geometry/lighting authority. Paired deterministic captures test that isolation. |

The source allocator is separate from advection. An already overfilled resident
cell gets no new admitted water and is otherwise left alone. Therefore the
allocator cannot disguise transport, moving-cell closure or pressure-support
errors by redistributing existing liquid every frame.

## Conservative transaction

All buffers carry `(V vx, V vy, V vz, V)` in SI units. Each admission or routing
operation uses one coefficient for volume and all momentum components.

1. Admit `min(pendingVolume, max(0, openCapacity - residentVolume))` locally.
2. If significant routable requests remain, send at most half of each donor's
   pending quantity to open neighbors, weighted by their shared face apertures.
   The retained half prevents a bipartite routing oscillation.
3. Store one net transfer per face and apply it with opposite signs to the two
   neighboring cells. Admit newly arrived requests only into remaining capacity.
4. Repeat with GPU-generated indirect dispatch arguments, up to 64 iterations.
   Preserve every unplaced quantity for a later transaction.

This is source placement, **not** a physical diffusion/advection model. It can
move a request through a full cell without moving that cell's existing resident
water. Convex mixing may dissipate source kinetic energy but must not increase
it. Global linear momentum is conserved; angular momentum, minimum-displacement
placement and an exact reconstructed free surface are not established here.

The routing threshold is `particleRestVolume × 1e-7` per donor. It gates work
only. Sub-threshold requests stay in the pending buffer and in all mass/momentum
totals; they can be admitted when space opens. They are never discarded or
silently added to resident volume. Pending requests above the budget remain
explicit too. A future ownership transaction must resolve/transfer this ledger,
not interpret a small residual as permission to drop mass.

Pause performs no admission or routing. Opt-in validation of a paused frame
uses a read-only transaction snapshot, so the pause/reset control fixture also
checks the frozen case.

## Validation

```powershell
.\test-bulk-projected.ps1 -Mode capacity
.\test-bulk-projected.ps1 -Mode projected -CaseFilter '^(initial-calm|initial-room|calm|room|wake|adaptive)$'
.\profile-fluid-bulk.ps1 -Capacity -Tag source-admission-cost -Repeats 3
```

```bash
node --test engine/*.test.mjs
node engine/check-bulk-projected.mjs RUNTIME --capacity
```

The full source snapshot includes before/after resident and pending quantities,
open capacities and face apertures. An independent FP64 CPU replay checks the
actual admission/routing result, resident capacity bound, positivity, global
mass/momentum and non-increasing kinetic energy. First-frame calm/room cases
exercise initial oversubscription directly, not merely a quiet final frame.
Other runs cover inlet births, motion, resets, paused inspection and adaptive
particle/rendering paths. Reference tests also cover insufficient capacity,
disconnected graphs, competing sources, tiny cells and retained tiny requests.

Recorded results: [source audit](source-admission-validation.json) and
[render isolation](source-admission-parity.json). The current build passes 146
Node/reference tests, seven Windows CTests, twelve admission GPU cases and six
matched projected-bulk references. All six pairs match exactly in authoritative
particle diagnostics and all ten captured lighting/guide channels. This is
final-capture isolation, not a new full temporal-sequence audit or a GBV-clean
claim; the preceding Windows debug-component attempt failed with `0x887A002D`.

Initial resident overfill is zero in both the calm and room cases, versus
0.431271 and 2.10373 m³ before admission. After the first 64-iteration transaction,
0.00989353 and 0.000767244 m³ respectively remain pending and fully accounted for.
The largest independent source snapshot error is `2.29e-8` in volume/volume-weighted
momentum components; maximum relative source conservation error is `4.48e-9`.
No kinetic-energy increase was measured in these source snapshots.

Three interleaved 300-frame room/inlet/orbit/foam profiles per mode, 1080p
Balanced, FG off, normal bins/float atomics, no full validation snapshots:

| Median across runs | Projected bulk | + source admission |
| --- | ---: | ---: |
| Raw GPU frame | 21.6643 ms | 21.9717 ms |
| Fluid GPU time | 10.3249 ms | 10.5428 ms |
| Source admission mean | — | 0.24306 ms |

The allocator adds 1,470,832 logical GPU buffer bytes in the room, excluding
reused bulk/cut resources and validation readbacks. The bounded command sequence
still has overhead when indirect face/cell dispatches become empty; it is not a
zero-cost inactive subsystem or a performance improvement.

Later transport still fails local bounded-volume acceptance: room resident
excess is 2.21187 m³ after 120 frames, and the moving-solid wake retains
0.012837 m³ in closed cells. The wake's total excess slightly worsens relative
to the prior projected-bulk replica. These defects are retained in the audit,
not hidden behind the passing source-admission checks.

## Remaining ownership requirements

Resident transport still uses the projected bulk donor scheme. It can overfill
cells, leave liquid in newly closed cells, and move outside particle-defined
pressure support. Those errors remain reported. The coarse graph also does not
resolve disconnected sub-cell components; boundary refinement/component-aware
source placement is required before claiming arbitrary-geometry ownership.

Next requirements are bounded phase transport tied to pressure classification,
closing-cell treatment, conservative particle/grid mass and momentum exchange,
and consistent surface reconstruction/reseeding. This allocator provides an
admission primitive, not the completed Narrow Band FLIP system or a faster play
preset. Sparse per-region source work and a timing-budget policy remain future
optimizations.

Related [cut-cell state redistribution literature](https://arxiv.org/abs/2005.05734)
motivates keeping redistributed conserved quantities explicit. This original
source-admission algorithm is not that paper's linearity-preserving SRD method,
nor a VOF scheme for the physical advection step.
