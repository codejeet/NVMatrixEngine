# Pressure-flux bulk transport checkpoint

`--fluid-bulk-projected` connects the existing **passive** bulk inventory to the
cut-pressure solver's canonical shared-face flux and current/previous geometric
capacities. It implies bulk inventory, cut cells, mixed MAC MGPCG and adaptive
fluid rendering. Relaxation-only pressure and synthetic bulk fixtures are
rejected for this mode. Existing launchers and default particle ownership remain
unchanged.

This is a prerequisite for flowing grid-owned interiors, **not** completed Narrow
Band FLIP or a bounded liquid volume-of-fluid solver. The APIC particles still
own physical water, pressure classification and the canonical DXR surface.

## Existing-system connections

| Producer | Bulk consumer |
| --- | --- |
| `FluidMac::projectedVolumeFlux()` | Restrict four canonical fine face fluxes into each coarse face in FP64. The physical open aperture is already included; do not multiply by it again or use the extrapolated FP32 velocity cache. |
| `FluidCutCellGpuView` | Current/previous open m³, including partial final cells and moving-solid endpoints. The same per-projection `swept` flag selects the old donor volume only for a new simulated endpoint. Cached/reset/paused placement uses current capacity. |
| Fine cut-cell volumes + MAC velocity delta | Open-volume-weighted bulk momentum source. This is still an explicitly ledgered replica force, not two-way conservative pressure coupling. |
| `FluidSystem` substeps | Geometry binding follows each recorded endpoint; transport follows projection/G2P. Existing UAV barriers and frame fences are reused. No new normal-play CPU readback or queue synchronization. |
| Existing bulk transfer/reduction/debug | Shared conservative face transfers, donor limits, source ledger, F9 volume/speed view and small asynchronous metrics. Report excess against actual open capacity. |
| DXR/photons/ReSTIR/DLSS-RR | No changes to optical authority, light transport or history. Fixed-point-photon paired captures independently test this isolation. |

Only canonical rates/restriction are FP64. Inventory, momentum source, donor
limits and final shared transfers remain FP32. The GPU view explicitly marks
whether its face-rate resource contains float or double elements.

## Local and global correctness are separate

For prescribed oriented flux `F`, a donor transfers the same fraction of all
four conserved quantities `(V vx, V vy, V vz, V)`:

```text
transfer = dt × F × donorLimit × inventory / oldOpenVolume
newInventory = oldInventory + incomingTransfers − outgoingTransfers
```

Where the pressure flux satisfies geometric conservation,
`sum(outward F) + (newOpenVolume − oldOpenVolume)/dt = 0`, this preserves a
spatially constant liquid fraction and velocity **if the donor limiter is
inactive**. Using current capacity for a swept update, or multiplying the flux
by aperture again, breaks that identity even though global mass still balances.
Independent manufactured tests exercise both regression traps.

The 95% donor limit preserves positivity under excessive outflow, but is not a
local substep or bounded VOF method. Its activation can break constant-fraction
preservation. Zero-capacity donors retain inconsistent inventory as reported
excess instead of deleting mass or dividing by a tiny number for transport.
The diagnostic fraction alone uses a `1e-20 m³` denominator floor, so residual
inventory in a fully closed cell appears as a very large finite ratio.

Nearest-cell initial/source particle aggregation can already overfill a cell.
The independently advected replica can also leave the particle-defined region
where pressure enforces incompressibility. Moving solids can close cells that
still contain replica inventory. None of these issues is repaired by merely
using the correct projected flux. The report exposes `excessVolumeM3`,
`overfilledCells`, `maxVolumeFraction` and limiter activations; they are **not**
accepted as physically bounded liquid support.

Required before flowing ownership: capacity-compatible conservative initial and
inlet allocation; a transported liquid fraction participating in pressure/free
surface classification; bounded shared phase fluxes; conservative grid/particle
mass and momentum exchange; and consistent reconstruction/reseeding. Clamping
inventory, suppressing excess diagnostics or calling the replica authoritative
would not meet those requirements.

## Validation and cost

```powershell
.\test-bulk-projected.ps1
.\test-bulk-projected.ps1 -Mode legacy -CaseFilter '^(calm|room|wake|adaptive)$'
.\test-fluid-bulk.ps1 -CaseFilter '^(translation|high-cfl|persistent)$'
.\profile-fluid-bulk.ps1 -Projected -Tag bulk-projected-cost -Repeats 3
```

```bash
node --test engine/*.test.mjs
node engine/check-bulk-projected.mjs RUNTIME
```

Opt-in snapshots independently check canonical fine/coarse flux restriction,
old/current fine/coarse volume restriction, open-volume-weighted momentum
sources, donor denominators/limits, shared transfers, cell updates, excess
volume and reductions. Final-frame snapshots add readback overhead and are
excluded from cost profiles. Normal play reads the existing 64-byte totals and
timestamps after the existing frame fence.

Verified on RTX 5090 in [the recorded audit](bulk-projected-validation.json):
138 Node/reference tests, seven Windows CTests, ten projected GPU scenarios,
four matched legacy scenarios and three original bulk transport stress fixtures.
The new final-substep canonical restriction error is zero in all ten scenarios;
maximum relative mass error is `4.05e-7`. Current/previous capacity restriction
and independently reconstructed force sources also pass their numerical gates.
All four paired scenes are exactly equal in authoritative diagnostics and all
ten captured lighting/guide channels ([capture audit](bulk-projected-parity.json)).
This is final-capture isolation evidence, not a new full temporal-sequence audit.

Three interleaved 300-frame room/inlet/orbit/foam runs per mode, 1080p Balanced,
FG off, normal bins/float atomics and no full validation snapshots:

| Median across runs | Cut-pressure baseline | + projected bulk |
| --- | ---: | ---: |
| Raw GPU frame | 21.4066 ms | 21.6426 ms |
| Fluid GPU time | 10.2388 ms | 10.2854 ms |
| Bulk pass mean | — | 0.04745 ms |

The room inventory/transport resources occupy 2,335,176 logical GPU buffer
bytes, excluding reused cut/MAC resources and validation readbacks. This adds
work; it is not a performance preset or 60-FPS raw-render acceptance.

Local acceptance remains **open**. Using the same actual cut-capacity observer
for both modes, room excess changes from 2.53422 to 2.92246 m³ (worse), while the
wake changes from 0.0425206 to 0.0128256 m³. The latter still has 0.0121657 m³
of replica inventory in closed cells. Calm initial overfill is 0.431271 m³ in
both modes. These mixed results are retained explicitly: fixing the flux
contract is not enough to produce a physically valid Eulerian liquid fraction.

The Windows debug component was unavailable at the preceding checkpoint
(`0x887A002D`); no OS/driver changes or new GBV-clean claim are made here.

The mathematical references are [Narrow Band FLIP](https://visualcomputing.ist.ac.at/publications/2016/NarrowBandFLIP/)
and [mass/momentum-consistent staggered-grid VOF](https://arxiv.org/abs/1811.12327).
They motivate coupled bulk/surface transport and consistent momentum flux, not
a claim that the current replica implements either complete method.
