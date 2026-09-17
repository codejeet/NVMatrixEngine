# Solid cut-cell geometry prerequisite

Follow-up: `--fluid-cut-pressure` now opts into an actual weighted projection;
see [the pressure contract and remaining limits](FLUID_CUT_PRESSURE.md). The
geometry-only flag and historical measurements below retain their original scope.

`--fluid-cut-cells` builds reusable **solid geometry**, not a second copy of
liquid. It provides open cell volumes and shared face apertures on the fine MAC
grid and their conservative 2:1 restrictions. `K` toggles red-to-cyan partial-cell
wires; `--fluid-cut-view` starts with that x-ray overlay visible. Frame Generation
is suspended while this untracked diagnostic overlay is visible.
`Play Cut Cell Lab.cmd` enables the capacity and bulk observers without expensive
validation readbacks; press `K` or `F9` to inspect either representation.

The ordinary launchers, particle ownership, pressure operator, reconstructed
water, photon paths and DLSS inputs remain unchanged. This checkpoint is a
prerequisite for flowing bulk ownership, **not an implementation of that handoff**.

## Integration map

| Existing system | Connection |
| --- | --- |
| `FluidSystem`, `FluidColliderTimeline` | Update geometry at actual simulation endpoints, including moving rigid-body substeps; reset/paused placement initializes or updates the current endpoint. |
| `solid.hlsli`, `MeshSdfAsset` | Reuse the same sphere/box/capsule/cylinder/plane and imported mesh-SDF sampling as the particle solver. No separate collision framework. |
| `FluidCutCells`, `gpu_resources` | Persistent GPU buffers, root constants, existing shader compilation, UAV barriers, PIX regions and timestamp queries. No additional queue fence or particle CPU work. |
| Fine/coarse MAC interfaces | `FluidCutCellGpuView` exports current/previous volume and one aperture per shared face, dimensions/bounds, a frame `changed` flag and a per-projection `swept` flag. Pressure consumes these only with the separate `--fluid-cut-pressure` option. |
| `FluidBulk` | Capacity audit reads its current inventory without modifying it. Reports excess over geometric capacity and inventory inside fully closed cells; no clamping or reconciliation. |
| DXR, photons, RR | Continue seeing the existing canonical particle-water surface. This geometric observer adds no transport or filtering path. |
| Existing diagnostics/presentation | `K` wires, normal small metric/timestamp readback through the existing fence, bounded full-grid audits when explicitly requested. |

## Numerical contract

- Sample the union of solid SDFs at shared grid corners. Positive distance means
  open space. "Open volume" includes both air and water; it is **not water mass**.
- Divide each voxel into six tetrahedra sharing the 000–111 diagonal. Integrate
  the positive part of the linearly interpolated SDF analytically. A cut face is
  two triangles with the matching low–high diagonal on both neighboring cells.
- Opposite-sign edge interpolation avoids division by zero at equal/coincident
  vertices. The two-inside tetrahedral wedge is three disjoint tetrahedra, not a
  smoothed occupancy estimate or a sum of corner signs.
- Store physical m³ and m², accounting for clipped terminal cells in odd-sized
  domains. A terminal coarse face lies at `fineDimension`, not necessarily
  `2 * coarseDimension`.
- Coarse volumes sum up to eight fine volumes; coarse apertures sum up to four
  fine apertures. They are never independently re-sampled at coarse corners.
- Geometric apertures at the outer domain describe open cross-sectional area.
  They do **not** authorize outflow through the simulation's closed outer wall.
- Retain the previous geometry endpoint for swept-capacity work. Reset makes
  previous=current. On a cached frame `changed=false`: consumers must not apply
  a previous endpoint's volume difference as a new source. The reported absolute
  change is the last updated endpoint pair, not an integrated frame sweep.

Static transforms reuse the geometry buffers, even while particles move.
Changing rigid transforms currently rebuild the bounded geometry grid; work is
not spatially compacted yet. Normal readback is 64 bytes of metrics plus timestamps.
Validation is deliberately much more expensive and is not a performance mode.

This is exact for the **piecewise-linear sampled SDF**, not for arbitrary curved
geometry. Features that fit between sample corners can be missed. It is not yet
a conservative thin-solid classifier or a high-order moving cut-cell method.
The reusable interface makes these limitations explicit before any pressure or
liquid ownership depends on it.

## Validation

```powershell
.\test-fluid-cut-cells.ps1
.\test-cut-cells-parity.ps1
.\profile-fluid-cut-cells.ps1
```

```bash
node --test engine/*.test.mjs
node engine/check-cut-cells-parity.mjs RUNTIME
node engine/record-cut-cells.mjs RUNTIME
```

`--fluid-cut-validate --frames=N` independently checks the final geometry endpoint
of every rendered frame: collider/mesh samples, simplex integration, all shared
face areas, exact previous-volume copies, both restriction identities, global
reductions and optional bulk-capacity metrics. The four geometry-only fixtures
are `--fluid-cut-fixture=1|2|3|4`: axis plane, oblique plane, moving plane, sphere.
They do not replace physical colliders or change the simulated/rendered water.

CPU tests independently check planar volume against the analytic distribution
of a weighted sum of uniforms, all vertex permutations/sign complements,
coincident vertices, shared-face triangulation and partial terminal volumes.
GPU fixtures check analytic plane volumes and the convergence-scale error of a
contained sphere as well as replaying the sampled geometry in double precision.
The test matrix also includes mesh/primitive room geometry, inlet, wake,
pause/single-step/reset/K, the debug FG suspension, and the existing adaptive
particle/MAC/work/optical/PT combination.

The [machine-readable evidence](cut-cell-validation.json) records fresh
binary/shader hashes, every scoped GPU case, on/off render comparisons and three
interleaved non-validation profiles. This is a geometry cost measurement, not
an adaptive-fluid FPS gain. GPU-based validation remains unexecuted because the
Windows debug component was previously unavailable (`0x887A002D`).

### Measured checkpoint — RTX 5090, Windows Release

121 Node tests, seven Windows CTests and all 12 bounded GPU cases pass.
Static fixtures build geometry once; the moving-plane fixture updates all 120
simulation endpoints. Prior-volume copy error is zero. All ten captured raw
lighting, guide, DLSS output and caustic channels are bit-identical for on/off
comparisons of calm water, room/inlet orbit and the moving-solid wake.

Three interleaved on/off pairs, 1080p Balanced, 300 rendered frames per run,
FG off, no full-grid validation, ordinary unordered particle bins:

| Median of run medians | Disabled | Capacity geometry enabled |
| --- | ---: | ---: |
| Complete raw frame | 12.468 ms | 12.563 ms |
| Fluid simulation, including capacity work | 2.927 ms | 2.959 ms |

The median of the three capacity-pass mean timings is **0.0168 ms/frame**;
logical default-heap storage in the room is **3,506,020 bytes**, before heap
alignment and excluding validation/readback allocations. The full frame varies
between runs; the measured change is overhead, not an FPS improvement.

The 48-frame inlet case reports approximately 48.57 m³ in the passive replica,
1.91 m³ beyond local open capacities and 0.0022 m³ inside fully closed cells.
This is a failure of that **passive replica's local representation**, not a claim
that the authoritative particle water has gained or lost that mass. Tiny sliver
capacities produce very high local ratios; the unclamped excess-volume metric is
more informative than that ratio alone.

## Next dependency, not hidden by the new metrics

The passive bulk inventory can conserve global mass while exceeding local open
capacity. Its previous full-cell ratio does not detect all solid-volume errors.
The new diagnostics expose both overfill and inventory in fully closed cells.
Neither deleting that excess nor limiting it away would conserve correct water.

Before removing particles from flowing regions, implement compatible cut-cell
pressure/velocity support, liquid volume fractions, moving-boundary swept flux,
and conservative grid/particle mass and momentum exchange. The fine/coarse
operator must keep divergence and gradient adjoint under the same weights.
Current cell geometry alone establishes none of those physical properties.

The distinction between boundary geometry, pressure weights and coupled velocity
updates follows the conceptual separation in
[Batty, Bertails and Bridson's solid-fluid coupling work](https://www.cs.ubc.ca/labs/imager/tr/2007/Batty_VariationalFluids/).
This code is original; it does not claim to implement that complete solver.
The [full adaptive requirement audit](ADAPTIVE_REQUIREMENTS.md) remains open.
