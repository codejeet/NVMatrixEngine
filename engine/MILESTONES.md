# Rewrite milestones and acceptance gates

The goal is the requested raygen-only spectral-caustics + ReSTIR PT engine, followed by migration of NVMatrixEngine's six levels. The branch keeps the existing game playable throughout. Do not promote a stage because a feature flag exists or a single screenshot looks good.

## 1. DXR/SER and DLSS foundation — implemented

- Separate executable/build; app-local retail Agility; SM 6.9 standard HitObject, NVAPI fallback, ordinary raygen TraceRay fallback.
- Capability reporting and selectable A/B controls; no compute traversal.
- Explicit pass barriers/transitions and bounded lifetimes; raw captures and GPU timestamps.
- Real RR/SR evaluation at SDK-recommended internal resolution, noisy color and complete baseline guides.
- Acceptance: all supported selections run, unsupported selections fail, finite guides/output, successful repeated frames and shutdown. Debug-layer validation remains pending Windows Graphics Tools availability; actual Ampere/Turing testing remains pending hardware access.

## 2. Spectral caustics — initial vertical slice implemented; generalization pending

Implemented: one collimated spectral source, one triangular prism, CIE LUT, Cauchy/Fresnel/TIR/absorption, packed planar differentials, normalized texture-space splats, float/fixed atomics, atlas history/reset, and dynamic prism motion guides.

Before calling this stage complete:

- General guided per-light sampling with explicit emission PDFs and budgets; include unrefracted source lighting in the complementary camera estimator.
- Multiple prisms and analytic convex/concave lenses; differential propagation includes changing surface normals. Preserve the existing Sellmeier reference path for comparison with the proposed Cauchy approximation.
- Receiver UV chart allocation/gutters/world-area Jacobians for arbitrary meshes; moving receivers, blockers and topology changes invalidate all affected transport history.
- Account for sampled, deposited, absorbed, escaped and truncated radiant power. Current atlas values are XYZ-weighted power, not a substitute for complete radiant-energy accounting.
- Independent high-sample spectral reference and wavelength-resolved hit records: check refraction/dispersion, focusing, dark regions behind blockers, footprint convergence, energy, and float/fixed error across budgets/resolutions.
- Ensure a deliberate camera-visible dielectric appearance without moving caustic transport into ReSTIR or silently replacing dispersion with RGB refraction.
- Prove acceptable moving-caustic lag/banding, not merely that the dirty flag fired. The EMA is not guaranteed converged on the first frame after edits.

## 3. RTXDI direct illumination — not implemented

- Integrate the pinned SDK's light/surface/random/visibility bridge and reservoir storage.
- Internal-resolution current/previous G-buffer and ping-pong reservoirs; temporal reprojection, surface/material validation and spatial reuse using SDK math.
- Visibility rays remain raygen-only; no reuse across invalid transport epochs. No denoiser before RR.
- Acceptance: stationary mean agrees with independent NEE reference; light changes/disocclusions invalidate correctly; no NaN/zero-PDF division; raw variance and cost measured with reuse off/on. SDK integration is not complete until its actual sampling/reuse entry points execute.

## 4. ReSTIR PT / GRIS — not implemented

- Use pinned RTXDI PT path-tracer contexts/reservoirs and hybrid shift mapping (reconnection plus replay), temporal reuse and 1–2 spatial passes.
- Track reconnection probabilities, Jacobians, path validity and pairwise MIS; avoid ad-hoc weights that alter the estimator.
- Start with 2–3 indirect bounces and an explicit glass/caustic path partition. Define how diffuse re-reflection of deposited caustic lighting is sampled without duplicating the light-to-glass-to-receiver segment.
- Acceptance: comparisons against independent high-sample path tracing for diffuse, glossy, specular-near-vertex, disocclusion and dynamic-light fixtures. Measure bias, noise, temporal stability, memory and pass times. A plain GI reservoir is not ReSTIR PT.

## 5. NVIDIA presentation and latency — initial FG/Reflex integration

- Reflex frame tokens/markers and latency mode with correct simulation/render/present ordering.
- Implemented in the experimental `src/streamline.*` wrapper: shared RR/FG/PCL token,
  capability-gated 2×/3×/4× controls, separate HUD-less/UI buffers, input-completion
  fence, and swapchain recreation when disabling/enabling the plugin. See README
  and `test-frame-gen.ps1`; external generated-frame IQ/pacing qualification remains.
- Optional FG only after support query; compatible driver/OS/HAGS requirements are reported, not automatically changed. Required HUD-less/depth/motion resources remain alive through present; resize/minimize/fullscreen lifecycle tested with FG disabled before mutation.
- Exposure stress fixtures with high caustic peaks, correct pre-exposure and optional transparency inputs for beams.
- OMM remains skipped while scenes contain no alpha-tested geometry. Async photon/raster overlap is a measured optimization after resource/queue ownership is validated, not an assumption of free overlap.
- Acceptance: no stale UI in generated frames, clean resize/shutdown, measured rendered versus displayed FPS and latency, correct fallback without FG.

## 6. Game migration and performance qualification — playable integration slice

The lab now reuses the existing Bullet `Game` mechanics, the RmlUi DX12 backend and native audio in one neon test chamber: rolling/jumping, grab/throw, cradle docking, top-down mouse/scroll tuning, a raw spectral-flux receiver, charge, portal opening and completion. The six-level campaign, home/video UI and full settings migration remain pending; the original game remains the release build.

- Reuse NVMatrixEngine's game/physics/optical solver, RmlUi, sound, menu and settings around the new renderer. Preserve all six levels, prism/lens negative controls, fine tuning and campaign completion.
- Test animated high-divergence representative scenes at 1440p/4K Quality/Balanced. Compare same scene, sampling budget and image quality with standard SER, NVAPI SER and no reorder; measure complete-frame cost, not just one shader.
- Capture profiling counters where available; optimize continuation live state, SBT/material organization, reservoir bandwidth and scheduling based on evidence. Do not assume the small homogeneous lab fixture demonstrates a SER speedup.
- Only then consider changing the game default or producing a new portable release. The existing ZIP remains untouched until an explicitly requested release is validated.
