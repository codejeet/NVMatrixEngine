# Camera, lighting controls, and watercraft

The interactive lab uses a full-frame equisolid-angle projection, `r = 2 f sin(theta/2)`,
with a default **120-degree diagonal** field of view and a 90–160-degree slider.
The path tracer and DLSS-RR render a matching wide rectilinear projection; the
final presentation resamples it to the fisheye sensor. Mouse picking applies the
same mapping. This models the projection, not a multi-element lens assembly.
Finite-resolution resampling can soften the centre, especially at very wide FOVs.
Frame Generation is paused in fisheye mode because its current input contract is
rectilinear; it remains available with the normal lens. Debug overlays use the
normal projection. Lens/view changes reset reconstruction history.

Esc exposes neon, single-overhead, white-studio, and blackout lighting presets.
The ball flashlight is an 8-radiant-watt, 24-degree-half-angle spot source with
inverse-square direct illumination, shadow rays, and its own share of the
existing spectral photon budget. Refracted flashlight transport belongs to the
photon pass; un-refracted light belongs to NEE. Presets also gate emissive geometry
and sky illumination. UI settings currently last for the process session.

Interactive room launches include a twin-pontoon boat. Rendering, Bullet, and
GPU SDF colliders share its hull dimensions. Eight hull probes read the animated
canonical fluid field and MAC velocity. Twelve more serve the avatar and cubes.
Only these 20 height/flow results cross the existing frame fence, not particles.
Hydrostatic samples follow basin-connected water rather than selecting the highest
detached splash. Displacement and dissipative quadratic drag drive the rigid bodies;
drag impulse bounds include angular effective mass at each application point.
the boat receives thrust and steering torque. Moving hull SDFs displace the fluid
and generate wakes. The boat weighs 220 kg. Esc > Ball buoyancy toggles the avatar
between Float (hollow 60 kg ball, about 46 kg/m³ effective density) and
Sink (solid glass at 2500 kg/m³, about 3293 kg at the same 0.68 m radius).
The public Water Lab now defaults to **Sink**; Float remains selectable. The actual Bullet mass and inertia change without resetting position, velocity,
or water. The selected mass also becomes the boat's passenger load; solid glass
can overload the boat. The selection survives chamber/water resets during the
session and leaves non-water gameplay weights unchanged. Ctrl uses a gameplay
dive thruster.

This is a real-time hydrostatic coupling approximation, not a monolithic,
momentum-conserving pressure/rigid-body solve. Column samples assume locally
single-valued water surfaces; overturning sheets, slamming, planing, and propeller
jet aeration are not explicitly resolved. Stale samples expire after 150 ms;
box displacement uses quadrature, while spherical displacement uses cap volume.

The Esc liquid-quality controls reserve 100k–1M particle capacity and expose MAC
spacing and simulation frequency. They do not reseed the existing pool while
dragging. Apply explicitly waits for the existing queue, rebuilds fluid/surface/
whitewater resources, reconnects optical feedback, and resets histories.

Validation: `NVMatrixEngineExperienceTest` exercises the actual C++ lens mapping,
hydrostatic draft, propulsion, steering, boarding/exit, and underwater camera.
It also checks physical sink/float behavior, unchanged pose/speed on a paused
density switch, reset persistence, heavy passenger loading, and unchanged legacy
gameplay mass. The render fixture clicks Sink and Float and verifies the resulting
Bullet density/mass on the following frame.
`--experience-test --frames=240 --fluid-depth=.85` drives the RmlUi controls,
rebuilds coarse then fine fluid grids, drives the boat using GPU water samples,
and captures the menu, boat, and underwater views. `--boat` enables the craft in
bounded room tests; ordinary bounded reference runs keep their original scene.

Density-toggle validation (2026-09-14 UTC): 215 Node checks and all eight native
CTest cases passed. The 240-frame RTX run passed on repeat. The first run passed
both density-menu assertions but failed the existing frame-140 underwater-camera
assertion: the lightweight ball had been advected above the fixture's original
0.85 m waterline. That full-scene motion check remains intermittent; no threshold
was weakened, and it is not evidence of deterministic fluid/camera stability.
