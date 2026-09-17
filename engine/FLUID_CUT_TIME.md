# Time-centered moving-cut pressure

`--fluid-cut-time-centered` opts into temporal geometry for the **authoritative
particle-fluid pressure projection**. It implies cut cells and mixed MAC MGPCG.
It can be combined with `--fluid-bulk-bounded` to inspect conservative transport
of the passive coarse inventory. Normal launchers remain unchanged.

## Integration map

- `FluidCutCells` retains the preceding sampled corner SDF and integrates each
  shared face's opening over the simulated geometry interval. Existing endpoint
  volumes, collision geometry, solid-density kernel and renderer inputs remain
  distinct from this pressure geometry.
- `FluidSystem::projectGrid` selects the temporal pressure bindings only for a
  real moving-geometry simulation substep. `FluidMac` uses the same bindings for
  classification, mixed-resolution rows, MGPCG and canonical FP64 shared flux.
- The pressure support volume is the positive mean of the old/new endpoint
  volumes. The swept source is still their signed **difference divided by dt**.
  A cell that closes during the interval therefore need not lose its pressure
  unknown and every outlet at the start of the solve.
- The existing bind restores endpoint geometry before G2P, particle contact and
  density repair. DXR reconstruction, photons, ReSTIR and DLSS-RR continue to
  consume the actual animated particle surface, not the temporal pressure field.
- `FluidBulk` consumes the resulting canonical integrated shared flux without a
  second approximate velocity restriction. Its capacity/source-admission and
  receiver bounds continue to use **end-of-step** open capacity.

This uses existing persistent DX12 buffers, compute compilation, UAV barriers,
PIX events, frame-fence readback and pressure snapshots. There is no per-particle
CPU processing, new queue, extra normal-play synchronization or mass relocation.
The cut-cell debug camera moved from root constants to an upload CBV to keep
the enlarged root signature within DX12's 64-DWORD limit.

## Temporal geometry and limitations

Interpolate the sampled endpoint SDF values in time, using the existing spatial
Freudenthal face triangles. Split the time interval at each corner's zero
crossing; integrate each interval with four-point Gauss quadrature. Fully open,
fully closed and unchanged faces have inexpensive exact shortcuts for this
model. The independently evaluated CPU audit uses FP64 eight-point quadrature.
Initialization sets identical endpoints. Cached geometry, zero-step placement
and paused frames never apply an old swept source or old integrated aperture.

The pressure-normalization volume is an endpoint mean, **not** an exactly
integrated temporal volume or a new liquid-capacity definition. Reported pressure
residuals in this mode use that temporal normalization. End-state particle
compression and optical checks remain separate acceptance measures.

This is an approximation to moving rigid geometry, not exact swept-solid
intersection. Fast motion, rotation, changing nearest solids or an occlusion
that occurs entirely between endpoints can defeat linear SDF interpolation.
Time refinement/CFL studies and robust small-cell stabilization remain needed.
The moving-boundary literature motivates retaining time-dependent apertures and
volumes; this original implementation is not the complete stabilization scheme
of [Bennett, Nikiforakis and Klein](https://arxiv.org/abs/1711.11361) or the
moving-embedded-boundary solver of [Natarajan et al.](https://arxiv.org/abs/2108.00126).

In particular, this change **does not complete closing-cell liquid transport**.
The existing explicit donor cap leaves at least 5% of resident water when a
single update would otherwise empty it. Previous closure excess, particle-only
pressure support, partially filled cells and phase-flux limiting can also break
pressure/phase consistency. No residual inventory is deleted or silently moved
to pending sources. Flowing grid/particle ownership remains experimental and
must not be promoted based on a geometry-only or passive-mass test.

## Validation

```powershell
.\test-cut-pressure.ps1 -TimeCentered -NamePrefix cut-time
.\test-bulk-projected.ps1 -Mode bounded -TimeCentered
.\test-water-temporal.ps1 -CutPressure -Name cut-time-reference
.\test-water-temporal.ps1 -TimeCentered -Name cut-time-temporal
.\profile-cut-time.ps1 -Repeats 3
```

```bash
node --test engine/*.test.mjs
node engine/validate-water-temporal.mjs RUNTIME cut-time-temporal cut-time-reference --preserve
```

Independent analytic tests cover exact closure time, stationary/zero ties,
time-reversal symmetry, translating plane area, randomized sign changes and the
remaining explicit-donor closure defect. GPU geometry audits compare old corner
history and time-integrated face area. The existing independent physical-face
matrix/flux assembly uses the actual selected pressure geometry.

`auditedClosingCellSamples`, `auditedClosingLiquidSamples` and
`auditedClosingConnectedSamples` count cells in **audited final-substep
snapshots**, not all GPU substeps or unique world cells. Their volume sum is
likewise an audit sample sum, not total displaced liquid. These diagnostics
distinguish retained pressure support from merely enabling a flag. Normal
profiling does not enable these CPU snapshots.

The final build passes 160 reference tests, seven native CTests, eleven pressure
scenarios, twelve bounded-bulk scenarios, four same-build endpoint comparisons,
and the cut-cell wire overlay test. In the reset/moving-object sequence, 28
audited disappearing liquid cells retain positive pressure connections. The
wake's closing cells contain no particle centers in the audited snapshots;
retaining temporal geometry does not by itself supply their missing liquid
pressure support.

The deterministic same-build comparisons preserve total particle mass. Peak
particle compression is no higher than the endpoint reference plus the existing
0.01 tolerance, and reconstructed volume differs by less than 0.05%. Passive
inventory results are mixed and remain explicitly reported:

| Resident excess after transport | Endpoint pressure | Temporal pressure |
| --- | ---: | ---: |
| Calm pool | 0 m³ | 0 m³ |
| Room/inlet | 0.00533324 m³ | 0.00594653 m³ |
| Moving wake | 0.0127408 m³ | 0.00468072 m³ |
| Adaptive room | 0.00593424 m³ | 0.00653923 m³ |

Wake inventory in completely closed cells falls from `0.0124087` to
`0.00468031 m³` (about 62%). The residual is not zero; room/inlet overfill gets
slightly worse. These are not complete bounded-liquid or ownership acceptance.

[Motion-sequence evidence](cut-time-temporal.json) passes the existing
preservation gates across static, rising/top-down orbit, live-water orbit and
rolling phases: less than 1% mean-brightness change and no more than 5% worsening
of raw/RR frame differences. Measured worst changes are 0.183% in brightness,
0.383% in raw differences and 1.465% in RR differences. Camera motion does not
reset RR or the stationary-water caustic history. This is temporal preservation,
not another claimed denoising improvement.

## Raw cost

[Same-build evidence and executable/shader hashes](cut-time-validation.json)
include three interleaved 300-frame room/inlet/orbit runs per mode at 1080p
Balanced, with normal bins/float atomics, foam enabled, FG off and no full
validation snapshots. Both sides enable capacity admission and the phase limiter.

| Median across runs | Endpoint pressure | Temporal pressure |
| --- | ---: | ---: |
| Raw GPU frame | 22.6742 ms | 22.6111 ms |
| Fluid GPU time | 11.0795 ms | 11.0736 ms |
| Cut geometry mean | 0.776851 ms | 0.778112 ms |

The frame-time difference is only about 0.28% and is smaller than the run-to-run
spread; do not interpret it as an established speedup. The extra persistent
corner/aperture storage is 2,139,648 logical GPU bytes in this room (excluding
validation snapshots and the debug-camera upload). The measurements establish
low incremental cost for this geometry prerequisite, not 60-FPS raw-render
acceptance or completion of adaptive work allocation.

No new GPU-based-validation claim is made: the preceding Windows Graphics Tools
initialization probe failed with `0x887A002D`. OS and driver settings are untouched.
This checkpoint is one prerequisite for the full adaptive-fluid specification,
not completion of Narrow Band FLIP or an asserted raw-frame speedup.
