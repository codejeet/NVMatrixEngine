# Contribution-weighted diffuse visibility

The default camera lighting estimator reduces visibility work at Lambertian
surfaces, including endpoints behind reflection/refraction and diffuse foam.
The spectral photon budget, caustic atlas, camera path depth and dielectric
Fresnel splitting retain their existing behavior.

`shaders/direct-light.hlsli` groups point lights and cube area lights into
separate two-source strata. Their importance is the largest unoccluded RGB
channel after material reflectance, including the cosine, distance, source
power, medium attenuation and area PDF. Selection happens before any shadow
test. A dominant blocked light cannot remove support for a weaker visible source.
The overhead water light, other collimated direct complements and flashlight
retain full visibility, preserving their moving shadow boundaries.

For each positive candidate with importance `w` and pair total `W`, its marginal
inclusion probability is `p = clamp(1.5*w/W, 0.5, 1)`. A lone positive candidate
is always tested. Two positive candidates cost 1.5 shadow rays in expectation
instead of two. Systematic sampling shares a uniform offset across the strata
and bounds total selected work to floor or ceil of the summed probabilities.
Each retained, visible candidate contributes `irradiance/p`. Strong lights can
be deterministic; no retained sample receives more than 2x amplification.

Reduction is enabled when the procedural liquid is active: measured triangle-only
visibility was too cheap to amortize stochastic selection. Vertices allocated
more than one optical sample also use full visibility to respect the quality
budget. Both decisions are made before lighting samples are observed. The
`lambertianReduction` report field records whether reduction is enabled by the
user; applicability still depends on the liquid and per-vertex sample budget.

This is conditional, compensated visibility sampling: the area-light samples
are held fixed while the visibility estimator is reduced. The probability is
not a radiance clamp or an early-stopping rule. The expected RGB result matches
the all-light estimator; added variance remains, particularly at shadow edges.
See the underlying [importance sampling and roulette principles in PBRT](https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency).

Candidate seeds and the selection seed use separate hash streams. Every NEE
invocation advances its parent stream by the same amount in both modes, so
selection cannot change subsequent diffuse directions. RTXDI's existing replay
seed reproduces the entire oracle, including its visibility selection. No new
history buffers or cached visibility are introduced. Photon-owned refractive
paths stay excluded from diffuse eye-path continuation.

## Water work

The procedural liquid intersection still clips the ray to the same cell and
isolates the same cubic roots, including equal-sign endpoints, thin sheets and
multiple crossings. For forced-opaque, first-hit rays that skip closest-hit
shading, opposite endpoint signs already prove a crossing within the finite
ray interval. Equal-sign endpoints still use derivative extrema to find an
interior bracket, preserving thin sheets. Boolean shadow queries report a point
inside the bracket without running 16 distance-refinement iterations. Ordinary camera and photon
rays retain the full root solve. No water normals, refraction directions,
extinction or surface geometry are approximated by this optimization.

Direct lighting also determines the endpoint's ambient extinction once and
reuses it across local lights. This avoids repeated liquid-field lookups at a
water endpoint. Reducing shadow queries saves the entire procedural traversal
for omitted samples, not just the root refinement.

## Reference modes and validation

- `--lambertian-reference` traces all positive light candidates with the same
  candidate samples and path RNG consumption.
- `--water-visibility-reference` restores full root refinement for shadow rays.
- Reports expose `lambertianReduction` and `waterVisibilityBracket`. Existing
  optical counters measure actual camera/reuse rays and pass timings.
- `NVMatrixEngineLambertianTest` compiles the actual scalar shader policies as
  C++, integrates the selection dimension across all visibility masks, and
  compares 100,008 water polynomials in full and visibility-only modes. It also
  runs on Linux with `g++ -std=c++20 -O2 engine/tests/lambertian.cpp -o /tmp/lambertian-test`.
- `test-lambertian.ps1 -BuildDir <build>` captures nine seeded raw frames for
  diffuse, water, underwater, fresh/spatially reused PT, and a static liquid
  fixture with actual temporal PT reuse. Run
  `node engine/check-lambertian.mjs <build>/bin/Release` to check mean RGB,
  immutable guides/atlases, and exact water visibility parity. These are
  empirical scene checks, not a general quality or performance guarantee.

Each candidate is evaluated once, with only two candidate states live across
visibility traversal. This avoids a large ray-state array inside the already
expensive dielectric prefix. Fewer rays alone do not prove a frame-time
improvement; the reference modes measure actual pass and frame timings. Fluid
simulation and reconstruction costs are separate from this optical optimization.

## Measured validation, 2026-09-17

[Recorded results and source/shader hashes](lambertian-validation.json): RTX 5090,
557x313 internal / 960x540 output, 256 frames per run, fixed-point photons,
ordinary DXR, frame generation off. Raw comparisons use nine frames. The reference
is the same binary with both reference switches enabled.

| Scene | Fewer camera rays | Largest mean RGB change | Raw relative RMS difference | Camera ms, reduced / reference |
| --- | ---: | ---: | ---: | ---: |
| Water room | 13.60% | 0.00084% | 0.708% | 3.821 / 4.038 |
| Underwater | 12.09% | 0.00299% | 0.811% | 3.325 / 3.472 |
| Fresh PT, water | 13.60% | 0.00159% | 0.797% | 3.981 / 4.221 |
| Spatial PT reuse, water | 13.60% | 0.00092% | 0.800% | 4.342 / 4.350 |
| Temporal PT, static liquid fixture | 16.97% | 0.02948% | 1.851% | 1.895 / 1.674 |

The dry control produces identical raw lighting and ray counts. Water visibility
alone produces bit-identical raw lighting, guides and both photon atlases in the
room/underwater captures. Diffuse selection preserves material/depth/normal/hit
distance guides and atlases exactly; motion differences remain below 1/8192 pixel.
The temporal fixture exercises 99 actual reuse frames. The separate 96-frame
moving-water, whitewater, boat and adaptive-sampling capture passes finiteness,
caustic bounds and photon-energy checks. All 241 Node tests, the shared-policy
GCC/MSVC tests and six transport shader compilations pass.

These timings are individual observations. Even the identical-work dry control
varied by about 5.5%, so a general frame-time gain is not established. The static
temporal PT fixture is slower despite fewer rays; use `--lambertian-reference`
when cheap traversal or reuse makes the selection overhead unhelpful. The
water/underwater quality and ray-count comparisons are the stronger evidence.
