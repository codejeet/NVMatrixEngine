# Live narrow-band ownership

Run `Play Narrow Band Deep Pool Lab.cmd`, or add `--fluid-narrow-band` to a CUDA
fluid-room launch. It selects CUDA graph replay and the existing uniform pressure
solver by default. The fine/mixed pressure choices remain independent. Ordinary
launchers keep their previous all-particle path.

This mode **actually removes** calm interior particles. There are two physical
owners, sharing one FP64 volume/linear-momentum ledger:

- active APIC/FLIP particles at the surface, boundaries and disturbances;
- flowing 2x2x2-cell Eulerian owners in calm interiors.

The sparse pressure page pool is not another water inventory. Rendering samples
are not additional live simulation particles. Allocated particle-buffer capacity
does not shrink: freed IDs are recycled for restoration.

## Engine connections

`FluidSystem` supplies the existing particle/grid exchange buffers and rendered
previous positions to `FluidCuda`. The same CUDA/DX12 external fence now publishes
21 views atomically through `Solver`'s existing private transaction. No particle
positions, ownership decisions or restoration lists are read back to the CPU.

`fluid_cuda_narrow_band.cu` evaluates current spatial bins and the solid SDF.
Its occupancy guide filters combined volume fractions before clamping, avoiding
false air/compression flags when particle spacing does not divide the grid size.
This guide owns no water and never clips the physical volume or momentum.
Resolved velocity variation, APIC detail, free-surface proximity and swept solid
margins prohibit retirement. Immutable neighboring decisions add padding; a
physical-time dwell delays retirement, while disturbance promotion is immediate.
Uniform translation is eligible; this is not a stationary dormant-particle proxy.

Retirement transfers volume and momentum, clears dead particle/cache records and
rebuilds bins. Point-bin overflow stays in a fractional particle owner rather
than overfilling a grid owner or clipping the physical quantity. Restoration
reserves recycled **issued** IDs atomically, validates collision-safe sites before
changing either owner, and returns the arithmetic remainder to the last particle.
Unissued emitter slots remain untouched. If capacity or safe sites are unavailable,
the quantity stays grid-owned; it is never silently deleted. Rebirth waits until
a cell has eight base-particle volumes for a useful 2³ sample stencil. Smaller
advected remnants keep flowing on the grid and can accumulate into a later birth;
they do not exhaust the free pool with infinitesimal particles. Restored motion
history starts at the new position, not at the old occupant of the recycled ID.

The existing joint P2G and matched grid-return operators contribute grid water
to the MAC velocity and pressure field. Bounded geometric transport advances its
inventory using the same projected face rates. The cell-mass cache and positional
density measurement include both owners. Surface particles retain the original
anisotropic reconstruction; volume-weighted grid quadrature contributes to the
same continuous nodal field in retired regions. Partial owners shrink spatial
support instead of turning a tiny quantity into a full negative-phi cell. The
scalar is normalized by the symmetric anisotropy matrix's stretch bound so tiny
owners cannot create unbounded nodal gradients and unstable ray-root residuals.
Surface-LOD fingerprints include grid quantity and material motion changes.

That canonical field feeds the existing procedural BLAS, camera/photon DXR
intersection, water medium, caustics and DLSS reconstruction. There is no separate
screen-space or particle-sphere water rendering path.

## Validation and scope

`test-fluid-narrow-band.ps1` runs the CUDA ownership fixture and bounded rendered
deep-room reference/calm/inlet cases and a 300-frame moving-ball/boat run with
whitewater enabled. GPU cases cover real retirement/restoration,
nonzero linear momentum, fractional overflow, automatic disturbance promotion,
blocked restoration, grid-only pressure/density, source-slot protection,
direct/graph execution, pause/reset and whole-frame rejection. An incommensurate
particle lattice checks the occupancy policy independently of grid-aligned seeds.
Sub-FP32 remnants stay grid-owned instead of becoming zero-weight particles;
thin-interface regression cases exercise inverse geometry without intermediate
underflow or deleting physical inventory.

Reports distinguish issued source count (`fluid.particles`), live count
(`fluid.active`), grid-owned volume and cumulative retire/restore counts. Total
water is the sum of both owners; comparing only live particle count with initial
particle count is no longer a conservation test.

This is a single-phase, bounded-domain narrow-band implementation, **not** a
claim of completed arbitrary multiresolution liquid research. Mean-only grid
owners cannot exactly preserve arbitrary angular/APIC detail, so retirement is
error-gated. Fine capacities are Cartesian, not moving cut-cell GCL volumes;
moving solids are protected with restoration margins. The non-owning transport
occupancy guide is bounded separately from the physical ledger: point-binned
particle volume is not treated as an exact geometric cut volume. Extreme
teleports or a fully exhausted restoration pool can defer restoration. The
baseline's dense global MAC pressure solve and fixed backing allocations remain.

Particle reduction is not, by itself, evidence of a raw-frame speedup. Profile
the complete simulation and rendering before changing the ordinary defaults.

## Validated build — 14 September 2026

Windows Release, RTX 5090, CUDA 13.1 / SM 120. The same compiled lab and canonical
DXR shaders completed the following bounded checks (frame generation disabled):

| Deep-room case | Frames | Live particles | Grid-owned volume (m³) | Relative total-volume error |
| --- | ---: | ---: | ---: | ---: |
| All-particle reference | 96 | 900,000 | 0 | 0 |
| Narrow-band calm | 96 | 859,754 | 491.749 | 4.82e-14 |
| Narrow-band inlet | 96 | 861,624 | 437.775 | 1.89e-13 |
| Narrow-band rolling ball + boat | 300 | 861,505 | 363.908 | 7.08e-16 |

All four had zero bad fluid roots and zero truncated intersection traversals;
camera/through-rays and water photon paths hit the reconstructed liquid. The
calm reconstructed-volume estimate differed from the reference by 0.70% (2.5%
gate). The moving case also passed the whitewater inventory check. Particle
counts are current live counts, not cumulative retire events; the harness now
rejects a calm run which retires/restores without reducing its live population.

Also passed: four narrow-band GPU cases, 15 geometric-transport cases (252 plane
inputs, including the captured underflow regression), 38 mixed-MAC cases,
39 composed-solver cases, eight DX12/CUDA transfer cases, nine Windows CTest
checks and 235 JavaScript tests. Normal and narrow-band HLSL variants compile.
These are numerical/render integration checks, not a latency benchmark or GPU
validation-layer certification; the D3D12 debug layer/GBV is unavailable on this
Windows installation. No new elevation was requested.
