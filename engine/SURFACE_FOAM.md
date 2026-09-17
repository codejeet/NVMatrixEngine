# Persistent grainy surface foam

The experimental native lab now transports a persistent foam layer on the water.
The earlier soft per-marker coating removed opaque sphere geometry but still
looked like fuzzy clumps. Markers now feed entrainment only; they no longer map
one-to-one to visible patches. The APIC carrier and canonical DXR surface are unchanged.
The former cellular pattern has also been removed: the current appearance is
irregular multiscale froth, without recognizable circles, rings or film-border cells.

## GPU integration

`WhitewaterUpdate` advects/classifies the existing bounded 8,192 secondary slots.
Foam markers project onto the canonical liquid zero crossing and cache its normal.
`FoamClear` and `FoamSplat` reconstruct a shared entrainment source. `FoamTransport`
backtraces the MAC velocity and advects density and material coordinates through
two persistent GPU buffers. The DXR water material samples this layer through
`fluid/foam-field.hlsli`, without an independent render mesh.

- Flattened, flow-aligned kernels overlap in the **source**, not the visible
  layer. A marker contributes at most 0.50; production begins strictly above
  0.50, so one isolated marker cannot produce a fuzzy disc.
- Surface-rafter production favours upward interfaces. It does not paint the
  vertical sides of the falling inlet jet white. Underwater bubble and spray
  geometry, water absorption and the lighting spectrum remain unchanged.
- Source lifetimes still fade over 0.18 s at birth and 0.65 s at expiry. The layer
  itself persists, backtraces the MAC velocity, and decays with a 1.4 s time
  constant. Production/decay use an exact constant-source exponential update;
  density is capped at 4. Pausing copies history exactly, including coordinates.
- A narrow scalar-field band extends density around the moving interface. Only
  actual DXR water hits receive shading; there are no floating foam billboards.
- The material uses four independently seeded, rotated noise bands at 7.1 cm,
  2.3 cm, 8.3 mm and 3.1 mm scales. Quintic interpolation hides lattice seams;
  the bands do not share a tiled texture or doubled frequency. There is no
  nearest-point/cellular evaluation, ring outline or hard alpha threshold.
- Noise uses **advected material coordinates**, not frame or screen coordinates.
  A ray-cone footprint fades each unresolved band to its zero mean. The previous
  layer's average coverage is preserved; bounded modulation changes texture,
  not average opacity. Photon/laser rays use the resolved appearance.
- Integer 16.16 source accumulation is order independent. The persistent density
  and density-weighted material displacement use float4 ping-pong buffers, with
  separate read/write histories. Paused output is deterministic.
- A 64-byte secondary record carries surface normal and age. Foam is excluded
  from the sphere BLAS. Bubbles and airborne spray keep their dielectric geometry.
- The field is GPU resident, with UAV ordering before DXR reads. Water-quality
  rebuilds recreate it with the new surface dimensions. Disabled whitewater takes
  a guarded zero-coverage path and allocates no foam field.
- Default room field allocation is 52,540,740 bytes: one uint source plus two
  float4 histories. Secondary records remain 64 bytes each. These are **dense
  bounded fields**, not sparse storage; cost scales with surface-grid dimensions.

## Optical model

For coverage `c`, dielectric Fresnel `F` and effective foam reflectance `a`:

```text
wet-film reflection: F
diffuse foam:        (1 - F) c a
clear transmission: (1 - F) (1 - c)
absorbed/untracked:  (1 - F) c (1 - a)
```

Camera paths evaluate the diffuse term against scene lighting, preserving the
existing wet-surface reflection. Transmission retains the existing medium/IOR
and Beer–Lambert handling. The existing bounded Whitted-prefix throughput cutoff
(0.001) also applies to the diffuse coating lobe, avoiding many visibility rays
for negligible reflected tails. This is a bounded-work approximation, not an
unbiased arbitrarily deep integrator.

Photon and analytic laser transmission use the same `1 - c` factor. Foam must not
leave full-strength refractive caustics under an opaque patch. Photons removed by
foam enter the opaque/untracked-diffuse ledger; they are not emitted again as
diffuse photons. Analytic beam endpoints illuminate the coating without adding an
unattenuated continuation. Albedo guides carry the actual coverage-weighted
diffuse reflectance, rather than baking foam lighting into a material guide.

The zero-thickness coating approximates the ensemble scattering of small bubbles.
It does **not** geometrically resolve foam cells, films, interference, volumetric
multiple scattering or diffuse photon re-emission. The canonical water geometry
still determines reflection normals, wet-film specular guides and primary motion;
independent tangential foam-pattern motion does not have a separate guide layer.
Marker overlap/field spacing and grain scales are artistic subgrid controls, not
calibrated gas volume or a measured bubble-size distribution. Material-coordinate
advection is first-order semi-Lagrangian; large deformation and topology changes
can distort or mix the pattern. The XZ noise parameterization and upward
production favour floating pool foam, not general vertical waterfall foam.
Thin interfaces can share field support; this is not a resolved two-sided shell.

## Validation

`whitewater.test.mjs` checks the coating/sphere separation, binary layout,
camera/photon/laser connectivity, optical-depth continuity, accumulation bounds
and energy partition. `--fluid-validate` additionally checks finite secondary
state, unit foam normals, phase counts, finite layer data, canonical-fluid ray hits,
and bubble/spray optical boundaries. Capture validation checks finite RR guides
and photon energy accounting. Foam passes have PIX-friendly events; their GPU
time is included in `whitewaterSimulation`, with BLAS time reported separately.
`foamActiveNodes`, `foamMaxDensity` and GPU nonfinite counters audit the new field.
`foam-layer.test.mjs` checks isolated-source suppression, timestep consistency,
pause, material-coordinate advection, grain continuity/non-tiling, per-band
filtering, coverage bounds and preservation of average opacity.

### Historical cellular-layer validation (2026-09-13)

These captures used the now-replaced cellular appearance; the underlying layer
transport, resource layout and physics remain the same in the grainy version.

- Release build, all five compute entries, six transport variants, 226 Node tests
  and 8 native CTests passed. Installed executable and shaders use the new layout.
- `foam-layer-inlet`: 240-frame close-up; 21,628 populated layer nodes, density
  bounded by 4; 793 foam boundary and 232 bubble boundary probes, no invalids.
- `foam-layer-detail`: 600-frame, 1920×1080/DLSS Quality inlet capture; 48,460 layer
  nodes, 1,042 foam and 306 bubble boundary probes; fluid and foam validation passed.
  The stream remains clear, with the cellular layer on the pool surface.
- `foam-layer-temporal`: 320 frames with top-down orbit, live water, and rolling.
  All 20 capture audits passed. Paused layer population (8,764 nodes), maximum
  density (1.3817) and phase counts remained identical during camera movement.
  Water/RR resets remained at one each; top-down floor relative change was 0.0649.
  These checks do not constitute a perceptual guarantee of zero flicker.
- `foam-layer-experience-repeat`: 240-frame settings, underwater, boat and
  resolution-rebuild test passed with a 117,863,460-byte rebuilt layer. The first
  attempt (`foam-layer-experience`) tripped the **fluid** collision assertion:
  0.002523 m penetration at particle 40040, collider 3. The unchanged repeat passed
  at 1.34e-7 m maximum penetration. No solver code or collision tolerance was
  changed; this intermittent broader validation failure remains recorded.
- Fixed atomics/SER-off, NVAPI SER and whitewater-disabled room tests passed,
  including the new layer population/range assertions and finite-guide/energy audits.

The matching 180-frame orbit (`foam-layer-orbit`, 1280×720/DLSS Balanced, FG off)
measured **10.663 ms** median total GPU frame time, **0.144 ms** secondary compute
including layer transport, **3.482 ms** camera transport and **0.112 ms** secondary
BLAS. Compared with the previous soft coating's 10.518 ms total, this is about
+0.15 ms (+1.4%) in these single runs; do not generalize it to other scenes/settings.

### Historical soft-marker coating

Initial coating implementation validated on RTX 5090 / Windows on 2026-09-13
(the captures below used the original 0.75 soft-marker coating, not the current
persistent layer; preserved as historical comparison):

- All four whitewater compute entries and six transport variants compiled;
  native release build, 8 CTests and 221 Node tests passed.
- `foam-experience`: 240 frames of settings, lens/first-person/underwater, boat,
  buoyancy and water-resolution rebuilds. The rebuilt foam field was 13,095,940
  bytes, with 246 foam boundary probes and 208 bubble boundary probes, no errors.
- `foam-temporal`: 320 frames including paused water, top-down orbit, moving water
  and ball rolling. Water and RR histories each reset only once. Twenty consecutive
  capture audits passed finite-guide and photon-accounting checks; top-down floor
  irradiance relative frame difference was 0.0646, preserving the recent history
  fix. This is not a foam-specific perceptual flicker score.
- `room-water-inlet-view`, `room-water-fixed`, `room-water-nvapi`: all passed GPU
  state/root/material/coverage tests. The inlet close-up tested 803 foam and 234
  bubble boundary probes, with no invalid results. Secondary-off smoke passed.
- The capture auditor now accounts for environment selection and the ball's 8 W
  flashlight, rather than assuming the default source configuration for all tests.

Identical 180-frame orbit scenario, 1280×720 output, DLSS Balanced, frame generation
off (`foam-before` versus `foam-streaks`; medians after the existing warmup):

| GPU pass | Sphere foam | Coating foam |
| --- | ---: | ---: |
| Entire real frame | 10.171 ms | 10.518 ms |
| Camera transport | 3.116 ms | 3.610 ms |
| Secondary compute, including coating | 0.031 ms | 0.057 ms |
| Secondary BLAS | 0.176 ms | 0.112 ms |

Observed total cost is about +0.35 ms (+3.4%), not a speedup. Wider optical coverage
and diffuse lighting add camera work while fewer active sphere primitives reduce
BLAS cost. These are single-run measurements, not a general performance guarantee;
slightly different secondary trajectories and GPU scheduling also affect timings.
The full D3D12 debug/GPU-validation layer is unavailable on this installation;
these are native runtime and explicit GPU-probe checks, not a claim of debug-layer
validation. Mainline and portable-release packages were not changed.

## References

The secondary phase follows the conceptual family in Ihmsen et al.,
[Unified spray, foam and air bubbles for particle-based fluids](https://cg.informatik.uni-freiburg.de/publications/2012_CGI_sprayFoamBubbles.pdf).
Density-dependent whiteness is also motivated by Akinci et al.,
[Screen Space Foam Rendering](https://cg.informatik.uni-freiburg.de/publications/2013_WSCG_screenSpaceFoamRendering.pdf).
This implementation is original world-space/DXR code; it does **not** adopt that
paper's screen-space surface pipeline or claim to reproduce either full method.
