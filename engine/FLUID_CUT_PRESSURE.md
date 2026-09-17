# Cut-cell pressure integration (experimental, opt-in)

Follow-up: [boundary-aware density kernels](FLUID_DENSITY_KERNEL.md) replace the
voxel-quadrature support described in this historical checkpoint. Its measured
results below remain labeled with their original implementation scope.

`--fluid-cut-pressure` connects the geometric solid capacities to the existing
mixed MAC projection and MGPCG. It changes actual projected velocities, not just
debug geometry. Ordinary launchers and pressure modes remain unchanged.

## Integration map

| Existing system | New connection |
| --- | --- |
| `FluidCutCells` | Shared physical face areas and current/previous open volumes feed pressure classification, flux assembly and physical residual normalization. |
| `FluidColliderTimeline` | A separate simulation-start pose anchors swept capacities across zero-step render frames, resets and paused placement. Substeps retain immutable upload slices. |
| `FluidSystem` | Selects cut-aware classification, forces, viscosity and curvature kernels; reuses P2G/G2P, collisions, density repair and resource barriers. |
| `FluidMac` | Uses area-weighted face fluxes and actual cell volumes. Partial cells and a guard band remain fine; full interior cells retain the existing 2:1 pressure/velocity DOFs. |
| `FluidMacPressure` | The same MGPCG uses explicit air-boundary conductance and control volume. Its Galerkin hierarchy and smoothing remain FP32. A cut-only sidecar preserves precise physical RHS and six-face conductances. |
| Future bulk transport | A persistent GPU buffer preserves projected shared-face volume flux in m³/s, separate from the rounded APIC velocity cache. It is not yet consumed by authoritative bulk transport. |
| DXR / caustics / RR | The existing canonical reconstructed particle surface remains the optical boundary. No new denoiser, surface shortcut or photon transport path. |

## Numerical contract

For a cell with open volume `V`, shared open face area `A`, center distance `d`
and outward signed velocity `u`, solve:

```text
sum_faces(A * u_after) + (V_current - V_previous) / dt = 0
u_after = u_before - dt / density * pressure_gradient
```

The volume-change term is applied only for a newly simulated geometry endpoint.
Initial/reset geometry, paused placement, cached substeps and render-only poses
do not create another swept source. The closed simulation domain never becomes
an outlet merely because its geometric cross-section is open.

Face area weights both divergence and the pressure operator. It does **not**
multiply the velocity correction a second time. Fine/coarse junctions retain
the original adjoint shared-face formulation and compatible eight-child
prolongation. Partial terminal cells use their actual center separation.

`MacRow` explicitly carries `boundaryDiagonal` and `volumeUnits`; the multigrid
hierarchy no longer infers unit Dirichlet weights or assumes every leaf's volume
is exactly one/eight full cells. This contract is shared by cut and legacy MAC.

### Precision is part of the flux contract

A moving-sphere audit exposed a cell with roughly 0.001 of a full cell's open
volume and pressure around 152 kPa. FP32 RHS rounding was large enough to pass
the rounded system's convergence test while missing the physical flux equation.
Cut-aware MGPCG therefore preserves the physical target and regular fine-cell
conductances in a separate 64-byte-per-cell FP64 buffer. The approximation used
for preconditioning does not replace this target.

The projected shared-face volume flux also remains FP64 on GPU. APIC continues
using its existing FP32 velocity transfer cache; its rounding-induced divergence
and absolute flux discrepancy are reported separately. Internal coarse-child
faces are prolongation/cache values, not additional coarse pressure DOFs.
Do not advertise the precise flux residual as the precision of every rounded
particle-transfer velocity.

The positional density projection must also retain partially open cells as
active DOFs. Its missing-solid kernel support now uses cut-cell capacities,
and its displacement stencil respects fully closed apertures. This is a
voxel-quadrature approximation of the solid kernel, not an exact integral or a
liquid-volume-fraction transport method. Leaving the old center-solid classifier
in that consumer caused a room-floor compression regression despite a converged
velocity projection; pressure convergence alone is not physical acceptance.

Cut mode permits up to four nonlinear density repairs per substep. Each repair
rechecks current post-contact error on GPU and generates zero work when no repair
is requested. Current bins are reused between repairs. The normal path retains
its one-repair limit. `densityRepairsLastSubstep` reports actual requests, not the
configured maximum; hitting the limit is not a claim of nonlinear convergence.

## Validation and remaining scope

`test-cut-pressure.ps1` runs independent geometric, pressure, hierarchy, projected
flux, particle and optical-surface audits. `cut-pressure.test.mjs` checks weighted
adjointness, Galerkin energy, manufactured projection and sliver precision.
Windows collider tests cover a distinct simulation anchor on zero-step frames.
Validation is expensive and is not a performance mode.

This remains a solid-boundary pressure milestone, **not Narrow Band FLIP**.
Liquid occupancy is still particle-derived, not a conservative liquid volume
fraction. Particles and the existing positional density solver still own liquid
mass. Fully closing cells, fast solid CCD, conservative source allocation,
grid/particle handoff and flowing coarse ownership need further work. The
sampled solid SDF still has the thin/subgrid limitations described in
`FLUID_CUT_CELLS.md`. No arbitrary-geometry conservation claim follows from a
small set of converged room/wake runs.

The finite-volume/variational separation follows the conceptual discussion in
[Batty, Bertails and Bridson's coupling work](https://www.cs.ubc.ca/labs/imager/tr/2007/Batty_VariationalFluids/).
This is original engine-integrated code, not a claim to implement their full
two-way coupled method. The full `ADAPTIVE_REQUIREMENTS.md` audit remains open.

## Measured checkpoint — RTX 5090 / Windows Release

127 Node tests, seven Windows CTests, all 11 cut-pressure operator/geometry
cases, and four legacy-MAC regression cases pass. The cut cases cover empty
water, static/coarse water, dormant owners, LOD changes, falling water, room
inlet, moving sphere, pause/single-step/reset, relaxation fallback and the
resampling/sparse-work/adaptive-ray/ReSTIR-PT combination. Captured DLSS guides,
caustic energy and procedural roots are also checked. Normal room startup passes.

All MGPCG substeps in these runs meet the unchanged `1e-4 / second` target without
exhausting the iteration budget. The independent canonical-flux divergence peaks
at approximately `9.98e-5 / second`. Relaxation is a bounded fallback, not a
convergence claim. The pressure precision-sidecar matrix matches independent
physical face assembly to the reported precision.

The moving wake ends at 1.00061× peak kernel density with exactly 100,000 rest-mass
units. Calm water and dormant owners retain 96 actual coarse pressure leaves and
request zero nonlinear repairs. However, particle acceptance is **not universal**:

| Matched deterministic case | Legacy peak density | Cut peak density | Surface-volume difference |
| --- | ---: | ---: | ---: |
| Room inlet, no resampling | 1.11377× | 1.25103× — fails existing comparison | 0.536% |
| Inlet + adaptive resampling/work/PT | 1.11365× | 1.00962× — passes comparison | 0.214% |

Both conserve issued particle rest mass and pass the existing 1% surface-volume
comparison; that does not excuse the first row's density regression. The voxel
solid-kernel approximation and nonconforming particle interpolation near cut
boundaries need further work before default promotion or flowing bulk ownership.

Three interleaved pairs, 300 frames each, 1080p Balanced, FG off, normal unordered
particle bins, inlet + orbit + whitewater, no full-grid validation:

| Median of three run medians | Legacy mixed MAC MGPCG + geometry observer | Cut pressure |
| --- | ---: | ---: |
| Raw frame | 16.758 ms | 20.730 ms |
| Fluid simulation | 6.749 ms | 10.035 ms |
| Pressure (median of per-run means) | 3.730 ms | 3.892 ms |

This measures added boundary fidelity/correction cost, **not a speedup** and not
a comparison against the ordinary faster Jacobi launcher. Most of the added
fluid time is outside pressure, including the bounded density repairs. Cut mode
does not yet meet the initial 60-FPS combined target in this profile. Logical
MAC default-buffer storage in the room is 38,982,432 bytes plus the existing
MGPCG hierarchy and 3,506,020 bytes of geometric capacities; heap alignment and
validation snapshots are additional.

Two 320-frame resampling/sparse-work/adaptive-ray/whitewater sequences pass the
existing temporal `--preserve` gates against same-build legacy MGPCG. Maximum
brightness change is 0.568%, raw motion-delta increase 0.054%, and RR motion-delta
increase 2.027%; camera-only frames retain water history and one RR reset. These
are scoped motion-compensated image checks, not proof of unbiased transport or
pure Monte Carlo variance. Actual generated-frame presentation is outside this
checkpoint. GPU validation was attempted again but initialization failed because
the Windows debug component is unavailable (`0x887A002D`); no GBV-clean claim.

Reproduction:

```powershell
.\test-cut-pressure.ps1
.\test-cut-pressure.ps1 -Mode legacy -CaseFilter '^(room|adaptive|calm|relaxation)$'
.\profile-cut-pressure.ps1 -Repeats 3
.\test-water-temporal.ps1 -Name water-temporal-cut-reference -MacMultigrid -Resample -SparseWork -Optical adaptive -Whitewater
.\test-water-temporal.ps1 -Name water-temporal-cut -CutPressure -Resample -SparseWork -Optical adaptive -Whitewater
```

```bash
node --test engine/*.test.mjs
node engine/record-cut-pressure.mjs RUNTIME
node engine/validate-water-temporal.mjs RUNTIME water-temporal-cut water-temporal-cut-reference --preserve
```

[Scoped evidence and binary/shader hashes](cut-pressure-validation.json) and
[temporal measurements](cut-pressure-temporal.json) retain both passing checks
and the failed room-density comparison. Default promotion is explicitly false;
ordinary launchers and the portable release remain unchanged.
