# Boundary-aware density support (experimental cut-pressure path)

This continues `6919305`. It does not complete the adaptive-fluid specification,
Narrow Band FLIP, or a conforming particle-velocity interpolant.

## Integration map

- `FluidCutCells` integrates the missing quadratic B-spline support against its
  existing piecewise-linear solid SDF. A persistent 16-byte-per-cell record holds
  the integral, two input fingerprints and the most recent update flag.
- `CutSolidKernel` splits each integration ray at all Freudenthal tetrahedron
  transitions and solid crossings. Along that axis it uses the analytic kernel
  antiderivative; the other two axes use six-point Gaussian quadrature. Grid-axis
  planes are exact apart from floating-point arithmetic. Oblique/curved sampled
  boundaries are numerical quadrature, not exact curved-geometry integration.
- Exactly axis-planar corner data takes a separable analytic fast path, without
  a flatness tolerance. The cubic antiderivative uses explicit multiplication:
  inspecting DXIL showed that `pow(t,3)` otherwise emitted repeated Log/Exp
  instructions inside the quadrature loop.
- A moving collider still updates the existing geometric grid, but kernel
  integration runs only where the 27-cell support fingerprint changed. Fully
  open/closed cells contribute occupancy, not irrelevant far-field SDF changes.
  Two 32-bit fingerprints make collisions unlikely, not mathematically impossible.
  `--fluid-cut-kernel-full` disables this local cache for matched validation.
- The existing volume pass fingerprints each voxel once while its corner SDF
  samples are already loaded. Neighborhood classification reuses those small
  shared inputs rather than rehashing the same SDF corners up to 27 times.
- A separate lightweight classifier compacts changed cells using wave-level GPU
  allocation. `ExecuteIndirect` launches integration only for that list; a zero
  list dispatches no integration work. Existing capacity-reduction scratch holds
  the temporary dispatch arguments, with explicit UAV/indirect state transitions
  before its normal reduction lifetime. The list and voxel fingerprints add
  twelve bytes per cell plus a count; the list cannot exceed the fixed capacity.
- `DensityGatherCut` and its error-triggered repair read the cached support.
  No per-particle CPU work, new queue, extra frame fence, or density denoiser is
  introduced. Geometry-only observers do not allocate or compute the full kernel
  field; analytic geometry fixtures enable it for numerical tests.
- The weighted MAC pressure operator, precise shared flux, actual particle
  masses, canonical fluid DXR surface, photons and DLSS guide contract are unchanged.

At the room floor (`h=.16 m`, first open sample `.12 m` above the floor), the
missing kernel measure is `0.0703125`. Uniformly spreading the solid fraction
over its voxel gives `0.125`, a material overestimate. The new implementation
also integrates the physical domain exterior, including fractional last cells.
This is positional density support, not authoritative liquid volume transport.

## Validation contract

- Node tests cover the antiderivative, translated planes on every axis, clipped
  voxels, tetrahedral interpolation and solid/open complementarity.
- Existing bounded GPU snapshots check every cached mass for finite `[0,1]`
  values and independently check work-list range, uniqueness, exact coverage and
  reduction counts. Axis-plane fixtures compare every cell against the analytic answer
  with a `1e-5` absolute gate. Other cases rotate through up to eight spatial
  samples per frame and compare with an independently evaluated FP64 eight-point
  quadrature, with an absolute `0.003` gate. This is sampled higher-order
  agreement, not a rigorous global quadrature-error bound.
- `check-density-kernel.mjs` compares independent deterministic cache/full runs:
  particle diagnostics, actual surface volume, photons, and every valid captured
  radiance/caustic/DLSS guide channel must match exactly. These controlled runs use
  the existing fixed-point photon fallback, not floating-point atomic accumulation
  whose summation order is nondeterministic. Production profiles and ordinary
  pressure tests retain NVIDIA float atomics. Texture row padding is excluded
  because its contents are not image data.
- Existing pressure, rest-mass, density, procedural root, optical energy and
  temporal gates remain separate. Passing the kernel integral audit does not
  excuse a particle-density or temporal regression.

The broader reason to treat interpolation separately is documented in
[Azevedo, Batty and Oliveira's cut-cell work](https://cs.uwaterloo.ca/~c2batty/papers/Azevedo2016.pdf).
This patch does **not** implement their polyhedral conforming interpolant. The
engine's original bounded density correction also remains distinct from the
[full IDP-FLIP boundary treatment](https://animation.rwth-aachen.de/media/papers/66/2019-TVCG-ImplicitDensityProjection.pdf).

## Remaining scope

The ordinary room now passes its unchanged density comparison, but four repairs
can still be requested and broader particle boundary/displacement coupling needs
work. This is not a proof of nonlinear convergence or general volume conservation.
Sparse physical grids, flowing coarse liquid ownership, conservative fine/coarse
exchange, adaptive caustic reservoirs, multirate/async scheduling and budget
control remain tracked in `ADAPTIVE_REQUIREMENTS.md`. Ordinary play launchers
and the portable release are not promoted by this checkpoint.

## Reproduction

After the normal Windows build and `compile-shaders.ps1`, run these serially
with games closed (the scripts only terminate their own timed-out processes):

```powershell
.\engine\test-fluid-cut-cells.ps1 -CaseFilter '^(plane|oblique|moving-plane|sphere)$'
.\engine\test-cut-pressure.ps1 -NamePrefix density-kernel-cached
.\engine\test-cut-pressure.ps1 -NamePrefix density-kernel -Mode legacy -CaseFilter '^(calm|room|adaptive)$'
.\engine\test-density-kernel-parity.ps1
.\engine\profile-density-kernel.ps1 -Repeats 3
.\engine\test-water-temporal.ps1 -Name water-temporal-kernel-reference -MacMultigrid -Resample -SparseWork -Optical adaptive -Whitewater
.\engine\test-water-temporal.ps1 -Name water-temporal-kernel -CutPressure -Resample -SparseWork -Optical adaptive -Whitewater
```

```text
node --test engine/*.test.mjs
node engine/check-density-kernel.mjs RUNTIME
node engine/validate-water-temporal.mjs RUNTIME water-temporal-kernel water-temporal-kernel-reference --preserve
node engine/record-density-kernel.mjs RUNTIME
```

Validation and profiling are different workloads. The matrix/surface snapshots
are intentionally excluded from timing runs. Reports reject stale captures from
before the executable or any compiled shader. CPU tests, shader compilation and
other GPU workloads must not run concurrently with those profiles.

## Measured checkpoint — RTX 5090 / Windows Release

133 Node/reference tests and seven Windows CTests pass. All eleven cut-pressure
cases, four analytic/curved geometry fixtures, three matched legacy regressions,
eight cache/full parity runs and both 320-frame temporal sequences pass their
scoped checks. Normal room startup passes. GPU validation was retried but still
cannot initialize the Windows debug component (`0x887A002D`); no GBV-clean claim.

| Deterministic case | Legacy peak kernel density | New cut mode | Surface-volume difference |
| --- | ---: | ---: | ---: |
| Calm | 1.00002× | 1.00002× | 0% |
| Room inlet | 1.11377× | 1.07294× | 0.330% |
| Resampled/adaptive inlet | 1.11365× | 1.04635× | 0.00124% |

The preceding cut-pressure checkpoint's ordinary room peak was 1.25103×.
Particle rest mass matches in every comparison; those sampled density/volume
checks do not establish general physical conservation. The moving wake ends at
1.00309×. All MGPCG substeps meet the unchanged `1e-4 / second` target without
exhaustion; the relaxation case remains a bounded fallback, not a convergence
claim. Maximum sampled higher-order kernel disagreement is 0.000466.

Cache/full fixed-point runs have identical physics diagnostics and every value
in all ten captured channels, including raw radiance, guides, caustics and RR
output. In the room, the last moving endpoint rebuilds 414 of 123,975 kernels;
the wake rebuilds 2,443 of 105,750. CPU snapshots independently verify work-list
counts, coverage and uniqueness. Production float-atomic profiles are separate.

Three interleaved repeats, 300 frames each, 1080p Balanced, FG off, normal bins,
inlet + orbit + whitewater, no full-grid validation:

| Median of run measurements | Legacy MGPCG + geometry observer | New full kernel recomputation | New cached/indirect kernel |
| --- | ---: | ---: | ---: |
| Raw frame | 16.876 ms | 21.603 ms | 21.558 ms |
| Fluid simulation | 6.765 ms | 10.296 ms | 10.228 ms |
| Pressure mean | 3.743 ms | 3.866 ms | 3.860 ms |
| Geometry + kernel mean | 0.018 ms | 0.847 ms | 0.779 ms |

The local cache reduces boundary work by about 8%, but raw-frame improvement
over full recomputation is only 0.21%, too small to call a meaningful FPS gain.
The more accurate cut mode remains slower than legacy and below the initial
60-FPS raw target. Density repair and the remaining uniform classification work
need further optimization. The room's geometric/kernel/work default buffers
occupy 6,979,420 logical bytes, excluding heap alignment and validation snapshots.

The unchanged temporal-preservation gates pass: maximum brightness change
0.494%, raw motion-delta increase 0.543%, RR motion-delta increase 0.308%.
Camera-only frames retain fluid/caustic history; live water and rolling resume
actual simulation. These image measures include parallax and real deformation,
not just estimator noise. Actual generated-frame presentation is outside this
FG-off checkpoint.

Evidence: `density-kernel-validation.json`, `density-kernel-parity.json`,
`density-kernel-temporal.json`. The implementation map remains part of the full
adaptive goal; it does not replace that goal with a smaller completed task.
