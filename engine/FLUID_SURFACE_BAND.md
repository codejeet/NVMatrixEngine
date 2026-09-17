# Geometric liquid interface band

Follow-up to `c4465c2`, addressing adaptive-spec sections 12–14 and 28–31.
The previous `abs(phi)` estimator was overly conservative inside this non-SDF
particle field. Do not relax its numerical budgets to force coarsening.

Integration:

- Fine reconstruction → persistent, spatially indexed 8³ zero-crossing masks.
- Unchanged coarse-input fingerprints → retain the corresponding fine mask;
  changed inputs still trigger current fine reconstruction and mask refresh.
- Analysis → load the 27-brick mask neighborhood once into group-shared memory.
  Surface-cell corners plus one node of padding cover shading normals and the
  existing photon normal-differential stencil across brick boundaries.
- Same error thresholds, shared-node constraints, unsafe-face protection,
  fresh-veto repairs, sign checks, DXR field and actual-deformation flag remain.
- Independent validation → reconstruct a world-node band from the full fine
  reference, audit reference interface-cell counts and enforce the unchanged
  scalar/normal/seam gates within the geometrically relevant region.

The masks describe the *fine reference*, not the approximate field's own
surface, so a lost feature cannot remove itself from the validation domain.
Node sign checks still cover the entire field. This is rendering adaptivity,
not removal of simulation particles or omission of bulk pressure coupling.

Use `test-fluid-surface-lod.ps1 -Prefix surface-band` to keep the previous
checkpoint's captures separate. A deep particle-based pool is included alongside
the normal room, analytic fields and moving/optical cases; analytic-only savings
are not sufficient to establish the requested particle-surface adaptation.

Admission diagnostics now distinguish protected, sign-vetoed, scalar-rejected
and normal-rejected surface bricks. Initial particle-pool checks rejected every
surface brick on scalar error, even though no bricks were hard-protected.
`--fluid-surface-lod-axes=xz` tests a 5×9×5 nested lattice: retain the nonlinear
vertical interface profile and coarsen tangentially where error permits. Any
nonempty subset of `xyz` is accepted (405, 225 or 125 anchors). The mask is fixed
for the field lifetime so neighboring bricks share exactly the same anchors;
fine/coarse selection, guards and smooth transitions remain per-brick. This is
not yet independently oriented coarse lattices or compressed voxel storage.

## Recorded checks

[Runtime evidence](fluid-surface-band-validation.json) records binary/shader
hashes, 30 bounded GPU cases, 109 Node tests and seven Windows CTest cases.
The full Release executable and shader set compiled. Existing third-party
Bullet/RmlUi warnings remain; GPU-based D3D12 validation was unavailable at the
prior initialization attempt (`0x887A002D`), so this is not a GBV-clean claim.

Directional sampling on **real particle water**, not analytic geometry:

| 32-frame deterministic audit | Coarse surface bricks | Coarse gathers / active bricks | Fine-reference scalar error / h |
| --- | ---: | ---: | ---: |
| Calm pit, XZ | 8 / 172 | 24 / 448 | 0.0000734 |
| Calm room, XZ | 60 / 855 | 36 / 938 | 0.0000212 |
| Deep pool, XZ | 0 / 1097 | 0 / 1840 | 0 |

Each XZ coarse gather evaluates 225 kernels instead of 729. The room's reported
frame evaluates 665,658 kernels versus 683,802 full-fine kernels (2.65% fewer).
Coarse **surface state** and coarse **gather work** are different: changed input
fingerprints and periodic audits still require fine evaluation, even when the
approved rendering LOD remains coarse. These counts are a bounded-frame result,
not a sustained performance claim. Deep/high-error water correctly stays fine.

All scalar, normal, sign-event, cached-interface, shared-boundary and motion-guide
checks passed. Maximum scalar and normal errors across the suite were 0.000502 h
and 0.000221, inside the unchanged 0.004 h / 0.015 limits. Moving wake/emission
tests exercised refinement after earlier coarse states. Thin surfaces, empty
domains, one-axis 405-anchor lattices, mixed MAC, FG and ReSTIR PT also passed.

The real-room caustic atlas differs from its same-pipeline full-fine reference
by **0.000477% relative L1**. The curved analytic comparison remains 0.261%.
Both are below the unchanged 1% gate. The particle-water debug capture contains
124,885 fine-colored and 2,963 coarse-colored raw pixels and was visually
inspected, alongside the normal shaded scene.

[Temporal preservation](fluid-surface-band-temporal.json) compares static water,
rising/top-down camera orbit, live water and rolling against the normal renderer.
Maximum raw/RR regression is 0.303% / 1.118%, within the existing 5% gates;
brightness changes stay below 0.015%. Camera-only motion adds no RR or caustic
history resets. LOD-only motion is emitted only within the geometric optical
band: changing a harmless bulk scalar must not amplify a weak interior gradient
into a fictitious surface-motion guide.

## Performance and remaining work

Three interleaved same-build 1080p Balanced orbits per mode and scene, 300 frames,
first 32 excluded, FG and validation off. Mean of run medians:

| Scene / metric | Normal renderer | Directional adaptive surface |
| --- | ---: | ---: |
| Calm room: reconstruction | 0.996 ms | 1.058 ms |
| Calm room: raw frame | 10.431 ms | 10.582 ms |
| Live inlet/foam: reconstruction | 1.156 ms | 1.223 ms |
| Live inlet/foam: raw frame | 12.314 ms | 12.461 ms |

There is **no net speedup yet**. Classification/error checks cost more than the
current amount of saved reconstruction work; live high-error surfaces mostly
stay fine. Raw tails vary markedly in both modes, so these runs do not establish
a stutter improvement. This consumer remains opt-in; default launchers and the
released game are unchanged.

Reproduce profiling with `profile-fluid-surface-band.ps1`. Temporal checks use
`test-water-temporal.ps1 -SurfaceLod -SurfaceLodAxes xz` and a separate ordinary
run, followed by `validate-water-temporal.mjs ... --preserve`.
`record-fluid-surface-band.mjs` rejects stale evidence and checks actual work,
caustic parity, debug pixels and profiling configuration.

Next: reduce repeated classification/error work and quantify full-refresh causes
over complete sequences. Dynamic surface error prediction, compressed storage,
flowing coarse bulk and the remaining adaptive-optics/budget-controller phases
remain open in `ADAPTIVE_REQUIREMENTS.md`. Do not promote the consumer or claim
the full adaptive spec complete on the strength of these narrower checks.
