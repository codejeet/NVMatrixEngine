# Engine validation

## Live room-water profile / visibility optimization — 2026-09-12

Three interleaved original/candidate pairs per view at 1080p Balanced, FG off,
with the live room inlet and foam/bubbles: raw renderer throughput improves
**5.5% in orbit and 4.6% while rolling**; the stationary median is effectively
unchanged. Camera GPU time falls 18.6% / 8.8% in the moving views; p95 renderer
intervals improve roughly 5–7%. The solver remains about 2 ms and reconstruction
1.0–1.2 ms. No physics, resolution, sampling or optical quality settings changed.

Only boolean visibility traversal changes: accept the first occluder and skip
closest-hit shading, including photon blocker and RTXDI PT visibility queries.
Water/whitewater procedural intersections and ordinary closest-hit transport
remain enabled. Other experiments were rejected after timing/image comparison.

**54 Node tests, 5 Windows CTests, six transport variants, 12 deterministic
image/backend comparisons, two 320-frame temporal sequences and five live-room
backend/FG/PT tests pass.** Raw image differences are below 6e-9 relative L1;
guides and camera event counts match. Temporal RR variation remains within 1%
of the previous corrected build, with no additional reconstruction resets.
Mainline/portable release are unchanged; Windows debug layer remains unavailable.

See [method, per-view timings and reproduction](ROOM_PERFORMANCE.md) and
[complete numerical evidence and shader hashes](room-performance-validation.json).

## Water flicker during orbit and rolling — 2026-09-12

This follow-up targets **temporal instability, not removal of the visible grid or
caustic pattern**. The previous spatial sampling change did not validate this
camera-motion case. A new 320-frame test settles the water, pauses it during an
orbit into a steep top-down view, resumes simulation, and rolls the avatar.

Two changes ship: analytic, coverage-preserving grid filtering using projected
camera-ray footprints (also through specular/refraction segments), and independent
water-history invalidation. Avatar motion retains the water atlas's existing
four-frame EMA rather than restarting the entire atlas every frame. Light,
prism/other-object motion and explicit resets still clear it; static caustics and
ReSTIR invalidation are unchanged. Paused liquid also uses the short history when
the avatar moves. No extra rays or full-screen blur, and no fake refracted guides.

Motion-compensated consecutive **RR output** luminance variation decreased by
**42.1% in top-down orbit**, **35.2% in live-water orbit**, and **93.5% after rolling**
versus `cfb9980` shaders. Raw-input reductions are 47.6%, 28.9% and 77.2% respectively.
Mean luminance changes by less than 0.7%. Static/opaque atlas resets still reach
58 during rolling, while water resets remain at 10, with one RR reset throughout.
The same sequence with foam/bubbles enabled passes and gives similar metrics.
Camera cost is essentially unchanged in this short 960×540 Balanced sequence
(2.270 → 2.267 ms); this is not a general performance claim.

**52 Node tests, 5 Windows CTests, all six transport variants and other kernels
pass.** Five current fixed-atomic/SER/FG/ReSTIR GPU cases pass (540 frames), as do
moving-optic and explicit-reset pit cases (192 frames). The moving-optic case
still resets both atlases 127 times; explicit liquid reset independently clears
water history. Captured guides are finite and photon energy remains bounded.
Independent per-frame photon sampling is retained: a correlated packet-refresh
experiment worsened reconstructed temporal behavior and was rejected.

The metric includes refracted parallax mismatch in the primary motion guide; it
is not a claim of zero flicker or a ground-truth optical-flow error. Grid footprints
use a paraxial ray cone, not full curved-interface differentials. The short water
EMA can leave a brief moving-avatar caustic/shadow trail. Real water motion and
residual estimator noise remain visible. Mainline and portable ZIP are untouched;
the Windows debug layer remains unavailable.

Reproduce with `test-water-temporal.ps1` (optionally `-Whitewater`) and
`node engine/validate-water-temporal.mjs <runtime-directory> <prefix> [reference-prefix]`.
See [measurements and shader hashes](water-temporal-validation.json).

## Underwater checkerboard correction — 2026-09-12

The repeating dark patches were already in the raw fluid photon atlas, not the
white floor material or DLSS. A 4× photon-budget diagnostic changed the pattern's
frequency; isotropic surface reconstruction retained it. The room flood's eight
wavelengths share 2,048 spatial launches at the default budget. Its much wider
aperture exposed the digitally shifted Sobol lattice, while the old two-texel
footprint cap cut even a flat pool's photon coverage.

Water-flood aperture coordinates now use nested uniform scrambling. Wavelength
strata, correlated spectral bundles, Fresnel transport and source power are
unchanged. Dynamic photon footprints retain their differential size up to a
roughly four-texel sigma bound; focused small footprints and the static prism cap
are unchanged. Every splat remains discretely energy-normalized. No extra rays,
field smoothing, screen-space filter, temporal-history increase or solver change.

All six transport variants and the remaining shader kernels compile; **50 Node
tests and 5 Windows CTests pass**. Seven room GPU cases (1,020 frames) pass with
flowing/still water, fixed atomics, standard/NVAPI SER, FG and optional ReSTIR PT.
Raw guides are finite, energy accounting remains bounded, RR resets once, and
actual fluid probes find no bad/truncated roots. The analytic sphere, thin sheet
and settled legacy pit regression cases also pass (248 frames).

The same quiet 64×64 floor-atlas patch in 240-frame flowing-room captures has
XYZ-Y coefficient of variation **0.201 → 0.110 (45.3% lower)**; mean Y changes
only **−0.14%**. The independent flat-pool CPU regression reduces the strongest
coherent Fourier component by **66.7%**, conserves power within 2e-14, and retains
dyadic coverage. Before/after raw atlases and reconstructed screenshots were
visually inspected. Remaining random photon noise is not claimed eliminated.

A short uninstrumented RTX 5090 check measured the photon pass at 0.673 ms versus
about 0.580 ms in preceding same-scene runs: approximately **0.1 ms extra**, not
an FPS speedup claim. Total raw frame timing remained near 10 ms; instrumented
validation/FG/SER timings must not be substituted for the normal raw-frame cost.
The slower fixed-point fallback remains correct. This is a shader-only lab
correction; mainline and portable ZIP are untouched. The debug layer remains
unavailable on this Windows installation, as noted below.

Reproduce with `node --test engine/*.test.mjs`, `test-photon-sampling.ps1`, and
`node engine/validate-photon-sampling.mjs <runtime-directory> [old-capture-prefix]`.
See [GPU results and shader hashes](photon-sampling-validation.json).

## White floor, tighter liquid traversal and optional ReSTIR PT — 2026-09-12

Windows Release, all six transport libraries and fluid kernels compile. **48 Node
tests and 5 Windows CTests pass.** The floor is neutral white with a dark metre
grid; the room source covers its full footprint at 140 radiant W. The old pit
keeps 28 W. The room screenshot was inspected: the grid is visible through water.

The 12 cases in `test-restir-pt.ps1` cover fresh sampling, temporal-only,
spatial-only, combined reuse, moving geometry, active fluid, orbit, NVAPI/fixed
fallbacks and Frame Generation. NVIDIA PT resamples actual multi-bounce diffuse
paths from opaque primary surfaces; it is not a completed refractive/glossy-primary
PT integration. It remains optional because it adds work, not a raw-speed win.
Fresh/reference and reused/fresh mean opaque RGB agree within 3% in the static
fixture. This is an estimator brightness smoke test, **not** an equal-time
variance/convergence proof. Every captured guide is finite and the photon energy
ledger remains bounded. The GPU reports nonzero reused paths; geometry/liquid
motion disables stale temporal suffix reuse while preserving RR history.

`test-temporal.ps1 -RestirPt` also passes the 256-frame prism-motion/orbit/settle
sequence: one RR reset, 34 atlas resets, visible response to both motions, and
return error 0.000295 in compressed luminance. Settled RR/raw frame-delta ratio
is 0.00724 over 381,853 stable pixel pairs. This measures RR stabilization, not a
ReSTIR speed or independent variance claim.

Three interleaved same-binary full/tight traversal pairs, RTX 5090, 1280×720
Balanced, 100k initial particles, wall inlet flowing, whitewater on, FG **off**,
240 real frames/run, first 32 excluded. **Per-cell probes disabled for timings**:

| Stage | Full brick DDA | Tight surface DDA | Median reduction |
| --- | ---: | ---: | ---: |
| Photon rays | 0.594 ms | 0.580 ms | 2.4% |
| Camera rays | 4.358 ms | 4.130 ms | 5.2% |
| Raw frame interval | 10.196 ms | 9.939 ms | 2.5% |
| APIC simulation | 1.944 ms | 1.947 ms | Unchanged within run variation |

Both modes use the **new** white floor and broad light, so this isolates traversal,
not a scene-brightness change. The raw interval excludes the outer gameplay loop;
these short scene-specific measurements are not universal FPS promises. Separate
instrumented runs show **31.9% fewer cell visits**, no bad/truncated roots, and
integrated caustic XYZ agreement within 2%. Instrumented runs show a larger timing
gain and must not be used for the user-facing speedup claim. Twelve legacy pit
render cases (849 frames) also pass, including analytic sphere, subcell sheet,
empty liquid, moving colliders, fixed atomics, SER and reset/debug controls.

See [research and exact PT limitations](FLUID_RENDER_RESEARCH.md),
[reproducible results](restir-pt-validation.json), `test-fluid-traversal.ps1`
(also run with `-NoProbes`) and `validate-restir-pt.mjs`.
D3D12 debug-layer / GPU validation availability remains limited as documented
below; actual DXR probes and finite-guide tests do not replace it. No shader/GPU
validation coverage on Ampere/Turing is claimed. Mainline and portable release
remain unchanged.

## Room-wide water, wall inlet and whitewater — 2026-09-12

Windows Release and all six transport variants plus the new emitter/whitewater
kernels compile. **43 Node tests and 5 Windows CTests pass.** The 11 room cases
in `test-room-water.ps1` pass over **2,430 real rendered frames**, including actual
T/E/RmlUi button input, pause/single-step/reset, capacity shutoff, an initially full
inlet, fixed atomics, NVAPI SER, FG and a 900-frame continuous fill. All room raw
captures pass finite RR-guide/energy/history checks. Secondary DXR probes verify
sphere boundaries, normals and water/air IOR transitions; no bad roots or nonfinite
secondary state were reported. See [reproducible results](room-water-validation.json)
and [model/limitations](ROOM_WATER.md).

The long run reaches **117,697 carrier particles, 947 foam and 163 bubbles** after
15 seconds of simulated flow. All four room quadrants remain populated. Carrier
bin membership/counts, pressure residual identity, domain bounds, solid collision
clearance and volume checks pass. Foam/bubble optical paths were validated and
the inlet view visually inspected; these captures do not establish a spray-specific
visual acceptance result.

RTX 5090, 1280×720 Balanced, 120 Hz APIC, 120 pressure / 60 density iterations,
FG off, 32-frame warmup (bounded validation includes optical probes):

| Test | Median raw frame | Secondary update + BLAS |
| --- | ---: | ---: |
| Closed inlet, secondary off | 9.92 ms | 0 |
| Flow, secondary off | 11.38 ms | 0 |
| Flow, secondary on | 12.51 ms | 0.229 ms |
| Inlet close-up, secondary on | 12.54 ms | 0.230 ms |
| 900-frame fill, secondary on | 13.07 ms | 0.247 ms |

The final matched flow pair costs approximately **1.12 ms more overall**, including
secondary traversal/shading, not only its compute/BLAS work. An earlier pair was
0.39 ms apart; these short evolving-fluid tests do not establish a fixed overhead
or guaranteed interactive frame rate. Counts and convergence evolve during the
longer run, so it is not a matched quality/performance comparison with the short
runs. The room uses 16 cm MAC / 8 cm render nodes versus the legacy pit's 8 cm /
4 cm; this is an explicit spatial-resolution tradeoff, not a solver speedup claim.

Regression suites also pass: **16 solver cases / 2,556 frames**, **12 glass-pit
render cases / 849 frames**, **8 material cases / 736 frames** plus 3 invalid-input
rejections, and **5 gameplay cases / 1,200 frames**. All 17 pit/gameplay raw captures
pass finite-guide and optical-energy checks. The ordinary optical chamber,
mainline native sources/executable and portable release are unchanged.

D3D12 debug-layer/GBV remains unavailable on this Windows installation; these
are executable/compute readback and DXR probe checks, not a debug-layer certification.
The previously documented intermittent FG pause-transition warning is not claimed
resolved by this feature. No OS/driver changes were made.

## Raw fluid/render optimization — 2026-09-12

Compared the saved **`035bf21` executable and shaders** with this optimization
on the RTX 5090. Two serial runs per view/build, each 360 real frames / 720 fixed
simulation substeps, discard 32 warmup frames: **656 pooled timing samples per
view/build**. 1920×1080 output, DLSS RR Balanced, 100k APIC particles, normal HUD,
65,536 photons/frame, 120 pressure + 60 density iterations, 120 Hz simulation.
Frame Generation is Off and unloaded; presentation is unpaced. No resolution,
normal filtering, optical branch limits, simulation quality or photon budgets
were reduced. These are renderer-frame timings, including submit/present/fence
overhead but excluding the outer game update, not a display-FPS guarantee.

| View | Median raw frame before → after | Reciprocal median FPS | P95 frame before → after |
| --- | ---: | ---: | ---: |
| Water close-up | 18.238 → 14.281 ms | 54.8 → 70.0 (+27.7%) | 28.417 → 20.540 ms |
| Gameplay camera | 15.755 → 11.787 ms | 63.5 → 84.8 (+33.7%) | 20.954 → 15.314 ms |

The **water/glass camera pass** was the main cost: 9.640 → 7.299 ms close-up,
7.052 → 4.249 ms gameplay. The spectral photon pass was only 0.70–0.72 ms before,
0.61–0.65 ms after. An original-compute-shader control with the added timing
columns and FAST_TRACE BLAS preference isolates simulation: **3.05–3.08 → 2.05
ms**, approximately one-third faster. Reconstruction stays around 2.33 ms; the
optimized BLAS build is about 0.123 ms. RR stays around 0.73 ms. Independently
computed pass medians need not sum to the median frame.

Implemented conservative surface-cell masks/tighter procedural AABBs,
FAST_TRACE BLAS construction, cached pressure/density stencil coefficients and
live laser segment masks. [Implementation notes](FLUID_IMPLEMENTATION.md)
describe scratch lifetimes and the unchanged field/optics contracts.
[Machine-readable results](fluid-performance-validation.json) contain individual
run medians, pooled results, static optical comparisons and compiled shader
hashes. Reproduce timings with `profile-fluid.ps1`; the saved-build comparison
validator is `validate-fluid-performance.mjs`.

Validation:

- Windows Release, all six DXR variants and fluid/resolve shaders compiled;
  **39 Node tests, 5 Windows CTests**, all **16 solver / 2,556-frame** cases,
  **8 material / 736-frame** cases and 3 invalid-material rejections passed.
  The 100k-particle long test retained its volume/residual/finite-state gates
  over 2,400 substeps. The solver operators/iteration counts are unchanged;
  floating-point reassociation and GPU particle scatter order do not promise
  bit-identical long-running liquid trajectories.
- All **12 procedural-fluid / 849-frame** render cases and **5 gameplay /
  1,200-frame** cases passed, including thin sheets, mesh/moving colliders,
  isotropic control, fixed atomics, both SER backends and UI controls. All 24
  fluid/gameplay/FG raw captures had finite guides and valid energy/history.
- Static canonical sphere/sheet comparisons isolate rendering from simulation
  drift. Traversed cells fell **39.7% / 50.8%**. Probe hit counts stayed
  **794 / 658**, glass shell entry/exit hits stayed 10, no bad roots or truncated
  marches. Maximum raw-lighting relative L1 difference was **1.42e-7** and dynamic
  caustic-atlas difference **5.52e-8**. RR output differed by at most **0.187%**;
  it is not claimed bit-identical. The non-fluid control retained identical
  guides and path counts. The water close-up was visually inspected.
- FG Off / 2× / 3× / 4× / DRED / fluid passed with expected actual presentation
  counts. The deliberate pause-resize-controls case passed once but emitted
  `Couldn't lock the mutex on sync present` in **2 of 3 optimized runs**;
  those runs completed normally (180 rendered / 326 presented, status 0).
  Testing the saved, unchanged `035bf21` build reproduced the same warning in
  **1 of 2 runs**. This is an existing intermittent pause-transition issue,
  not a clean lifecycle qualification or a new optimization regression. The
  warning was **not waived** in the test. Its underlying SDK/presentation cause
  remains unresolved; no FG synchronization change is bundled here.

D3D12 debug-layer/GBV validation remains unavailable on this Windows setup;
process-local DRED and the numerical/readback checks are not substitutes.
Mainline native sources, executable and portable ZIP are unchanged.

## DLSS Frame Generation / glass fluid pit — 2026-09-12

Windows Release and shaders built on the RTX 5090 setup with pinned Streamline
2.12.0. NVIDIA support queries confirmed FG, Reflex and HAGS availability without
changing Windows settings. The driver reports up to five generated frames;
this checkpoint exposes and tests 2×/3×/4×, not dynamic MFG or 6×.

| Fixture (1280×720, RR Quality) | Real frames / RR evaluations | SDK actual presents | FG status |
| --- | ---: | ---: | ---: |
| Off, plugin unloaded | 60 | 60 | 0 |
| 2× | 180 | 358 | 0 |
| 3× | 180 | 536 | 0 |
| 4× | 180 | 714 | 0 |
| Pause/F8 off/on/two resizes, VSync lifecycle | 180 | 326 | 0 |
| 100k-particle procedural liquid + glass pit, 2× | 90 | 178 | 0 |
| Process-local DRED enabled, 2× | 60 | 118 | 0 |

Every bounded fixture matched the shared-token, simulation, render-submit and
present marker counts to real rendered frames. The first two frames are real-only.
The lifecycle fixture retained five deliberate RR resets; the others retained one.
All cases shut down cleanly. `test-frame-gen.ps1` rejects SDK errors and unexpected
warnings, allowing only two documented SDK startup notices and a pacing-timer
reset during deliberately slow capture/reconfiguration in the lifecycle test.

The real `Play Lab.cmd` launch also passed **411 interactive frames / 757 SDK
presents**, resize, minimize/restore, and graceful shutdown, with three RR resets.
Three module-relative/foreign-directory launch cases passed another 144 frames.
The frame-generation startup failure in an unpaced window was fixed by using the
capability-checked tearing creation/present flags; waitable pacing is retained.
Error cleanup now fences submitted work before UI uploads can be destroyed.
Process-local `--dred` diagnostics were added while diagnosing that failure.

CPU validation: **36 Node tests and 5 Windows CTests** pass, including device-depth
projection, premultiplied UI composition, glass-shell manifold/SDF checks, fluid
numerics, optics and gameplay. Real-frame captures of the gameplay/pause screens
and procedural liquid were inspected. Captured guides and caustics are finite;
2× versus 4× caustic-atlas relative L1 difference was **2.40e-8**, consistent with
float-atomic ordering noise, not a changed light-transport estimator.

The final binary also passed all **8 liquid-material cases (736 frames)** and
three unstable/invalid-input rejections, **12 procedural-fluid render cases
(849 frames)**, **16 particle/MAC/APIC/FLIP cases (2,556 frames)**, and **5 gameplay
cases (1,200 frames)**. These include 100k particles over 2,400 solver steps,
moving colliders, both SER backends, fixed-point atomics, ten glass-shell optical
probes, tuning/grab/throw/jump and actual optical portal completion. The material
and optical model limits are recorded in [FLUID_IMPLEMENTATION.md](FLUID_IMPLEMENTATION.md).

These are actual **presentation counts**, not a benchmark claiming a corresponding
simulation speedup or display-scanout rate. Generated display frames are not in
the raw capture format. External frame-pacing/interpolation-IQ qualification and
tests on other RTX generations remain pending. The optional D3D12 debug layer is
still unavailable on this Windows installation; DRED is not a substitute for GPU
validation. Mainline `native/` sources, executable and portable ZIP are unchanged.

## Lasers / spectral water caustics — 2026-09-11

The lab now includes two 1.5 W monochromatic lasers, a 28 W white tank illuminator, a watertight refractive water mesh and a fifth receiver chart. **L** cycles green/blue/red. [WATER_OPTICS.md](WATER_OPTICS.md) specifies the IAPWS/Pope–Fry data, radiometric/photometric conversion, bounded beam integration and accuracy limits. Water ripples are static; no animated fluid or buoyancy claim is made.

- Windows CTest **4/4**, optics/source tests **19/19**. Tests verify three published IAPWS index values, absorption conversion/interpolation, manifold water geometry, analytic ripple-normal derivatives, phase-function normalization, zero-scattering beam visibility, source-power normalization, rolling/orbit and interaction behavior.
- Eight water/laser controls passed **1,536 DLSS frames**: green, blue, red, flat water, no lasers, no water, fixed-point atomics and no haze. Every case had one RR reset, finite captured guides/output/atlas, and zero invalid/overflow photon counters. Standard and NVAPI SER also each passed a 48-frame water/laser capture. All six raygen variants compiled; auto remains conservative non-SER.
- In the laser-free control, ripples raised interior, spatially averaged irradiance's coefficient of variation from **0.02203 to 0.33477**, while submerged radiant flux changed only from **19.0574 to 19.0517 W**. This measures photon focusing on the receiver, not a texture or denoiser effect. With lasers active, submerged flux was **20.3014 W blue**, **20.2526 W green**, and **19.9496 W red**, demonstrating wavelength-dependent absorption. Disabled water contributed zero submerged flux.
- The default 71 W photon-source ledger balanced within **0.00018 W** in the final-frame report; the control matrix's maximum discrepancy was **0.01682 W**, within the fixed-point diagnostic quantization allowance. Float/fixed atlas relative L1 difference was **0.000053318** (0.00533%). Camera path caps remain nonzero and reported; zero photon/beam truncations in these static controls do not imply infinite-bounce convergence.
- Final 720p-output / 853×480-internal default measurements on RTX 5090: **0.164 ms photon + beam raygen**, **1.184 ms camera**, **0.502 ms RR**. Earlier development runs measured 0.155/0.865/0.504 ms respectively; do not interpret cross-run differences as an optimization claim. At 1080p-output / 1280×720-internal, scripted rolling/orbit camera passes measured **1.408/1.508 ms**, with RR around **0.86 ms**. These are bounded, unpaced per-pass medians—not a promised interactive FPS or a resumed mainline comparison. NVAPI float atomics remain the fast default; concentrated laser splats make the fixed-point CAS fallback substantially slower.
- All five gameplay cases (**1,200 frames**), rolling (**300**), orbit (**300**) and temporal stability (**256**) passed again. Precision controls, grabbing/throwing, jump, portal completion and real **L-key wavelength cycling** passed. Rolling/orbit each retained one RR reset; explicit-cut gameplay retained five. The fixture's settled RR/noisy luminance-delta ratio was **0.00415**. The interactive launcher rendered **962 frames**, survived resize/minimize/restore and closed normally; three module-relative launch cases also passed.

[water-validation.json](water-validation.json) records the final captures, counters, compiled shader hashes and timing context. The preview was visually inspected. D3D12 debug-layer validation remains unavailable; no driver/OS settings changed. The mainline native executable and existing portable ZIP were not rebuilt or replaced.

## Rolling follow-camera fix — 2026-09-10

The ball's existing 120 Hz physics interpolation was not stalling in the reproduced routes. Instead, the old camera's overextended point ray could release abruptly when clearing a wall edge. The lab now uses a 30 cm sphere sweep, immediate inward collision protection and a 100 ms / 8 m/s-capped outward recovery. Ball interpolation and the accepted orbit integrator are unchanged. Picking uses the last displayed camera; the once-per-frame recovered camera also feeds DLSS and previous-view matrices.

- Windows CTest: **3/3 passed**. Fifteen four-second routes cover five headings at 60/144/240 Hz. At 240 Hz the maximum camera-distance step fell from **0.661112 m to 0.0309877 m**; ball steps stayed below 0.024681 m and no rolling render-pose stalls were observed. The test also covers variable cadence, reversals, jumping, immediate safe retraction, pause/restart and precision-camera transitions. These numbers measure simulated camera motion, **not frame time or an FPS gain**.
- Optics/source checks: **16/16 passed**. The rebuilt Windows lab passed **300 rolling/reversal/jump frames**, **300 orbit frames**, all five gameplay cases (**1,200 frames**), and the **256-frame temporal regression**. Rolling and orbit each retained a single startup RR reset. The real-input controls/tuning/throw/portal case retained five explicit-cut resets. All seven playable captures had finite guides/output/atlas, valid motion and zero invalid/overflow photon counters.
- The three module-relative launch cases passed; the actual launcher rendered **872 interactive frames**, survived resize/minimize/restore, and shut down cleanly. The rolling capture was visually inspected; this does not replace a subjective high-refresh playtest.
- Compiled shaders are unchanged from `f09d517`. No light-transport, denoising or GPU-workload change is bundled here. The shared native camera API gained an opt-in state argument; default/mainline callers retain their original camera behavior. The mainline executable and portable ZIP were not rebuilt.

[rolling-validation.json](rolling-validation.json) records the route measurements and rendered checks. Abrupt inward collision avoidance remains intentional; this fixes a reproduced source of rolling stutter, not every possible frame-pacing issue. D3D12 debug-layer validation remains unavailable as documented below.

## Orbit input / CPU scheduling follow-up — 2026-09-10

The mouse-driven camera previously changed only on legacy pointer messages; the display wait could also wake with input still queued. Orbit now uses foreground raw deltas, integrates them at render cadence with a 6 ms exponential response, and processes queued input again immediately after the display wake. Absolute-device/registration-failure fallback, pointer-based tuning and menu behavior are retained. Pending orbit movement clears on pause, focus loss, tuning and explicit gameplay resets. The OS caption no longer repaints every 15 frames; live FPS remains in the HUD.

- Windows CTest: **2/2 passed**, including the actual production orbit integrator. Total angle is stable at 60/120/144/240/360 Hz, irregular frame partitions agree, settling does not drift, and reset/clamp reversal work. Synthetic 125 Hz packets on a 240 Hz render cadence have angular-step **variance ratio 0.123665** after integration. This is an input-cadence test, not a measured display-stutter or FPS improvement. The smoothing adds a short response delay; physical mouse/monitor feel still needs playtesting.
- Optics/source checks: **15/15 passed**. The 360-frame gameplay test now includes a real right-button/move/release message sequence and verifies the full requested 0.4-radian turn is applied once. All five gameplay cases (**1,200 DLSS frames**) still pass.
- Scripted playable orbit: **300 frames at 1920×1080**, one RR reset, positive dense camera motion, no photon errors. The **256-frame temporal stability regression** passes again. Input sampling does not reset RR or change the lighting estimator.
- The interactive launcher registered raw mouse input, rendered **946 frames**, survived resize/minimize/restore, and closed cleanly. Basename, relative and foreign-working-directory launch tests also passed.
- Two ray-work experiments (early-out shadow traversal and reusing the initial glass hit) were checked on the same scripted orbit. They did **not** show a reliable GPU benefit here and were removed. Final compiled shader hashes are **identical to the accepted stability build**, preserving its light transport and DLSS guides. No GPU speedup is claimed for this input/CPU update.

An exploratory cross-run image check found bit-identical motion/depth/normal/albedo/specular guides and noisy-color relative L1 difference of `2.78e-9`. Reconstructed output differed by **0.1814% relative L1**, exceeding that diagnostic's tentative 0.1% all-texture threshold; compressed RGB mean absolute difference was **0.000399**. The images were visually inspected and the dedicated motion/settling regression passed. Independent RR output is not claimed bit-identical; the source of that small run-to-run output difference was not isolated.

The mainline renderer and portable release remain untouched. [orbit-validation.json](orbit-validation.json) records these checks and the rejected shader experiments; their bounded timings are not a resumed mainline-versus-lab profile.

## Temporal stability / pacing follow-up — 2026-09-10

The interrupted mainline-versus-lab profile was not resumed. This follow-up fixes reconstruction inputs, history ownership and interactive scheduling; it does not claim a measured frame-time speedup.

[stability-validation.json](stability-validation.json) records compiled shader hashes, the completed runs and temporal image checks without a new performance comparison.

- Corrected projected-image jitter sign and first-reflection hit-distance guide. Removed floating-point hit-position RNG reseeding.
- Split atlas invalidation from global RR reset. Retained exact rendered-pose motion vectors and TLAS updates, with a 0.1 mm, last-reset-anchor tolerance for photon-history invalidation. The optically untextured ball no longer rotates its triangulation with Bullet's angular pose.
- Auto uses ordinary raygen `TraceRay` for this continuation-heavy glass workload. Standard/NVAPI SER remain explicit, capability-checked choices. All six raygen variants and resolve/presentation shaders compiled.
- Explicit standard SER + fixed atomics and NVAPI SER + float atomics each passed a 48-frame correctness smoke test, with finite captured guides/atlas and one RR reset.
- Interactive VSync now has a one-frame-latency waitable swapchain; input/resize/close wake the wait. Removed duplicate RmlUi layout updates and redundant style churn; timing samples are only retained in bounded runs.
- CPU optics/source checks: **14/14 passed**. Windows CTest passed the actual Bullet mechanics and new transport-displacement/slow-drift checks.
- All five playable GPU cases passed again (**1,200 DLSS evaluations**). Initial, solved, blocked and overhead cases each had **one global RR reset**; the 360-frame movement/throw/tuning/restart/exit test had **five**, at explicit discontinuities. The rolling interval asserts no global history resets. The solved receiver still reads **0.886006 W**, while the initial and opaque-blocked controls remain **0 W**. All captured guides were finite; the blocked atlas was entirely zero.
- The new **256-frame temporal regression** passed static → prism motion → settle → orbit → settle. Exactly **34 atlas resets and one RR reset**; atlas age returns to 32 after motion. Static motion guides exclude jitter, while prism/orbit motion remains nonzero. RR responds to both pose and view changes and returns near the original image after settling.
- Across three final consecutive-frame pairs, **381,853** stable, non-edge surface samples had mean compressed-luminance changes of **0.00879273** in noisy input and **0.0000347335** in reconstructed output (ratio **0.00395025**). This is an RR-versus-noisy-input stability regression in the fixture, **not a before/after speedup or a claim of artifact-free gameplay**. Native output is area-averaged to input resolution for this check.
- Launcher tests passed basename, relative and unrelated working-directory launches. The actual interactive launcher rendered **908 frames**, survived resize → minimize → restore → resize, and closed cleanly. Its report confirms paced presentation and reconstruction resets on resize.

The initial playable run has 10 atlas resets across 180 frames, down from the historical 125, and one RR reset; its final atlas age is 32. A moving gate or adjusted optic still invalidates caustics; RR retains its own history. Rapid illumination changes can still lag in RR, and primary-surface guides cannot fully describe multilayer transmission. The two-bounce GI baseline, capped optical transport, pending ReSTIR work and unavailable D3D12 debug layer remain limitations. Mainline and the portable release were not modified.

## Historical foundation validation — 2026-09-08

## Playable glass / mechanics follow-up

[play-validation.json](play-validation.json) records shader hashes, all 13 GPU regressions and the final launcher check. The actual launcher stayed open for 474 interactive rendered frames and shut down with exit code 0; basename, relative-path and unrelated-directory launches also passed again.

The camera no longer draws the black interface placeholder. It now traces a Fresnel-split specular prefix with transmission, internal reflection/TIR, absorption and receiver-atlas lookup; post-diffuse paths still terminate at glass. The camera uses a 550 nm reference index for broadband appearance, while the photon simulation remains spectral. A low-output patterned calibration cradle makes transmitted detail visible from the overhead camera. Branch/footprint caps remain deliberate approximations, not a claim of an exact infinite-bounce spectral camera.

- CPU lab mechanics: passed settling, movement, grounded jump/no air jump, locking, top-down transition, mouse-plane movement/rotation, travel limits, cancel/restore, glass/cube grabbing and throwing, charge, gate and exit.
- CPU optics/source tests: **12/12 passed**, including dielectric slab energy, curved normal derivatives and a nonsingular near-critical footprint cap.
- `test-play.ps1`: all five playable GPU runs passed, totaling **1,200 DLSS evaluations**. Initial/misaligned and opaque-blocked controls measured **0 W** and did not charge. The solved 240-frame fixture measured **0.886006 W** of raw 510–570 nm flux and opened the gate. The real window-message run completed movement, jump, tuning, throwing, pause/HUD and portal victory in 360 frames.
- All nine captured raw textures were finite in the five playable cases, including the entirely dark atlas behind the blocker. Moving history ages are bounded and not falsely required to be fully converged.
- The new glass code actually executed millions of transmissions, internal reflections/TIR and reflections. Truncated camera branches are counted. Solved prism + ball transport also exercises counted footprint caps and photon truncation; these are not hidden as zero-error transport.
- The eight original `--fixture` GPU configurations were rerun with the new shaders: all supported backends, float/fixed accumulation, moving prism, 1440p and 4K passed. Static photon event counters are unchanged. Fixed/float atlas relative L1 error was **0.0032721%**; other SER/resolution variants agreed within numerical tolerance. All raw guide/atlas checks passed.

The original foundation measurements below are historical, before the playable camera/material/physics additions. The new chamber is not a validated six-level migration, full spectral camera, general emissive-light caustic solution, ReSTIR PT implementation, volumetric beam renderer or a new portable release. D3D12 debug-layer availability and actual Ampere/Turing hardware coverage remain as stated below.

## Original foundation results

Windows 11 build 26200, NVIDIA RTX 5090, driver 616.64. Retail app-local Agility 1.619.5 reports Shader Model 6.9, DXR 1.2 and actual standard hardware SER. NVAPI hit-object SER and fp32 atomic support also report available. The queried transport pipeline stack is 96 bytes for the standard variant; this is not a measurement of register spills or total per-thread memory.

[validation-results.json](validation-results.json) contains eight final controlled runs, all 512 post-warmup timing samples, capability/fixture metadata and compiled shader hashes. Each run completed 96 frames, with 32 warmup frames, 65,536 photons/frame and DLSS RR Quality. No GPU runs overlapped. These are smoke/validation measurements of a tiny homogeneous fixture, not a representative-game benchmark.

| Selection | Photon ms | Camera ms | RR ms | Frame ms |
| --- | ---: | ---: | ---: | ---: |
| Standard SER + float, 960×540 | 0.087 | 0.107 | 0.335 | 0.821 |
| Standard HitObject, reorder off + float | 0.078 | 0.073 | 0.302 | 0.743 |
| NVAPI SER + float | 0.085 | 0.108 | 0.314 | 0.805 |
| Standard SER + fixed | 0.207 | 0.110 | 0.321 | 0.946 |
| Plain TraceRay + fixed | 0.220 | 0.056 | 0.301 | 0.860 |
| Standard SER + float, 2560×1440 | 0.088 | 0.430 | 1.385 | 2.408 |
| Standard SER + float, 3840×2160 | 0.091 | 0.869 | 3.231 | 4.648 |

SER did **not** win on this simple scene. At the time of these historical measurements it remained enabled in auto mode to exercise the requested architecture; the stability follow-up above changes that policy. No speedup over the existing game is claimed. Float atomics were faster than the safety-first saturating CAS fallback in these runs; representative workload profiling remains a later gate.

## Numerical checks

All values in nine raw textures (seven RR inputs, RR output, caustics atlas) were finite for every final selection, the moving-prism case, and 1440p/4K output. Atlas energy was nonnegative and below the conservative CIE-weighted flux bound. Static history had age 32 everywhere; moving history had age 1 everywhere and exactly 96 resets across 96 frames. The moving prism produced a nonzero motion field (maximum 0.1864 internal pixels/frame in this fixture).

Relative L1 differences over atlas XYZ, against standard SER/float:

| Comparison | Relative L1 |
| --- | ---: |
| Fixed-point atomics | 0.0000327197 (0.003272%) |
| Standard HitObject, no reorder | 0.00000006051 |
| NVAPI SER | 0.00000006590 |
| Plain TraceRay + fixed | 0.0000327210 |
| Same atlas at 1440p output | 0.00000005828 |
| Same atlas at 4K output | 0.00000005898 |

The scene/angle hashes match before comparisons are accepted. Photon counters are identical across static backends: 65,536 emitted, 50,635 entering, 50,583 exits, 50,396 splats and 45 truncated continuations in the final frame. No overflow, invalid splats or footprint caps occurred. Truncation is nonzero and explicitly reported, not hidden as exact infinite-bounce transport. XYZ-weighted atlas checks do not replace the complete radiant-power accounting still required in milestone 2.

The first broad A/B batch exposed uncontrolled desktop input: raw prism normals differed by about 0.06 radians between two supposedly matching fixtures. That batch was rejected. Bounded runs now ignore mouse/keyboard events, record the prism angle and transport hash, and verify the fixed angle. The final matrix was rerun after this harness correction.

## Tests and limits

- Launcher regression: reproduced the basename-only `argv[0]` startup failure, then switched asset discovery to `GetModuleFileNameW`. After rebuilding, `test-launch.ps1` passed basename, relative-path, and unrelated Unicode/space-containing working-directory launches (48 frames and 48 DLSS evaluations each). The actual `Play Lab.cmd` stayed open, rendered 451 interactive frames with matching DLSS evaluations, and closed gracefully with exit code 0. Interactive failures now show an error dialog.
- CPU spectral/differential/normalization/Sobol/source-invariant tests: **8/8 passed**.
- Existing browser/unit suite: **31/31 passed**.
- Existing game rebuilt through CMake and its native physics/optics CTest passed after extracting the camera value type from the game header. Its solved lens-level GPU/DLSS smoke test also passed (72 beam segments), and the JavaScript native-optics reference tests passed 9/9. The unchanged `native/build.ps1` wrapper's Node invocation could not resolve its UNC test path in this WSL launch; direct CMake rebuild/CTest was used instead. No build-wrapper change is bundled into the engine rewrite.
- The recorded in-engine preview was visually inspected: the prism produces narrow spectral bands on receiver walls. Its visible interface is deliberately a debug silhouette, not finished transparent glass.
- **D3D12 debug-layer validation did not run:** `D3D12GetDebugInterface` returned `0x887A002D` (SDK component unavailable). No Windows feature, driver or security/performance-counter setting was changed.
- No real Ampere/Turing hardware test, ReSTIR estimator validation, lens/reference caustic convergence study, FG/Reflex validation or gameplay migration is claimed. See the explicit [acceptance gates](MILESTONES.md).

The normal native renderer remains inline compute and the portable ZIP's SHA256 is unchanged:
`66c789f3d6d84a8e1d74b87c7b5c780730827df1f6b1865d30bbd21e628cf5ed`.
