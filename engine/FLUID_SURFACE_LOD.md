# Adaptive canonical liquid surface

This document records the initial `c4465c2` checkpoint. The subsequent
[geometric interface band and directional sampling work](FLUID_SURFACE_BAND.md)
adds actual calm particle-water coarsening; the measurements below remain the
historical baseline, not measurements of that follow-up.

Integration map for spec sections 12–14, 21–23 and 28–31. This extends the
existing `FluidSurface`; it does not replace the solver or introduce a second
renderer.

| Existing subsystem | Adaptive connection |
| --- | --- |
| `FluidSurface`, compact page table and particle bins | Spatially persistent per-brick error/transition state; GPU partitions for 9³ fine and 5³ coarse reconstruction samples. |
| Covariance-shaped weighted-center reconstruction | Same kernel evaluator at both resolutions. Reconstructing 5³ anchors is actual reduced surface sampling, not a display-only requested LOD. |
| `FluidComplexity` | Consume physical disturbances, temporal change, visibility and optical priors independently of its physics LOD. Current moving-collider and occupancy guards prevent stale decisions. |
| Shared trilinear field | Constrain shared fine/coarse face nodes to the same interpolant. Gradual shared-node blend weights avoid cracks and abrupt geometry changes. The existing 9³ buffer is initially a prolongated DXR sampling cache, not compressed storage. |
| DXR intersection, normal, medium and photons | All continue to consume the same canonical field, masks and bounds. LOD displacement contributes to motion guides; geometry changes invalidate transport reuse without resetting RR every frame. |
| GPU profiling / validation | Separate fine/coarse sample counts and actual surface LOD; optional same-state full reconstruction checks, shared-node continuity checks, temporal and caustic comparisons. |

Two-level reconstruction first, followed by compressed backing allocation and
LOD-aware traversal. Coarsening must be based on measured interpolation/normal
error, not just hiding surfaces from the camera. A stable field may retain its
last error estimate between refreshes only while occupancy and disturbance
guards hold. Thin sheets, topology changes and moving boundaries stay fine.

Reference background: [Frisken et al., Adaptively Sampled Distance Fields
(2000)](https://graphics.stanford.edu/courses/cs468-03-fall/Papers/frisken00adaptively.pdf)
motivates detail/error-directed scalar sampling. [de Figueiredo et al.,
Revisiting Adaptively Sampled Distance Fields
(2001)](https://lhf.impa.br/ftp/papers/sib2001p.pdf) discusses limitations of
sample-based reconstruction. This engine's particle field is **not** a true
distance bound; its existing exact trilinear-cell root solver remains required.
The implementation below is original, engine-specific code rather than an
implementation copied from either paper.

## Runtime contract

`--fluid-surface-lod` enables the experimental consumer. The existing launchers
and default surface path are unchanged. `--fluid-surface-lod-fine` keeps the same
pipeline but forbids coarsening. `--fluid-surface-lod-validate` independently
reconstructs the fine field on the same GPU state and audits every updated frame;
it requires a bounded run and is not a performance mode.

Fine bricks evaluate 729 kernels; coarse bricks evaluate 125 anchors. GPU
compaction produces both indirect work lists. Every shared face/edge/corner node
uses the same transition constraint. Unsafe neighboring normal stencils veto
coarsening, with a separate repair pass if a fresh veto invalidates an already
scheduled coarse gather. Repair evaluations are counted rather than reported
as savings. Fine and coarse allocation is still a single, fixed-capacity 9³
sampling cache per active brick; this is **not compressed sparse storage**.

The normal tolerance is a unit-vector chord error, not a radiance error bound.
The scalar tolerance is measured in fine-cell widths; this particle field is
not an SDF, so that tolerance must not be presented as a guaranteed hit-distance
error. Sign-event and normal-stencil checks protect the tested thin surfaces,
but do not constitute a proof of arbitrary subcell topology preservation.

Per-brick input fingerprints cover the full 1.8-MAC-cell gather halo, weighted
particle positions, freshly computed anisotropic kernels, material displacement,
dormant owners and local collider transforms/shape. They are independent of bin
ordering. Changed inputs require a full error refresh before exposing an
approximation, even if the existing LOD remains coarse; unchanged inputs also
refresh periodically. Merely hashing bin counts was insufficient. Current
moving-solid and disturbance guards remain conservative.

The weighted input fingerprints are computed **once per MAC cell**, then reused
by overlapping brick stencils. Repeating the same particle/kernel hashing per
brick caused a measured scheduling regression and was removed. This shares the
existing GPU bins and a persistent scratch allocation; no CPU particle loop or
additional frame fence is introduced.

LOD deformation is flagged by the GPU only when the scalar field actually
changes. The camera motion-guide and caustic EMA consumers use this flag without
an additional CPU readback. Running classification or reconstruction because
the camera moved is **not** itself water motion. Otherwise camera orbit would
incorrectly shorten static caustic history and reintroduce flicker.

False-color view: `--fluid-surface-lod-view --fluid-view`, orange = fine,
blue = coarse, intermediate colors = transitions. This visualizes actual
sampling, not the complexity system's requested surface LOD.

## Validation scope

Production adaptive-surface acceptance remains open. The bounded fixture suite
covers flat/curved mixed-resolution surfaces, same-pipeline force-fine
references, camera orbit, thin slabs/spheres, empty/calm/falling water, wakes,
emission, mixed MAC, frame generation and ReSTIR PT. The current particle-based
gameplay pool conservatively remains fine; analytic-fixture savings must not be
advertised as a gameplay optimization.

Reproduce with `test-fluid-surface-lod.ps1`, `profile-fluid-surface-lod.ps1` and
`test-water-temporal.ps1 -SurfaceLod` paired against the ordinary temporal test.
`record-fluid-surface-lod.mjs` rejects stale captures and validates counts,
same-state error, debug colors and caustic parity. Temporal preservation is
checked separately with `validate-water-temporal.mjs ... --preserve`.

The D3D12 GPU-validation component was unavailable at the previous initialization
attempt (`0x887A002D`). These checks are numerical/runtime validation, not a
claim that GPU-based validation ran cleanly.

## Recorded checkpoint

The [runtime evidence](fluid-surface-lod-validation.json) identifies the executable
and shader hashes and includes 17 bounded GPU cases, 103 CPU tests and seven
Windows tests. The curved analytic box uses 103 coarse surface bricks out of
362, with 791,634 kernel evaluations versus 1,724,814 for a full fine gather
on the reported frame. This is a sampling-count result on an analytic fixture,
not a fluid-gameplay FPS claim. Maximum audited scalar error is 0.000500 fine
cell widths and normal chord error 0.000220; shared scalar/motion nodes agree
within the 2e-7 fp32 tolerance. Curved-box caustic-atlas relative L1 difference
from the force-fine reference is 0.261%, below the 1% comparison gate.

The [current temporal comparison](fluid-surface-lod-temporal.json) covers static,
rising/top-down orbit, live water and rolling. Maximum raw and RR regressions
are 0.90% and 0.991%, respectively, inside the unchanged 5% preservation gates;
maximum brightness change is 0.062%. Neither history resets nor radiance
filtering were introduced to obtain this result.

Three interleaved 1080p Balanced live-room/inlet/foam/orbit runs per mode, FG and
validation off, first 32 of 300 frames excluded:

| Mean of run medians | Default surface | Adaptive surface |
| --- | ---: | ---: |
| Surface reconstruction | 1.235 ms | 1.277 ms |
| Raw frame | 15.216 ms | 15.649 ms |

The current particle surfaces stay fine in these runs. The measured surface
overhead is about 3.4%; raw timings and tails vary substantially and do **not**
establish a speedup or stutter improvement. Default launchers remain unchanged.

The next required refinement is an error band derived from actual zero-crossing
cells and their normal stencils. This weighted-center scalar field is not an
SDF: deep interior values can remain near `-particleRadius`, so an `abs(phi)`
band can incorrectly treat harmless interior gradients as visible surface
error. That conservative estimator currently suppresses useful particle-field
coarsening. It must be replaced and validated, not bypassed by relaxing the
existing image/error gates. Sparse allocation, flowing bulk, adaptive optical
budgets and the other open items in `ADAPTIVE_REQUIREMENTS.md` remain required.
