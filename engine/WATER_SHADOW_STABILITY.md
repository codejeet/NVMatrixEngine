# Water shadows: buoyant-body history regression

The water atlas had a special exemption for avatar movement, but not for the new
boat or floating cubes. Every small buoyancy-driven transform cleared the entire
texture-space caustic history. A moving ball flashlight also changed the light
hash every frame and caused the same reset. This exposed single-frame photon
noise as dark, changing patches on the floor, particularly in overhead views.

Captured evidence from `experience-density-repeat`: frame 104 had 94 fluid-history
resets and water-atlas age 1; frame 140 had 130 resets and age 1; frame 186 had 169
resets and age 1. These are actual GPU atlas ages, not just host counters.

The fix separates emitted-light changes from flashlight pose and treats all
ordinary rigid-body movement as animated transport. The water atlas keeps its
existing four-frame-bounded EMA during movement, including when paused water
previously accumulated a longer history. Source power/spectrum/cone/environment
changes, explicit water resets and camera cuts still invalidate old lighting.
Static prism caustics and PT suffix caches keep their existing invalidation.

Photons still retrace the current geometry every real frame. No ray budget,
surface geometry, refraction, absorption, floor albedo, light power, or exposure
was changed. This repairs temporal-estimator starvation rather than brightening
shadows or hiding them with a screen-space blur. With alpha 1/4, old illumination
decays below 3.2% after 12 real frames; some short lag remains during fast motion.

The DLSS guide/input contract is unchanged: RR still receives raw screen-space
radiance and matching primary guides. Its reflection hit distance must not be
replaced by transmitted-floor distance: see the [NVIDIA RR integration guide,
sections 4.1.6–4.1.9](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md).
Multilayer refraction and surface motion remain harder than opaque reprojection.

Regression validation uses the existing 320-frame temporal sequence with
`--boat --fisheye --adaptive-rays --fluid-depth=0.85 --no-whitewater` (and then
with whitewater enabled). `check-water-shadow-history.mjs` checks consecutive
real-frame captures for finite guides, conserved photon accounting, one initial
water-history reset, bounded retained atlas age, and one initial RR reset. It
reports fixed-floor irradiance differences separately from screen-space output;
live fluid trajectories can differ between runs and must not be sold as pure
noise measurements.

## RTX 5090 validation, 2026-09-14 UTC

The unchanged installed executable (`water-boat-before`) was compared with the
candidate (`water-boat-after`), sequentially on the same GPU: 320 real frames,
960×540 output, DLSS Balanced, 65,536 photons/frame, the above room options, FG
off. Both runs finished successfully. Captures contain the actual presentation,
raw radiance, RR output, guides, and both photon atlases.

| Top-down check | Before | Fixed |
| --- | ---: | ---: |
| Water-history resets across all 320 frames | 320 | 1 |
| GPU water-atlas age in the measured phases | 1 | 4 |
| Fixed-floor relative frame difference | 0.33584 | 0.06391 |
| Mean floor irradiance Y | 0.68027 | 0.67971 |
| Motion-compensated raw frame difference | 0.033917 | 0.008475 |
| Motion-compensated RR frame difference | 0.021774 | 0.001470 |
| Whole-sequence median GPU frame time | 11.110 ms | 11.067 ms |

That is approximately 81% less fixed-floor lighting variation and 93% less
motion-compensated RR variation in the measured top-down phase, with a 0.08%
floor-irradiance difference and no meaningful performance regression. The raw/RR
measurement uses the existing `measure()` in `validate-water-temporal.mjs`, over
frames 221–224 and near-horizontal water pixels (about 66k samples/pair). It
includes refracted-parallax error, so it is not a universal perceptual flicker
score. Live-water and post-rolling RR differences also fell, from 0.02001 to
0.00161 and from 0.02288 to 0.00187 respectively. Before/after top-down captures
were visually inspected; dynamic caustics and refraction remain visible.

`water-boat-foam-after` repeated all 320 frames with foam/bubbles/spray enabled:
one water reset, one RR reset, retained age 4, finite guide/atlas values, and valid
photon energy accounting in every captured phase. The 240-frame
`water-shadow-experience` run also passed, exercising the flashlight, lighting
presets, lens/view changes, sink/float, buoyancy/boat, underwater view, and water
quality rebuilds. These deliberate setting changes still reset obsolete lighting;
ordinary moving-light/body frames no longer reset it continuously.

All 216 Node checks and all eight native CTest cases passed. The separate candidate
compiled and passed its gameplay self-test before GPU validation. The normal
`NVMatrixFluidLab.exe` was then rebuilt/installed and passed a 48-frame default-depth
startup check with foam, the boat and fisheye: one water reset, one RR reset,
retained atlas history, finite guides and valid photon accounting.
No fluid solver or light power was altered to produce these results. The available Windows setup still lacks the
D3D12 debug layer; this is runtime/numeric validation, not GPU-based validation.
