# Room-wide water and whitewater

Run **Play Fluid Lab.cmd** (`NVMatrixFluidLab.exe --fluid` / `--fluid-room`). The initial
100k-particle pool spans the chamber floor, approximately 30 cm deep. It is not a
floor-to-ceiling submerged-room preset. The old raised tank and its collision walls
are removed in this mode; `--fluid-pit` retains that scene for comparisons.

The floor now has neutral white tiles and dark metre-grid divisions. A 140 W
room-wide overhead collimated source illuminates it through the actual water,
so the bottom remains legible without an emissive-floor cheat. The small pit
retains its original 28 W source. ReSTIR PT is an optional opaque-indirect quality
mode via **Play ReSTIR PT Lab.cmd**; it does not replace water caustics or the
stable specular camera paths. [Research and limitations](FLUID_RENDER_RESEARCH.md).

The flood's photon aperture is scrambled to avoid repeating underwater checker
shadows. Water photon footprints now cover the wider room source without the old
pit-sized clamp; per-photon power remains normalized and the photon count is
unchanged. This corrects structured sampling artifacts, not all residual noise.
[Validation](VALIDATION.md#underwater-checkerboard-correction--2026-09-12).

For motion stability, the visible grid now uses pixel-footprint filtering, including
through water/reflections. Rolling the avatar keeps the water lighting's short
history instead of flashing the whole pool back to one noisy frame. Optical
changes still invalidate history; the grid and animated caustics are not removed.
[Orbit/rolling validation](VALIDATION.md#water-flicker-during-orbit-and-rolling--2026-09-12).

## Controls

- Click **SPEW WALL WATER**, press **T**, or **E** within 2.2 m of the wall valve.
  A hollow nozzle on the left wall pours into the same APIC pool. E elsewhere
  retains grab/release. The RmlUi button consumes its own pointer events so it
  does not also throw an item or start orbiting the camera.
- **I** switches between the inlet close-up and the rolling-ball camera.
- **P** pauses liquid, **.** single-steps, **B** restores the starting pool and
  closes the valve. This reset is not a simulated drain. Escape pauses gameplay
  and liquid; generated frames never advance fluid time.
- **U** exposes fluid and secondary counts/timings. FPS remains top-right.

`--fluid-emitter` opens the inlet on launch. `--fluid-capacity=N` sets the bounded
carrier allocation (default 250,000, maximum 1,000,000, at least the initial count).
The valve closes at capacity and the UI explains why. No live water is recycled
or deleted to sustain the stream. These limits are particle budgets, not a claim
that arbitrary fills maintain 60 FPS.

## Carrier and inlet

Room simulation bounds are approximately 11.9 × 3.04 × 13.9 m; the MAC cell size
is 16 cm and the reconstruction node spacing is 8 cm. This explicit room-scale
resolution tradeoff keeps the existing 120 Hz APIC, 120 pressure iterations and
60 density-projection iterations practical. The smaller pit retains its original
8 cm MAC / 4 cm surface spacing. Fine sheets below the room field resolution are
not guaranteed to survive.

The initial layout fills complete XZ layers before a partial final layer. Each
particle has a fixed volume. Birth count integrates **disc area × speed × simulated
time / particle volume**, retaining the fractional remainder. Only scalar count
bookkeeping occurs on CPU; the GPU creates positions, velocities, APIC state and
previous positions. Births are stratified through the interval's swept disc.
The current nozzle radius is 20 cm and speed is approximately 4.52 m/s
(approximately 0.568 m³/s). This is an intentionally large game-room inlet.

Player, prism, luminous cubes, gate, pedestal and source pedestal feed the shared
solid-SDF path. When a nearest contact would enter another solid or leave the
domain, bounded union-SDF escape finds a feasible contact. This prevents the
prism/pedestal junction from repeatedly pushing a particle into the other object.
Rigid-body coupling remains one-way: objects displace water, but do not gain
buoyancy or fluid reaction forces yet.

## Secondary phase

`src/fluid/whitewater.*` and `shaders/fluid/whitewater.hlsl` own 8,192 persistent
64-byte secondary slots. No secondary-neighbor search, pressure solve, particle
allocation on CPU, or particle readback in normal play is required. Births depend
on carrier speed, free-surface proximity, APIC deformation, grid-relative velocity
and curvature. The aerated inlet adds a local entrainment source. This is an
original bounded approximation inspired by the diffuse-particle family described
in [Macklin's PBF demo notes](https://blog.mmacklin.com/2013/04/24/position-based-fluids/)
and Ihmsen et al., *Unified spray, foam and air bubbles for particle-based fluids*
(2012), DOI 10.1007/s00371-012-0697-9; it is not a full reproduction of that method.

- Foam follows staggered grid velocity and projects onto the canonical liquid
  surface using its normalized scalar gradient. Finite lifetimes dissipate it.
- Bubbles follow liquid drag plus bounded buoyant rise. Centre and support samples
  must be submerged; reaching the interface converts a bubble to foam.
- Detached spray follows gravity and expires on re-entry. It is a secondary
  visual extension, not mass-conserving transfer out of the carrier solver.

Surface foam is a persistent, advected layer with irregular multiscale grain,
not cellular borders, an opaque sphere or a soft disc per marker. Flattened kernels seed
entrainment; isolated markers cannot produce visible foam. A separate GPU
transport pass carries density and material coordinates with MAC flow, with
gradual production and decay. Upward-interface production keeps the falling jet
clear. Camera, photon and laser transport share the layer; camera ray footprints
filter unresolved grain toward its mean. See [Surface foam](SURFACE_FOAM.md) for the energy
partition, limitations and validation.

Only bubbles and spray enter the secondary GPU AABB BLAS. Analytic sphere
intersections give normals and exact entry/exit distances. Bubbles use a water →
air → water dielectric interface, no absorption inside the cavity, and the same
spectral water IOR as the carrier. Spray uses air → water → air. Previous secondary
positions supply their primary motion guides to RR/FG; foam uses the canonical
water surface guides and a coverage-weighted diffuse albedo. Inactive and foam
AABBs use the DXR NaN exclusion convention.

This is **subgrid whitewater**, not resolved two-phase CFD or thin-film bubble
interference. Small bubble overlap is not merged into a union; origin-inside-a-
secondary classification and multi-bubble nesting are not a general medium stack.
Foam is an effective scattering approximation, not resolved volumetric foam
microstructure. The 8–21 mm bubble/spray radii and limited lifetimes are scene-tuned.
No claim of exact aeration volume or physically resolved bubble-size distributions.

## Validation and profiling

`test-room-water.ps1` covers still/flow, secondary-off A/B, inlet close-up, real
T/E/RmlUi input, pause/single-step/reset, capacity cutoff, fixed atomics, NVAPI SER
and DLSS-FG. `--fluid-room-test --frames=180` runs the input sequence. Explicit
validation reads finite particle state and count/volume/bin/collision invariants.
DXR probes independently check bubble/spray sphere roots, normals, material and
water/air IOR transitions, plus foam coverage and canonical water hits at foam
markers. They are absent in normal play.

Reports preserve the first nine timing columns and append `whitewaterSimulation`
and `whitewaterBlas`; total frame timing includes secondary traversal/shading too.
Use `--no-whitewater` with the identical scene, timestep, camera, output resolution,
RR mode and FG setting to measure the full effect. `whitewater` includes phase
counts, last-frame births/deaths, finite-state validation, foam-field bytes and
timings (secondary simulation now includes foam reconstruction);
`whitewaterProbes` reports actual optical boundary checks. The mainline executable
and portable release remain untouched.
