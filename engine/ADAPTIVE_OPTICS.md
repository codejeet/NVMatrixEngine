# Adaptive optical work

Integration map for adaptive-spec phases 7–10 (not a claim of full delivery):

- `CameraRaygen` writes immutable primary geometry and validates reprojected
  optical history before selecting a sample count. Decisions do not inspect
  the current samples' radiance or stop early based on their values.
- `OpticalImportance` owns GPU-resident ping-pong optical state, receiver
  importance, counters, validation readback and an independently timed compute
  pass. It reuses the renderer's root signature and `gpu::Buffer` abstractions.
- Existing spectral photons → `Accumulate` → receiver-space importance. This
  reuses actual refracted light paths, with no duplicate photon traversal.
- Raw post-composite radiance + immutable current geometry → temporal/variance,
  normal/depth/material-edge and caustic signals. This history is scheduling
  data only; it never replaces or prefilters DLSS-RR's noisy input.
- Baseline diffuse sample means and existing cube-light NEE accept variable
  sample counts. RTXDI PT's pinned `GenerateInitialSamples` performs its own
  candidate/reservoir normalization; replay lighting keeps its fixed sampler.
- The already-known primary dielectric intersection is reused by the specular
  prefix. A reference switch retains the former redundant trace for exact A/B.
- Water-surface pixels reduce optical error/receiver importance into a GPU
  brick feedback buffer. The next `FluidComplexity` classification consumes it;
  surface reconstruction consumes that classification on its following update.
  Physics importance is unchanged. Current brick disturbance metrics can also
  promote the camera's sample budget before image variance becomes visible.
  The actual DXR primitive ID supplies interface ownership, rather than a second
  floating-point spatial quantization at brick boundaries. The audit independently
  checks each root against that brick's closed bounds before reducing feedback.

Optical sampling remains opt-in: the scoped checks below pass, but the measured
water-room gain is small and this is a higher sample budget than normal play.
Guided/reused caustic paths, indirect optical work queues,
flowing coarse bulk and the timing-budget controller remain separate required
work; a screen-space diagnostic image alone does not implement them.

## Controls and limits

`--adaptive-rays` enables historical importance and variable fresh-sample counts.
`Play Adaptive Optics Lab.cmd` opens the real water room with this mode at Balanced
internal resolution. Existing launchers remain unchanged.
`--optical-samples=1..8` caps the 1/2/4/8 tiers (default four). This controls
area-light NEE samples, baseline diffuse paths and RTXDI initial PT candidates,
not multiple primary camera samples or adaptive specular branch depth.
`--optical-reference` uses the same maximum everywhere for matched-cap comparisons;
`--optical-importance` observes while keeping the original one-sample budget.

With optical importance enabled, F4 cycles shaded/importance/variance/temporal/
caustic/sample-budget/confidence views; F5 freezes screen-space sampling decisions.
It does not freeze water, photon transport, or the underlying history statistics.
`--optical-view=importance|variance|temporal|caustics|rays|confidence` selects a
startup view. FG suspends for these diagnostics. Debug-view transitions reset RR.

`--camera-retrace-primary` retains the former duplicate primary dielectric trace
as an A/B reference. All internal reflection/refraction branches, cutoffs,
Beer attenuation and photon ownership rules remain unchanged.

## Validation and measurements

- `test-optical-importance.ps1`: bounded GPU scenes, independent moment/histogram
  audit, exact independent pixel-to-world-brick max reduction, F4/F5 injection,
  resize/FG lifecycle, and all three traversal paths.
- `test-optical-parity.ps1` then `check-optical-parity.mjs`: same-state cached vs
  retraced dielectric primary and passive observer, including every raw capture
  channel. Fixed-point photons and ordered bins isolate unrelated scatter order.
- `test-optical-estimator.ps1` then `check-optical-estimator.mjs`: raw RGB means
  from nine production frames against uniform eight-sample reference, separately
  for diffuse baseline, fresh RTXDI PT, reused RTXDI PT and actual particle water.
  This is an empirical estimator check, not proof of general reused-path unbiasedness.
- `test-water-temporal.ps1 -Optical adaptive|uniform`: existing static, rising
  orbit, overhead orbit, live water and rolling temporal checks with FG off.
- `profile-optical.ps1`: interleaved 1080p Balanced live-room/inlet/foam profiles,
  validation and FG off. Compare adaptive with uniform four, and cached primary
  with retraced primary separately. Do not compare a four-sample quality mode to
  the old one-sample mode and call the difference an optimization regression/gain.

The optical report separates actual camera/reuse ray counts, selected sample
budgets, initial paths, rejection/confidence history, caustic pixels, and feedback.
Classifier timestamps exclude CPU audits; complete frame timings include them,
so audited runs are not performance results. History is GPU-resident; validation
copies are optional and use the existing completion fence, with no new CPU wait.
Live timestamp history is bounded to 4096 samples. Per-pixel history uses 160 bytes
at internal resolution; the receiver field uses approximately 5 MiB.

Counters use wave-wide reductions. Equal-brick waves also reduce feedback maxima
before atomic writes; mixed-brick waves retain exact per-pixel writes. Wave-vote
predicates use non-short-circuit integer masks so HLSL 2021 cannot split votes
between hit/miss branches. The CPU histogram and full feedback audits cover this
optimization, including partial waves at image boundaries.

## Recorded checkpoint

[Machine-readable evidence](optical-validation.json) records current executable/
shader hashes, 19 bounded optical configurations, nine exact-parity captures,
12 estimator sequences, temporal sequences, and 12 interleaved profile runs.
117 Node tests and seven Windows CTest cases pass. The independent audits remain
enabled in the numerical/control tests, not in the profiles.

Cached versus retraced primary and passive observer comparisons are bit-identical
for all raw lighting, guides, RR output and photon channels in static water,
prism and orbit scenes. Adaptive raw RGB means differ from uniform eight samples
by at most 0.555% across the tested baseline/fresh-PT/reused-PT/water estimators.
Relative to uniform four samples, camera-ray totals fall 34–35% in the diffuse
fixtures and 6.75% in the calm particle-water room. This is work reduction, not
an equivalent FPS claim or a general proof for reused transport.

The final FG-off temporal comparison retains all existing gates: worst raw
variation regression 0.50%, worst RR variation regression 1.33%, and largest
brightness difference 0.021%. Camera-only phases preserve water history; live
and rolling phases actually advance the simulation. These metrics also include
refracted parallax and genuine physical changes, not only estimator noise.

RTX 5090, 1920×1080 Balanced, live room/inlet/foam/orbit, 300 frames/run with
the first 32 excluded, three interleaved repeats per mode (median of medians):

| Mode | Camera pass | Complete raw frame |
| --- | ---: | ---: |
| Uniform four-sample cap | 10.8112 ms | 19.2238 ms |
| Adaptive, same cap | 10.4998 ms | 18.9474 ms |
| Original one-sample budget, cached primary | 4.9018 ms | 12.3864 ms |
| Original one-sample budget, retraced primary | 4.9042 ms | 12.3924 ms |

Adaptive saves 2.88% camera time and 1.44% complete raw-frame time against the
matched cap. The first per-pixel-atomic version showed no net gain; wave reductions
lowered its importance pass from 0.2701 to 0.1710 ms (36.7%). Primary-hit reuse is
exact but its standalone timing difference is too small to claim a reliable gain.
The four-sample quality mode is **not** faster than the old one-sample mode.

FG resources/options/status and RR rendering succeed, but actual generated
presents are **not validated in this session**: both adaptive and non-adaptive
runs report enabled/status zero with zero extra presents, and the strict FG
lifecycle test fails its presentation-count requirement. A user question about
desktop/display state is pending. Do not describe this as passing FG validation.
GPU-based validation was not available in the earlier environment check
(`0x887A002D`); these are runtime/independent numerical checks, not a GBV-clean claim.
