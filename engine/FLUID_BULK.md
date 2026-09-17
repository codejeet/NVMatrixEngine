# Adaptive checkpoint 3: persistent bulk inventory

For the later opt-in canonical pressure-flux/cut-capacity connection, see
[Pressure-flux bulk transport](FLUID_BULK_PROJECTED.md). The description below
documents the original `--fluid-bulk` mode, which remains available unchanged
in authority and scheduling.

`--fluid-bulk` enables a **passive, independently transported replica** of liquid
rest volume and linear momentum. It is not additional physical water. The APIC/
FLIP particles still own the simulated fluid and rendering surface. The ordinary
and adaptive launchers retain their previous behavior and cost.

Use `Play Bulk Inventory Lab.cmd` for inspection. **F9** cycles off, rest-volume
ratio and speed wire overlays. Blue/red means low/high; magenta in the volume
view marks cells above 101% of their geometric capacity. These are x-ray
diagnostic cells, not liquid geometry. **P**, **.**, **B** and the wall inlet retain
pause, single-step, reset and emission behavior.
The wire view omits cells below 1% capacity to avoid covering the screen with
tiny numerical tails; all such cells remain in transport and conservation totals.

## Concrete integration

| Existing system | Connection |
| --- | --- |
| `FluidSystem` / APIC bins | Seed once after reset; inject only the inlet's newly born particle-ID range. Each particle contributes to exactly one 2³ fine-cell aggregate. |
| Projected MAC faces | Sum area-weighted face flux onto coarse faces after each fine substep; clip physical areas at odd/partial domain edges. |
| Pre-force and projected velocity | Volume-average the MAC velocity change into an explicitly recorded bulk momentum source. |
| Solid SDF grid | Close coarse-face subfaces whose adjacent fine cells are solid; the outer domain is closed. |
| `FluidBulk` / `bulk.hlsl` | Own persistent ping/pong inventory, source ledger, shared face transfers, donor limits, reduction buffers, timings and debug PSOs. |
| Renderer / frame fence | Reuse resource helpers, HLSL compilation, PIX events and existing queue synchronization; collect small GPU totals after the existing fence. |
| DXR / caustics / DLSS | No changes to optical geometry, material transport, sampling or history. |

This is a bounded dense **coarse inventory grid**, not yet a sparse authoritative
MAC level. The GPU view exports current inventory, source ledger, coarse face
rates and physical bounds, all in UAV state. Face rates become valid after the
first transport substep. Normal execution has no full-grid or particle readback.

## Quantities and conservation

Each cell stores `(V vx, V vy, V vz, V)` where `V` is liquid rest volume in m³.
Multiply by material density to obtain linear momentum and mass. These are
integrated quantities, not point samples or a density that can safely be clamped.

1. Reset seeds the initial inventory and ledger from particle bins. Births add
   only their own volume/momentum once, before the first substep. No source means
   no particle-to-inventory rebuild, even while the particles continue moving.
2. The existing pressure-projected MAC velocities prescribe transport. Each
   coarse face sums fine `area × velocity`, retaining the fine/coarse flux
   cancellation identity. Closed solid subfaces contribute zero.
3. A first-order donor-cell finite-volume update transports all four quantities
   with the **same** transfer coefficient. Each face transfer is stored once and
   applied with opposite signs to its neighbors.
4. A donor's combined outgoing CFL limits total outflow to at most 95% of its
   inventory. This preserves nonnegative volume, including multi-axis flow and
   the high-CFL stress fixture. It is a safety limiter, **not** accurate adaptive
   substepping; every activation is reported.
5. The ledger records injected quantities and accumulated MAC-derived momentum
   sources. Global inventory must match the ledger; total volume must also match
   the independent CPU-side birth count times fixed particle rest volume.

No excess-volume saturation, negative-volume clamp, mass renormalization, particle
deletion or hidden reconciliation with the particle field is used.

## Important limits exposed by this checkpoint

**Conservation is not incompressibility or correct surface tracking.** A cell's
rest-volume/geometric-capacity ratio can exceed one. Initial nearest-cell particle
aggregation is noisy, coarse cells mix solid/liquid/air, and the fine velocity
extension is not divergence-free throughout the replica's independently evolving
support. Live pit/room tests expose local overfill despite accurate global mass.
These errors are reported and highlighted, not removed by throwing away liquid.

The momentum source is a coarse average of the existing MAC update, not a second
pressure solve or conservative force exchange between two authoritative levels.
Closing solid subfaces does not implement moving-solid swept-volume displacement
or cut-cell capacities. The grid has no own velocity evolution, free-surface
reconstruction, two-way rigid coupling, angular-momentum transfer or ownership
transition yet. First-order transport is diffusive and unsuitable as the final
visible surface tracker.

Consequently **this field must not yet classify particle-free liquid or replace
interior particles**. Next gates are geometric liquid/solid capacity tracking,
pressure-compatible bulk support and conservative fine/coarse momentum transfer,
followed by a gradual grid/particle ownership handoff. Those gates precede
reseeding, merging and any claimed adaptive speedup.

## Validation and reproduction

```powershell
.\test-fluid-bulk.ps1
.\profile-fluid-bulk.ps1
.\test-water-temporal.ps1 -Name water-temporal-bulk -Whitewater -Adaptive -Bulk
```

```bash
node --test engine/*.test.mjs
node engine/validate-water-temporal.mjs RUNTIME water-temporal-bulk water-temporal-adaptive --preserve
```

`--fluid-bulk-validate --frames=N` captures only the final frame's full inventory
and final substep inputs. CPU checks independently reconstruct donor limits,
shared face transfers, cell updates and GPU reductions, and compare coarse rates
directly against the captured fine MAC faces/solid masks. Normal runs reduce to
64 bytes of metrics plus timestamps; their invariants are checked every frame.

Bounded fixtures use `--fluid-bulk-fixture=1|2|3`: periodic translation, high-CFL
periodic transport and zero flow. The zero-flow test requires the inventory to
remain equal to its source ledger per cell while ordinary APIC particles keep
falling, proving that the persistent field is not rebuilt from those particles.
Fixtures affect only the passive inventory, never game water or rendered optics.

Profiling alternates off/on order across three 1080p Balanced room/inlet/foam
runs per mode, FG off. Frame/fluid medians discard 32 warm-up frames; bulk means
include all 300 frames. Full validation snapshots are excluded. This measures
added infrastructure cost, not a fluid optimization.

### Measured checkpoint — RTX 5090, Windows Release, 2026-09-12

- All 64 Node tests, six Windows CTests and 13 bounded GPU cases pass. The GPU
  matrix covers empty fluid, falling water, room/inlet/foam, three transport
  fixtures, pause/single-step/reset/F9, inlet reset, both optional pressure modes,
  wire rendering, frame-generation handoff and ReSTIR PT.
- Maximum relative mass error across those captures is `4.05e-7` (less than
  **0.00005%**). The zero-flow fixture retains its per-cell source inventory
  through 240 independent substeps; inlet births and resets match particle counts.
- Live coarse rest-volume ratios exceed **3×** cell capacity in some locations.
  This is an unresolved local representation/coupling error, not acceptable
  incompressible water. It is why the inventory remains passive.
- Three paired profiles measure bulk work at **0.0500 / 0.0438 / 0.0416 ms** per
  frame. Fluid medians are **1.947 / 1.953 / 1.960 ms** off versus
  **2.002 / 1.998 / 2.000 ms** on. All runs retain 105,899 particles and 600
  substeps. No FPS improvement is claimed. These profiles preceded only the
  validation snapshot extension and debug-tail culling; transport kernels and
  the disabled-overlay rendering path did not change.
- The 320-frame orbit/rolling sequence passes preservation gates against
  `water-temporal-adaptive`: brightness changes stay below **0.028%**, maximum raw
  temporal-delta increase is **0.198%**, and RR increase is **0.489%**. Camera-only
  phases do not advance water or reset caustic history; RR resets once.
- Room inventory uses 2,103,516 logical default-heap buffer bytes, excluding
  committed-heap alignment, PSOs/queries, upload/readback and validation staging.
  Full snapshots are opt-in and intentionally excluded from cost measurements.

See [the reproducible validation record](bulk-validation.json). The mainline game
and portable release are unchanged.

All transport, compute and debug shader variants compile; only the existing
external Bullet/RmlUi build warnings remain. An explicit empty-fluid
`--gpu-validation` probe on this build still fails before rendering with
`0x887A002D` (debug layer unavailable). Engine invariant checks do not constitute
a clean D3D12 debug/GBV run; no OS or driver settings were changed.

## References

- [Narrow Band FLIP](https://visualcomputing.ist.ac.at/publications/2016/NarrowBandFLIP/)
  motivates retaining a grid representation for particle-free interiors and
  validating the velocity handoff before removing particles.
- [Extended Narrow Band FLIP](https://pub.ista.ac.at/group_wojtan/projects/2018_Sato_XNBFLIP/XNBFLIP.pdf)
  informs future surface/representation transitions; those transitions are not
  implemented here.
- [LeVeque's finite-volume materials](https://www.clawpack.org/fvmhp_materials/)
  provide the conservation-law/donor-cell framework. The DX12 code is original;
  no Clawpack, Python or other simulation framework is introduced.
