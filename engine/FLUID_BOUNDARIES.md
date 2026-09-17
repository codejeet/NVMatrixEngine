# Substep boundaries and error-triggered density repair

This continues the adaptive-fluid implementation at `7f89656`; it is not a
completed multiresolution MAC solver or narrow-band fluid system.

## Integration

- `FluidColliderTimeline` owns only a bounded array of rigid colliders. It
  interpolates translation and shortest-arc rotation from the last simulated
  pose to the current render pose, with an immutable upload slice per substep.
  `FluidSystem` selects each slice through the existing root SRV and rebakes the
  solid MAC boundary. No particle readback, new queue, or frame wait is introduced.
- Boundary velocities use elapsed **simulation** time. Zero-substep frames retain
  the simulation anchor; paused placements, resets, and geometry replacement
  establish new anchors. Small-angle angular velocity uses `atan2`, avoiding
  `acos(w)` quantization. The final SDF transform exactly matches the renderer.
- The existing reconstruction, importance, resampling and ownership systems use
  the same `FluidGpuView` collider address. After simulation it points at the
  exact current pose used by the canonical scalar field and procedural DXR.
- Particle contacts are resolved before positional density projection as well
  as afterwards. The density solve sees displaced water rather than water still
  inside a newly moved solid. The full unilateral density error is used, with
  RHS bounded at 0.5 and the existing independent 0.2h displacement trust region.
- One additional density correction is requested entirely on GPU if a current
  occupied nonsolid cell has kernel density (including missing solid support)
  above 1.02. Generated indirect arguments control Jacobi, particle displacement,
  contact and rebinning. Empty requests skip their shader work. Classification
  and the error gather are still uniform, and a triggered solve remains globally
  coupled; this is **not yet a spatially compact or converged nonlinear solver**.
  Corrected bins are retained for the next substep's P2G, avoiding a redundant
  histogram/scan/scatter when no particle positions changed between those passes.

The need to incorporate obstacle displacement into density correction is also
visible in the [author's IDP-FLIP reference implementation](https://github.com/thunil/mantaflow/blob/master/source/plugin/implicitdensityprojection.cpp).
Our bounded contact/projection splitting is original engine-integrated code,
not that reference's full boundary formulation. Cut-cell solid capacities,
consistent swept-volume fluxes, two-way rigid coupling and CFL/CCD handling of
arbitrary fast objects remain separate work. Interpolation alone is not CCD.

## Diagnostics

`maxRelativeDensity` retains the original pre-first-correction measurement.
Validation-only `DensityMeasure` computes `postStepMaxRelativeDensity` from the
final particles and coarse owners after contacts and ownership exchanges. It
does not clear pressure or alter simulation state. Reports also include peak
cell coordinates/solid distances, post-step nonsolid peak, occupied volume, and
whether the final substep requested a repair.

Both peak locations are checked by an independent double-precision CPU gather
over current samples and the separable owner measure, not over GPU bins.
`meanHeight` now includes owner mass and position; `particleMeanHeight` explicitly
reports the particle-only centroid. These expensive checks run only during
bounded validation, not normal play or profiling.

## Investigation record

The 180-frame sphere fixture squeezes water against the right domain wall. It
exposed real nonsolid compression, not a checkerboard shading artifact or merely
stale reporting. Matched uniform-particle and owner runs, RTX 5090, 100k rest mass:

| Intermediate implementation | Uniform post-step peak | Owner post-step peak |
| --- | ---: | ---: |
| Original stepping + new audit | 2.60107 | 2.81103 |
| Substep-aligned colliders only | 3.04208 | 3.13507 |
| Contacts before density too | 2.47046 | 2.53471 |
| Full bounded density RHS too | 1.06713 | 1.11644 |
| GPU error-triggered repair too | 1.01246 | 1.00964 |

These are intermediate correctness experiments, **not benchmark speedups**.
In particular, collider interpolation alone did not cure compression. The
independent peak audit discrepancy in the last row was below 1e-6.
The original pre-correction density gate is retained, with an additional
post-step comparison; no reference threshold is relaxed to hide a regression.

## Validated checkpoint

- All 15 interior/reference cases pass, including the formerly failing moving
  sphere, 600-frame calm run, forced-fine/pause/single-step, room emitter, falling
  water, multigrid, passive bulk, FG and ReSTIR PT integration. Calm water retains
  96 owners / 6,624 mass units and requests **no** extra density solve; the moving
  sphere restores all 96 owners and requests repair. Issued mass remains 100,000.
- The final sphere run ends at pre-correction density 1.01375 and post-step 1.00962.
  The matched uniform reference ends at 1.01056 and 1.00713. Both unchanged
  pre-correction and new post-step relative gates pass, as does a new absolute 5%
  post-step compression limit. The resampled-only reference still differs from
  uniform water and is not substituted for it to obtain a pass.
- 75 Node tests and seven Windows CTests pass. CPU timeline tests cover skipped
  render frames, pause/reset, rigid midpoint/final endpoints, noncommuting
  rotations, slow rotation and quaternion hemisphere crossing.
- Six ordinary GPU regression cases pass: constant, affine, compression,
  roundtrip, controls and the 2,400-substep APIC run. Additional moving-SDF render
  and surface-control cases pass, including canonical DXR roots and photon checks.
- The 320-frame room orbit/rolling sequence passes `--preserve` against the prior
  `water-temporal-interior` sequence: maximum brightness change 0.321%, raw
  motion-delta increase 0.653%, RR motion-delta increase 2.566%, one RR reset.
  This is not pure Monte Carlo variance or proof of general dynamic ownership
  optical convergence; the shallow room has no eligible owners.
- Normal Windows Release was tested. GPU-based validation was **not** run:
  Windows Graphics Tools was previously unavailable (`0x887A002D`).

Three serial final room/orbit runs, wall inlet and whitewater enabled, 1080p
Balanced, FG off, no invariant snapshots: 804 measured frames have median raw
GPU frame time **12.567 ms**, p95 **15.567 ms**, simulation **2.957 ms**, and
reconstruction **1.159 ms**. This is the current operating point, not a controlled
speedup comparison against the old compression-buggy solver. The correctness
repair has a cost; fully sparse/local projection remains the optimization path.

`boundaries-validation.json` records fresh bounded reports; the independent image
comparison is preserved in `boundaries-temporal.json`. Reproduce with
`test-fluid-interior.ps1`, the scoped `test-fluid.ps1` / `test-fluid-render.ps1`
cases, `test-water-temporal.ps1 -Name water-temporal-boundaries -Whitewater
-Adaptive -Interior`, and `validate-water-temporal.mjs <runtime>
water-temporal-boundaries water-temporal-interior --preserve`.
`record-fluid-boundaries.mjs` reads those reports without launching tests and
rejects reports older than the executable. Current timing runs use
`profile-room.ps1 -Tag boundaries-final -Repeats 3 -CaseFilter '^orbit$'` with
profiling run serially and without simultaneous builds or invariant tests.
The old `interior-validation.json`
remains a historical record of the failed `7f89656` checkpoint.

General cut-cell volume conservation, flowing coarse ownership, multiresolution
MAC coupling and budget-controlled spatial work remain outstanding. A bounded
extra projection is not a guarantee that arbitrary impacts converge below 2%.
