# Authoritative dormant-interior ownership

`--fluid-interior` opts into particle-free **resting** coarse cells. It implies
particle resampling and shared importance. The normal fluid and adaptive
launchers remain unchanged. This is a bounded ownership milestone, **not a
completed Narrow Band FLIP, multiresolution MAC, or advected VOF solver**.

The previously failed moving-solid comparison now passes after the
[substep-boundary and density-repair work](FLUID_BOUNDARIES.md). The strict
pre-correction reference gate is unchanged; a post-step reference gate and
independent current-state density audit were added. This resolves that scoped
regression, not general flowing bulk or multiresolution pressure. Do not enable
this mode by default or call it production-ready.

## Concrete integration map

| Existing subsystem | Connection |
| --- | --- |
| `FluidSystem`, `gpu_resources` | Persistent 2h ownership cells, GPU slot recycling, barriers/events and existing frame fence; no new framework or queue waits. |
| `FluidComplexity` | Previous-frame requested physics LOD; current particle occupancy and SDF guards override stale/frozen requests. Render bricks are four MAC cells with two-cell padding. |
| `FluidResampling` | Ownership transfers precede pair merge/split. Actual particle mass and coarse-owned mass are accounted separately, never double-counted. |
| MAC P2G/G2P, pressure, density, capillarity | Coarse cells supply separable mass/momentum footprints; all occupied liquid remains in the existing globally coupled fine pressure solve. |
| `FluidSurface` | A protected negative interior extension joins the particle-supported canonical scalar field, preventing artificial internal air interfaces. Existing procedural BLAS and DXR intersection remain the renderer's geometry. |
| Path tracing, photons, RR/FG | Continue consuming the same canonical surface/material guides. Ownership itself adds no alternate ray traversal or denoising path. |
| GPU profiler/debug input | PIX-friendly ownership events, timestamp/counter readback, cyan x-ray ownership view through `G`, and `F10` forced particle restoration. |

## Ownership and transfer contract

Each 80-byte owner stores rest mass, centroid, mean velocity, an affine velocity
matrix and a compact separable lattice measure. This is integration metadata;
there are no live particle-buffer samples in an owned coarse cell. The current
admission policy accepts complete, nearly resting tensor distributions only.
Weighted or deformed neighborhoods remain particles. A three-fine-cell wet/solid
guard protects the free surface and reconstructed anisotropic neighborhood.
Geometric fitting error is bounded to 1% of a MAC cell; this is an approximate
policy checked against density and optical-volume references, not exact
equivalence for arbitrary particle positions.

The density footprint is the product of three sums of quadratic B-splines.
For axis spacing `s` and lattice extent `n`, its APIC moment is
`Daxis = h²/4 + s²(n²−1)/12`. Momentum restriction and its transpose use that
same anisotropic moment. Deposits preserve mass, centroid and linear/angular
momentum, with an explicit kinetic-plus-affine-energy rejection gate.
Restoration recreates mass-one samples from this measure and reproduces the
owner's moments. It does not use clumped corner samples.

An earlier eight-node approximation passed global conservation but changed
local density and expanded the rendered calm surface by about 3.5%. It was
rejected. Conservation tests alone are not sufficient acceptance.

Current admitted spacing is fitted to the settled distribution, not copied from
the original spawn lattice. Coarse cells wake at 0.0001 m/s (or equivalent affine
motion), so this checkpoint explicitly targets dormant water rather than hiding
unresolved flow inside a fixed shape.

Coarse mass supplies exact integer child-cell quanta from the admitted measure,
not particle population and not equal eighths. Particle-only offsets/counts
remain valid neighbor ranges. Pressure, density classification and capillary
occupancy consume combined mass. `FluidBulk`, if enabled, is still a separate
passive diagnostic replica and contributes **nothing** to authoritative mass.

Current projected MAC velocities update the owner using GPU-compacted indirect
work: one cooperative group per active owner, rather than sparse active lanes
within a domain-wide expensive kernel. Motion, affine growth, displaced centroid,
incoming particles, a nearby collider or a fine-detail request restore particles.
This is a dormant-state approximation, not conservative liquid flux across a
coarse/fine boundary. Continuous flow and general deformation still need the
authoritative multiresolution MAC/volume-fraction system in the full spec.

## Scheduling and limitations

- Ownership exchanges run before simulation and after final collision/binning.
  GPU free IDs are reserved once per frame; restores finish before resampling
  allocates from its own refreshed free list. Newly emitted IDs are not recycled.
- GPU indirect arguments re-bin only after an actual ownership exchange. Active
  owners are compacted entirely on GPU. No owner positions are read back normally.
- Pause prevents admission/advection; an explicit F10 change or moving collider
  may still restore detail and rebuild geometry. Tests freeze rigid poses when
  checking that a fully paused scene retains its surface.
- Both MAC and canonical surface spacing remain uniform. Owner storage is dense
  within the bounded domain; active update work is compact. Sparse LOD grids,
  coarse/fine pressure interfaces and flowing particle-free regions remain work.
- FLIP is rejected for this mode until its distinct moment/temporal contract is
  implemented; ordinary `--fluid-resample --fluid-flip` still works.
- Owner restoration initializes previous particle positions locally. It cannot
  reconstruct a general prior deformed distribution; admission/wake bounds and
  deep-interior protection limit exposure. General adaptive surface motion must
  be addressed with the later advected ownership/field representation.
- Emitter capacity is still issued mass, not an unbounded GPU source allocator.
- `meanHeight` includes both particles and owners; `particleMeanHeight` explicitly
  reports the particle-only centroid. Both are mass-weighted.

## Validation

`test-fluid-interior.ps1` includes particle-only reference, empty, calm,
force-fine/pause/single-step, long calm, moving-solid, falling, room emitter,
multigrid, passive bulk, FG and PT cases. Calm/cycle optical volume must stay
within 0.25% of the particle-only reference and peak density within 0.01.
`interior.test.mjs` independently verifies separable weights, APIC moments,
MAC transpose reproduction and padded brick mapping.
The moving-solid sequence also runs uniform and resampled particle references:
the existing resampler itself changes the moving scene's reconstructed volume,
so uniform particles are the primary physical comparison for that case.

`--fluid-interior-validate --frames=N` performs expensive per-frame joint
particle/owner snapshots. Independent double-precision host sums check exchange
mass, linear/angular momentum and energy. Full fluid validation checks actual
bin quanta, particle-free pressure/density coverage, collision and DXR roots.
Normal timing/counter readback shares the renderer's existing fence.

The [full requirement audit](ADAPTIVE_REQUIREMENTS.md) remains authoritative.
No full-system completion or net frame-rate improvement follows from this
ownership checkpoint alone.

## Historical checkpoint — `7f89656`, RTX 5090 / Windows Release

The 100k calm fixture holds 6,624 mass units in 96 owners, leaving 93,376 live
particle samples. F10 and the moving sphere both restore all owners; the forced
fine policy also prevents the pair resampler from immediately coarsening them.
The 600-frame/1,200-substep calm run passes the matched-duration density/volume
gate. Joint exchange mass error is zero, and moving-solid exchange momentum and
energy discrepancies are below 1e-11 per the recorded normalizations.

74 Node tests, six Windows CTests and the six ordinary fluid GPU regression
fixtures pass, including the 2,400-substep run. The strict **moving peak-density
gate remains failed**, as described above; other scoped interior scenarios pass.
The normal release was tested, not GPU-based validation: the Windows D3D12 debug
component was previously unavailable (`0x887A002D`).

The 320-frame room orbit/rolling sequence passes preservation thresholds against
`water-temporal-resampling`: maximum brightness change 0.029%, raw motion-delta
increase 0.075%, RR delta increase 0.205%, with one RR reset. This shallow room
contains no coarse owners, so this checks renderer integration, **not** dynamic
ownership optical convergence or pure Monte Carlo variance.

Three interleaved on/off pairs per scene, 804 measured frames per mode, FG off,
1080p Balanced, no invariant snapshots or concurrent profiling:

| Median GPU ms | Resampling baseline | Dormant owners |
| --- | ---: | ---: |
| Calm: simulation | 1.770 | 1.839 |
| Calm: reconstruction | 1.407 | 1.404 |
| Calm: complete raw frame | 7.714 | 7.769 |
| Room/inlet: simulation | 2.068 | 2.160 |
| Room/inlet: reconstruction | 1.154 | 1.183 |
| Room/inlet: complete raw frame | 12.273 | 12.301 |

This is **not a net FPS improvement**. Cooperative compact owner updates removed
the initially expensive sparse-lane restriction pass, but the current domains
retain all fine pressure work and gain little additional sample reduction over
pair resampling. The room has no eligible interior and pays only integration
overhead. Individual full-frame run medians vary; use pooled timings and do not
attribute every total-frame difference to fluid simulation.

`profile-fluid-interior.ps1` reproduces these pairs. `record-fluid-interior.mjs`
summarizes existing bounded reports without launching workloads. The measured
data and explicit failed acceptance are preserved in `interior-validation.json`.

These measurements preserve the original failed checkpoint. The subsequent
boundary/density results and remaining limitations are in `FLUID_BOUNDARIES.md`.
They do not retroactively change `interior-validation.json` or establish a net
speedup for the new implementation.
