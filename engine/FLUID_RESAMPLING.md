# Conservative adaptive particle samples

`--fluid-resample` enables real GPU particle merging/splitting driven by the
shared brick physics importance. It implies `--fluid-adaptive`. The ordinary
fluid launcher retains the uniform-sample baseline. This is **not yet Narrow
Band FLIP**: every occupied fine MAC cell retains particles; the passive bulk
inventory does not own liquid or replace pressure coupling.

## Integration and invariants

- `FluidParticle` remains 80 bytes. `apic0.w` carries rest mass relative to one
  initialized/emitted sample. Dyadic multiplicities permit exact integer bin
  accounting in units of 1/16. The current policy merges up to 8x and restores
  fine regions to 1x; it does not yet create sub-1x samples or particle-free cells.
- P2G, density projection, capillary occupancy, anisotropic covariance and
  scalar reconstruction all use mass, not the number of samples. G2P retains
  normalized interpolation. Collisions preserve the mass field when clearing
  affine velocity. Importance observes rest-mass changes rather than reacting to
  its own resampling; diagnostic counts still report actual samples.
- `FluidResampling` records after the final collision/bin pass and before
  surface reconstruction. It consumes previous-frame importance and current
  two-cell free-surface/solid guards. One pair per cell/frame provides gradual
  changes. Pausing stops resampling; reset initializes all particle masses.
- Equal-weight merges preserve center of mass and linear/angular APIC momentum.
  With quadratic moment `D=h²/4`, the affine correction is
  `sum(m outer(v-vbar,x-xbar))/(M D)`. A kinetic-plus-affine-energy gate rejects
  energy-injecting collapses. Separation and velocity-error gates limit changes
  to the local interpolation field. These invariants do **not** mean P2G at
  every individual face is identical after resampling.
- Symmetric splits use `Cnew = C (I-ddᵀ/(D+d·d))`, positions `x±d`, and velocities
  `v±Cnew*d`. This retains the combined moments without adding energy. Actual
  SDF tests protect both endpoints. Previous rendered positions are split or
  merged consistently so resampling does not fabricate a large motion vector.
- FLIP ignores affine state in P2G, so it cannot hide lost orbital momentum in
  that matrix: its merge gate additionally rejects non-negligible orbital torque;
  its splits retain equal velocities. Its validation excludes unused affine energy.
- Parent decisions are completed before recycled-slot writes, avoiding aliasing
  through stale bins. A GPU free list reuses only previously issued IDs, leaving
  future contiguous inlet ranges reserved. **Inlet capacity is still a limit on
  issued mass**, not recovered allocation capacity. A GPU source/free-list
  allocator and authoritative bulk ownership remain separate work.
- Secondary foam proposals are weighted by carrier mass, with the existing
  bounded per-slot probability. They remain a subgrid visual effect, not an
  exactly conserved multiphase mass/air-volume simulation.

The APIC moment convention follows the [authors' angular-momentum conserving
APIC paper](https://www.math.ucdavis.edu/~jteran/papers/JST17.pdf). Merge/split
algebra and engine integration here are original; the paper does not establish
the accuracy or performance of this engine's adaptive policy.

## Inspection and validation

- Expanded HUD distinguishes actual samples from issued rest-mass units and
  displays maintenance GPU time. `G` adds a particle-mass view when resampling
  is enabled: blue 1x, red 8x. `F6/F7` retain brick views/decision freeze.
- `--fluid-depth=<metres>` selects initial room depth; `--fluid-gravity=<m/s²>`
  allows a calm zero-gravity diagnostic without a hardcoded fake LOD pattern.
- `test-fluid-resampling.ps1` runs bounded empty, calm, falling/impact, rolling
  inlet/foam, freeze, reset, FLIP, multigrid, bulk, FG and PT cases.
- `--fluid-resample-validate --frames=N` takes opt-in before/after GPU snapshots
  **every rendered frame**. Independent double-precision host reductions check
  mass, linear/angular momentum and non-increasing energy for the resampling
  operation. This is expensive validation, not the normal runtime path.
- Normal execution reads only counters/timestamps after the existing renderer
  fence, checks exact global mass/sample accounting and introduces no new wait.
  Full fluid validation also checks mass in every GPU particle bin, collisions,
  pressure, field and procedural DXR roots.
- GPU-generated indirect arguments suppress all four maintenance stages when
  there are neither coarse requests nor weighted parents. The six bin passes
  execute only when merges/splits actually occurred, retaining the current valid
  bins otherwise. This has no CPU readback dependency or conditional fence wait.
- `resampling.test.mjs` covers random APIC pairs/splits, energy rejection,
  exact bin mass, the stale-bin alias hazard and mass-aware consumers.
- `profile-fluid-resampling.ps1 -Scene calm|room` interleaves on/off runs of the
  **same executable**, FG off, 1080p Balanced, 300 frames / 32 warmup. Both sides
  run importance. No invariant readback or concurrent profiling is allowed.

## Remaining scope

MAC and scalar spacing are uniform; pressure/density iteration budgets are
unchanged. Per-particle dispatches still span capacity (dead IDs exit early),
although neighborhood gathers visit fewer actual samples. The coarse/fine
ownership handoff, locally refined MAC grids, variable-resolution canonical
surface, adaptive rays/caustic reservoirs and budget feedback are still required
by the [full delivery audit](ADAPTIVE_REQUIREMENTS.md). Conserved mass is not a
proof of incompressibility or globally unchanged rendered fluid evolution.

## Measured checkpoint — RTX 5090 / Windows Release, 2026-09-12

The calm 100k-particle pit ends at **95,234 actual samples / 100,000 mass units**.
Its 12 coarse interior bricks drop from 6,624 to 1,858 samples (72% locally,
4.77% globally); all 93,376 fine-band samples remain. Only 13/120 frames rebuild
bins. The falling case merges 176 samples and restores all 176 under changing
importance; the FLIP variant merges/restores 107. The shallow rolling-room case
correctly performs no merges or bin rebuilds, while inlet mass reaches 105,899.

70 Node tests and six Windows CTests pass. The bounded GPU suite covers both
transport modes, full fluid/SDF/DXR validation, current bin mass, every-frame
resampling moments, reset/freeze, inlet/whitewater, pressure hierarchy, passive
bulk, FG and PT. Baseline constant/affine/compression/roundtrip/control tests and
the 2,400-substep settling test also pass. The 320-frame camera-motion sequence
passes preservation thresholds against `water-temporal-adaptive`: maximum
brightness shift 0.021%, maximum raw-motion-delta increase 0.122%, maximum RR
delta increase 1.135%, with one RR reset. This is a combined temporal/parallax
metric, not a pure Monte Carlo variance estimate or proof of all future LOD transitions.
The adaptive 1,200-substep settling case also passes volume/collision checks.
The requested debug-layer/GBV run stops at initialization with `0x887A002D`:
the Windows D3D12 debug component is unavailable. No debug-layer-clean claim is
made; no OS/driver settings were changed. All other GPU checks use normal Release.

Three interleaved on/off pairs per scene, 804 measured frames per mode, no
validation readback, no concurrent GPU work, FG off, 1080p Balanced:

| Scene / median GPU ms | Observer baseline | Resampling |
| --- | ---: | ---: |
| Calm pit: fluid simulation | 1.636 | 1.675 |
| Calm pit: surface reconstruction | 1.459 | 1.438 |
| Calm pit: full raw frame | 7.445 | 7.524 |
| Orbiting room/inlet: fluid simulation | 2.000 | 2.025 |
| Orbiting room/inlet: surface reconstruction | 1.166 | 1.150 |
| Orbiting room/inlet: full raw frame | 11.909 | 12.046 |

**This checkpoint is not a net FPS improvement.** Avoiding unnecessary bins
reduced the earlier maintenance overhead, but the current domains have little
safe coarse interior and still pay uniform MAC/pressure work. The measured
simulation increment is 0.026–0.039 ms; total frame differences also contain
camera/clock/run variation. Sparse work and bulk ownership are still necessary
for the requested larger performance gains. Reproduction evidence is in
`resampling-validation.json` and the two test/profile scripts.
