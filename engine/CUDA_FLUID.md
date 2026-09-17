# CUDA liquid backend and multiscale implementation contract

Revised 13 September 2026 after the multiscale research review. This narrows the
implementation order from `/tmp/multiscale-spec`; it does not certify a combined
research system or replace the existing acceptance requirements.

## Current status

The new opt-in `--fluid-narrow-band` mode adds **live particle removal and
restoration** to the composed solver, with actual flowing grid-owned water in
pressure, density and the canonical DXR surface. Use `Play Narrow Band Deep Pool
Lab.cmd`; see [NARROW_BAND.md](NARROW_BAND.md) for implementation and validation.
The all-particle default remains available. Earlier checkpoint statements below
about retirement being disabled describe those earlier modes, not this one.
This does not certify a performance improvement or complete multiscale research.

The optional CUDA build now runs the single-phase uniform-grid baseline inside
the playable renderer: APIC/FLIP transfers, pressure, material forces, collisions
and density repair. Select `--fluid-backend=cuda` explicitly; ordinary builds and
launchers retain DX12. The existing surface reconstruction, DXR, water medium,
caustics, foam, buoyancy and DLSS pipeline consume the same GPU views. The bounded
sparse pool and two-level pressure module now run through complete composed
CUDA substeps with guarded GPU publication. Select the experimental playable
mode explicitly with `--fluid-backend=cuda --fluid-cuda-pressure=mixed`. Bounded
graph replay reduces baseline launch overhead; this is not full realtime-adaptive
acceptance or completion of the contract below.

The isolated pressure module now offers coupled MGPCG with a true-divergence
gate, including a matrix-free fine fallback during sparse-pool deferral. Legacy
relaxation remains a reference mode. Composed APIC/FLIP, moving-collider and
failure-publication fixtures now pass, including a 101,376-active-particle case.
Rendered mixed-mode numerical and temporal sequences now pass. Initial raw-frame
measurements show no speedup in the shallow room; full latency qualification,
conservative particle ownership and multiscale reconstruction remain unfinished.
The normal launchers and faster uniform CUDA mode have not been replaced.

The pressure-gather optimization reduced the experimental room's median raw
frame from 22.32 to 17.55 ms. A subsequent device-controlled iteration experiment
confirms 17.60 → 16.54 ms in repeated final-build schedule comparisons, but its
p99 is worse in that repetition set. It still does not
meet the 60-Hz deadline gate or outperform the uniform baseline. Conditional-loop
sanitizer qualification remains open on this Windows/Blackwell setup;
see the final checkpoint for methodology, diagnostics and remaining work.

An additional explicit CUDA-in-Graphics scheduling option now runs through the
renderer. Repeated same-build tests show a small mixed-solver median gain
(16.54 → 16.32 ms), but worse p99 in that set. CIG does not solve the deadline-tail
problem and remains off by default. See the CIG checkpoint below; ordinary CUDA
and DX12 selections have not been replaced.

CPU submission tracing has now identified and fixed a major *renderer-side*
latency contaminant: RmlUi geometry upload allocation/retirement while the CUDA
prefix was already running. A bounded, fence-retired upload cache reduces pooled
uniform/mixed p99 from 40.22/61.84 ms to 18.89/23.60 ms in matched before/after
runs. This is not a faster pressure kernel or full 60-Hz/adaptive acceptance.
The remaining smaller GPU/queue and occasional enqueue stalls are visible in
the new trace rather than attributed indiscriminately to interop. See the CPU
submission checkpoint below.

Mixed pressure now uses advected velocity/occupancy error, wake retention,
spatial padding and physical-time hysteresis for its coarse/fine decisions.
The refinement checkpoint below records 90 passing CUDA cases and fresh rendered
tests. This is pressure-refinement infrastructure, not adaptive particle ownership
or variable-scale reconstruction. In the shallow-room timing case it adds work
without producing coarse cells; the faster uniform default remains unchanged.

CUDA now also consumes the existing FP64 particle ownership ledger when
`--fluid-owned-particles` is explicitly selected. It shares the DX12 birth/reset
producer, uses authoritative mass/momentum in P2G and preserves solver velocity
increments through guarded publication. The ownership checkpoint below records
101 CUDA cases and live engine/optical comparisons. This is still **all-particle
ownership**, not automatic retirement into a flowing bulk grid. Default unowned
CUDA retains its original 14 shared buffers and uniform submission path.

CUDA now has conservative FP64 **grid-owned** quantity transport and area
restriction from the canonical projected MAC faces. A real DX12 retire → CUDA
transport → DX12 restore fixture uses the existing exchange inventories, not a
duplicate bulk replica. The new checkpoint records 120 CUDA cases and six clean
transport/exchange sanitizer checks. This component is not yet called by the live
solver: joint momentum prediction, capacity-compatible interface advection and
grid-owned surface reconstruction must precede automatic particle retirement.

The transfer module now also accepts actual grid owners for **joint momentum
prediction and return**. Grid-only cells participate in pressure through the
combined mass cache; no synthetic particle counts are introduced. A bounded
four-step GPU-kernel sequence advances both representations and preserves total
volume, and the matrix now has 132 passing CUDA cases. This remains component
integration, not live automatic retirement: whole-Solver publication, joint
interface-capacity handling and the canonical grid-owned surface are still open.

The next checkpoint now connects joint prediction/return/transport to the actual
Solver transaction, including graph replay, externally replaced inventories,
grid-only pressure support and late-pressure rollback. The gameplay adapter still
passes particle-only views. The new coarse combined-capacity audit rejects
overfilled trials; it is not yet a capacity-compatible phase/interface flux solve.

Joint Solver transport now reconstructs the total liquid phase geometrically and
uses bounded conservative face fluxes instead of backward-Euler dilution. The
geometric checkpoint below records 153 CUDA cases, including thin-interface
precision and actual later-pressure rollback. It reduces the grid-only fixture's
pressure support from all 693 cells to 359 after eight steps, without deleting
small owners. Coupled particle/grid interface capacity and canonical grid-owned
rendering remain open; this is not a new playable adaptive mode or a speed claim.

Full grid-owned pools now pass the tighter capacity bound despite rounded MAC
pressure residuals. A conservative receiver-admission pass repairs the low-order
face-flux reference before geometric correction, with GPU-controlled bounded
iteration. The capacity checkpoint below records 157 CUDA cases and fresh live
regressions. It does **not** yet couple actual particle endpoint occupancy to the
geometric phase flux; particle retirement and grid-owned rendering remain disabled.

The joint Solver can now publish total-phase volumes and reconstructed planes
through the real `FluidCuda` DX12 adapter. These optional output-only GPU buffers
share the simulation transaction and fence; a rejected frame preserves the prior
geometry. The geometry-publication checkpoint records 162 CUDA cases, including
actual HLSL consumption. A subsequent continuous-field checkpoint below now
connects these descriptors to the existing `FluidSurface` nodal field and
procedural BLAS builder. This is opt-in subsystem integration, **not** a new
playable joint-rendering mode or automatic retirement; optical qualification and
particle-detail blending remain unfinished.

The following DXR checkpoint now traces the published phase surface through the
production procedural intersection and shared dielectric-interface code. Bounded
planar-pool geometry, visibility, refraction and attenuation tests pass; this is
not yet curved/moving caustic or rendered RR acceptance.

The subsequent moving-plane checkpoint fixes off-lattice interface displacement
and wall-induced normal distortion. Volume-matched reconstruction and exact
Cartesian-domain clipping now pass oblique/reflected plane sequences through
production DXR. This remains phase-surface qualification, not playable joint-grid
ownership or full variable-scale/curved-surface optical acceptance.

### Experimental runtime pressure controls

```
NVMatrixFluidLab.exe --fluid-room --fluid-backend=cuda --fluid-cuda-graphs=on --fluid-cuda-pressure=mixed --frame-gen=off
```

`--fluid-cuda-pressure=uniform|fine|mixed` separates the old uniform Jacobi
baseline from forced-fine and two-level convergence-qualified MGPCG. The last
two share the same pressure acceptance limits and guarded publication. They do
not yet vary particle radius/density, P2G/G2P spacing or reconstruction resolution.

The experimental budgets are `--fluid-cuda-pressure-bricks=512`,
`--fluid-cuda-pressure-changes=64` and `--fluid-cuda-cg-iterations=32`. They bound
pool capacity, changed pages per rendered submission and CG iterations, not all
topology/hierarchy construction work. Pool deferral uses the qualified fine
fallback; a failed solve rejects the frame instead of publishing an approximation.
Nondefault pressure budgets require `fine` or `mixed`. CUDA settings require the
CUDA backend; DX12-only adaptive ownership/tiled/pressure modes and partial
ballistic/transfer/material fixtures are rejected explicitly.

Selection passes through `Options` → `Renderer::createFluid` → `FluidSystemDesc`
→ CUDA `Config`. The existing surface/DXR/caustic/whitewater consumers retain
their canonical GPU views. Pool reset now clears CUDA refinement history, including
paused reset/rebuild. Rebuilding water quality preserves the selected backend and
pressure mode through the existing renderer options.

`test-cuda-engine.ps1 -CudaGraphs -CudaPressure mixed` forwards the mode to the
existing numerical, room and rendered-surface suites. Incomplete solver fixtures
are explicitly skipped, not reported as MGPCG coverage. `test-water-temporal.ps1`
also accepts `-CudaPressure`. `profile-cuda.ps1 -Comparison pressure` compares
uniform/fine/mixed CUDA graph paths sequentially. Uniform Jacobi remains a cost
reference, **not** an equal-error reference for the convergence-qualified solve.

### Final-face publication precision

The first rendered moving-collider run exposed a real precision gap: the
continuous pressure residual was 9.972e-5/s, but rounding the projected velocities
to FP32 produced 1.051e-4/s of actual divergence. The pressure stopping criterion
now reserves half the 1e-4/s physical budget for storage/prolongation error.
A separate GPU reduction checks actual final-face divergence before topology
and whole-frame publication. A failed check sets the same sticky failure latch
that prevents subsequent particle advancement and shared-field publication.
This tightens the solve; it does not relax the external acceptance limit.

The deferred `pressureDivergence` metric now measures final stored-face flux,
not just the pressure residual. Full engine validation independently recomputes
that flux in CPU double precision from its existing validation snapshot. It
reports `pressureAudit="published-fine-face-flux"`,
`maxPublishedFluxDivergence` and `maxPublishedFluxMismatch`. The uniform-stencil
`maxPressureResidualMismatch` is **null** for this mode, not an invented zero;
the independent mixed-operator/Galerkin oracle remains in the CUDA MAC fixtures.

The added quantization stress fixture converges to a 3.237e-7/s solver residual,
then detects and rejects 9.326e-4/s of rounded-face divergence before publishing
topology. This deliberately extreme velocity field tests the failure path, not
the physical operating range of the game.

### Explicit rigid-body discontinuities

Repeated room-control tests then isolated a second case at frame 70: the test's
cross-room `Game::place()` teleport became a swept fluid boundary impulse. Its
solver residual was 2.9e-5/s while rounded-face divergence reached 1.04e-4/s.
Simply allowing that error would hide an incorrect input trajectory.

`Game::place()` now marks per-body discontinuities (also covering boat exit).
The renderer maps these flags to the affected SDF collider slots, including
compound hull pieces. `FluidColliderTimeline` establishes new anchors only for
those slots; it preserves other colliders' real motion, including motion across
zero-substep frames. No distance/velocity heuristic reclassifies fast real motion
as a teleport. This correction applies to DX12 and CUDA equally. Explicit
teleports into the experimental resident-volume transport require a fluid reset;
they cannot silently delete displaced capacity.

The completion record is now 64 bytes, adding the pre-storage solver residual
to failure diagnostics alongside stored-face divergence and the completed frame
index. This remains one deferred record per submission, not iteration readback.

The unfinished spacetime fixture is isolated behind the default-off CMake option
`NVMATRIXENGINE_SPACETIME_EXPERIMENT`. Its shaders require the explicit
`compile-shaders.ps1 -SpacetimeExperiment` switch (or the dedicated
`compile-fluid-spacetime.ps1`). It is not linked into the game. Compiling that
fixture would not establish a complete ST-FLIP solver: pressure/interface,
collider, emitter and render-time synchronization acceptance remain outstanding.

### Optional graphics-shared CUDA context

`--fluid-cuda-context=cig` selects CUDA-in-Graphics (CIG); `primary` retains
the previous context path and remains the default. This requires a CUDA 13.1+
build and a driver/device reporting D3D12 CIG support. An unsupported explicit
request fails with a useful diagnostic, rather than silently changing backend.
It applies to uniform, fine and mixed pressure, with graph or direct submission.

The integration is confined to the existing `gpu::CudaInterop` boundary:

- Match the CUDA device to the DX12 adapter LUID, then create a private CUDA
  context tied to the renderer's native direct queue. No additional DX12 queue.
- Resolve Streamline's queue/device proxies through its documented native
  interface query; compare native COM identities. Ordinary submissions still
  use the original Streamline-aware queue.
- Activate that context around CUDA allocation, graph preparation/replay,
  telemetry and teardown. Scoped activation restores the caller's thread
  context, including exceptions and nested scopes.
- The current subsystem owns one CIG context. A test creating two simultaneous
  CIG contexts was rejected with `CUDA_ERROR_INVALID_VALUE` on driver 616.64,
  even with separate direct queues. Multi-CIG ownership is not qualified;
  future multiple-fluid instances should share the renderer-level context.
  The lifetime fixture instead verifies coexistence with an existing primary
  CUDA context, nested activation, rejected queues and restoration on teardown.
- Keep shared-resource transitions and both external-fence ownership transfers.
  CIG changes scheduling, not memory ownership or the numerical solver.
- Query the graphics shared-memory limit and disable silent shared-memory
  fallback. A kernel that cannot run in that context must fail explicitly.
  Reports include actual `contextMode` and `cigSharedMemoryBytes`.

This uses the [CUDA context API](https://docs.nvidia.com/cuda/archive/13.1.0/cuda-driver-api/group__CUDA__CTX.html)
and [Streamline's native-interface contract, section 5.3](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuide.md#53-how-to-check-if-sl-proxies-are-used),
not an additional NVIDIA framework. CUDA 13.0 builds retain the primary path;
the CIG branch is compile-time guarded because its strict shared-memory fallback
control requires the newer contract. Runtime support is checked independently.

Use `test-cuda-interop.ps1 -Context cig`,
`test-cuda-engine.ps1 -CudaGraphs -CudaPressure mixed -CudaContext cig`, and
`profile-cuda.ps1 -Context cig` for the explicit experiment. Engine and temporal
suites validate the reported context, so a primary-context run cannot pass as
CIG evidence. Separate timing checkpoints below retain both median and tail
latency; enabling the feature alone is not proof of a realtime deadline win.

## Verified research boundaries

- [The independent Blender repository](https://github.com/eric-rolph/st-flip-blender)
  exists and declares MIT licensing and optional CUDA. Its own documentation
  distinguishes internal validation from paper reproduction and describes dense
  storage within an active window. Existence is not a correctness, performance
  or RTX-50 validation result. It is not an engine dependency or design pillar.
- [ST-FLIP, Figure 25, page 18](https://ge.in.tum.de/download/ST-FLIP.pdf#page=18)
  does demonstrate a spatiotemporal extension of PF-FLIP in an adaptive dam
  discharge setup, reporting approximately twice the speed. Thus composition
  is not wholly unpublished. This does **not** validate our CUDA implementation,
  arbitrary variable-radius APIC transfers at resolution seams, bounded update
  latency, or interactive rigid-body coupling. The paper's workstation/offline
  throughput results do not predict this game's frame rate.
- [MSBG's 32768-cubed / 100-billion-active-voxel example](https://github.com/tum-pbs/MSBG)
  is CPU sparse-volume processing at 16-bit precision on a 32-core, 256-GB
  workstation. The distributed demonstration performs particle surface
  reconstruction and mean-curvature smoothing. It is not a realtime GPU liquid
  benchmark, and its memory scale is not our VRAM target.
- [Cirrus](https://github.com/wang-mengdi/Cirrus) demonstrates smoke/vortical
  flow maps. It reports 1.5–2x GPU optimization speedup separately from the
  one-to-two-orders resource-efficiency benefit of adaptivity. Borrow GPU block
  scheduling concepts, not assumptions that long-range flow maps survive liquid
  reseeding and topology changes. Do not multiply these numbers into a game FPS
  prediction.
- [LFM](https://yuchen-sun-cg.github.io/projects/lfm/) demonstrates matrix-free
  AMGPCG for its vortical-flow system. It is a pressure-optimization reference,
  not a certified solver for our free surfaces, solids and coarse/fine junctions.

## Integration map and sequence

1. **Interop before a solver port.** Reuse `gpu_resources.h`, the renderer's
   DX12 device/queue, persistent buffers and existing frame completion fence.
   Match CUDA/DX12 adapter LUIDs; import shared DEFAULT buffers and a timeline
   fence. Validate GPU writes through DX12 → CUDA → DX12 with no particle CPU
   round trip. Measure queue handoff separately. Allocation size, state ownership,
   teardown and partial-failure behavior are required tests, not just a happy path.
2. **Same-physics single-phase CUDA baseline.** Port `FluidSystem`'s existing
   binning, APIC/FLIP transfers, boundary conditions, pressure, density repair and
   collision order. Preserve particle mass, timestep, solid motion, emission and
   material parameters. Retain the current uniform MAC discretization initially.
   Use CUB scan/sort where useful; optimize launch scheduling only after parity.
   Unsupported experimental combinations must be rejected explicitly. CUDA stays
   opt-in; no silent partial-backend fallback and no requirement for ordinary
   DX12-only builds to install a CUDA toolkit.
3. **Bounded sparse storage and predictive refinement.** Introduce persistent
   GPU block pools, work lists and staged topology transactions. Separate particle
   density from grid and surface resolution. Keep a globally coupled bulk field;
   removing particles cannot remove liquid mass or pressure support.
4. **Two-level single-phase MAC and pressure.** Reuse the numerical contract in
   [FLUID_MAC_MULTIGRID.md](FLUID_MAC_MULTIGRID.md), including the actual mixed
   operator, shared face fluxes, compatible transfers and true-residual checks.
   Uniform-per-level storage is acceptable; independent per-level pressure solves
   without conservative interface coupling are not. Reuse existing manufactured
   solutions, closed-domain and moving-solid tests before adding levels.
5. **Adaptive rendering acceptance.** Feed `FluidSurface` and the existing
   procedural BLAS/intersection, water medium, photons, whitewater, buoyancy and
   DLSS pipeline through the existing GPU view. Validate the canonical field and
   optics at every spatial transition before reducing surface work.

Only after that baseline meets its numerical, optical and latency gates should
ST-FLIP, long timesteps or a pressure-coupled air phase be reconsidered. Secondary
spray/foam/bubbles remain a separate visual system, not a simulated air phase.
CUDA is a compute-backend choice, not a replacement for DXR or DirectXMath.

## Bounded latency and refinement policy

These remain implementation and acceptance requirements. The checkpoints record
partial implementation evidence; they do not waive unfinished items:

- Preallocate block/particle pools and scan/sort scratch. Bound changed bricks,
  splits/merges and hierarchy rebuild work per frame. Maintain a GPU backlog and
  high-water counters; do not allocate/free device memory in ordinary substeps.
- Stage changes off the live topology. Publish field, ownership, pressure and
  surface versions together only at a safe simulation boundary. Defer a complete
  change when its pool/work budget is exhausted; preserve valid coarse support
  and report overflow. Never leave a half-applied seam or silently discard mass.
- Advect refinement flags, sweep collider/emitter bounds and pad by predicted
  travel over the update horizon plus kernel/halo support. Carry detail into wakes.
  Coarse vorticity is a resolved-scale indicator, not an oracle for absent detail;
  retain a minimum surface band and periodic fine error probes.
- Define temporal error per brick as a dimensionless change from the advected
  previous state: normalized velocity-prediction error and liquid-fraction change,
  with collider/topology events forcing promotion. Expose the velocity scale and
  thresholds. Use immediate safety promotion, smoothed ordinary scores, separate
  promote/demote thresholds and a minimum residency time for demotion.
- Split/merge through the existing conservative ownership transaction model
  ([FLUID_FLOWING_OWNERSHIP.md](FLUID_FLOWING_OWNERSHIP.md)). Preserve total rest
  volume, mass, center of mass and linear momentum; audit APIC angular momentum
  and kinetic-energy changes. Do not hide a failed transfer with a surface filter.
- Keep timestep bounds tied to collider travel and viscosity/capillary stability.
  A latency cap is not permission to take unstable steps. Report simulation-time
  lag explicitly when a workload cannot keep pace; compare equal simulated time.
- Async overlap is deferred until simulation/render states can be safely double
  buffered within the VRAM budget. CUDA interop alone does not provide overlap.

## Surface and light-transport gate

Variable-radius reconstruction needs mass-normalized kernels, consistent physical
support, halos and a single boundary convention across LODs. Scalar signs and
gradients must agree on both sides of a seam. Use conservative field traversal
and root refinement; scalar magnitude alone is not a valid distance bound.

Keep camera, photon and laser intersections on the same canonical surface.
Compare hit distance, surface normals, reconstructed volume and optical energy
with the fine reference, including moving seams, sheets, droplets and nested
glass/water. Repeat the existing top-down orbit/rolling/inlet sequences on both
raw radiance and DLSS-RR output. A smoother final image cannot excuse geometric
cracks, biased caustics or changed mean brightness.

## Evidence required before promotion

Use same-build sequential A/B runs on the same GPU, scene, random seed, simulated
duration and camera/input trajectory, with FG off. Separate warm-up from steady
state, but also report cold start, emitter onset, collider impacts and topology
bursts. Record median, p95, p99 and maximum raw frame time, missed frame deadlines,
actual simulated seconds, peak VRAM and update backlog. Record fluid kernels,
handoff, reconstruction, BLAS, photons and camera path tracing separately.

The initial combined-frame target remains 60 raw FPS (16.67 ms), not generated
FPS. Treat p99 within that budget as a target to demonstrate on a specified scene,
not a present guarantee. Do not change resolution, timesteps, light budgets or
pressure tolerances in a backend-only comparison. Also run equal-error adaptive
comparisons; do not label a lower-quality solve a backend speedup.

Reuse independent mass/momentum, pressure/divergence, collision and temporal
capture tests. Report capped solves and unavailable GPU validation honestly.
The existing mixed-MAC checkpoint is slower than the normal uniform solver in
its recorded room comparison; it is useful correctness infrastructure, not an
already-proven performance baseline. Keep the faster normal solver as default
until measured end-to-end improvement earns a change.

## Scope-revision validation

Windows Release lab build passed; 8 Windows CTests and 227 Node tests passed.
The generated game project excludes `fluid_spacetime.cpp`, the experimental
CMake option defaults to OFF, and the shader script passes PowerShell syntax
validation. Existing Bullet/RmlUi header warnings remain. This checks normal
build isolation and regressions, **not** CUDA execution, GPU numerical parity,
new rendered captures or a realtime multiscale implementation.

## CUDA interop / transfer checkpoint — 14 September 2026

`gpu_cuda_interop.*` implements the shared-buffer/timeline boundary independently
of solver algorithms. CUDA matches the renderer's LUID, imports committed DEFAULT
allocations using allocation size (mapping logical buffer size), and closes NT
handles after import. A submitted DX12 prefix releases ownership; CUDA waits,
executes and signals; the reopened DX12 list reacquires resources. The in-flight
allocator is never reset. Callback failures poison the transaction and do not
queue a wait for a signal that was never enqueued. Teardown drains CUDA before
freeing kernel scratch. Device-loss recovery is not established by these tests.

`cuda/fluid_cuda_transfer.cu` provides GPU histogram/scan/scatter, optional stable
CUB radix sorting, quadratic staggered-face APIC P2G, and APIC or FLIP/PIC G2P with
the existing grid-velocity advection and domain clamp. Binning scratch is allocated
once, not per dispatch; frame constants are kernel arguments. CUDA reproduces
the 80-byte particle and 368-byte frame ABI. Inactive flags and APIC .w metadata
are retained. Unimplemented grid-ownership modes are rejected by the transfer API.

Build/run from Windows, with the lab closed:

```powershell
& ./engine/test-cuda.ps1
```

The default test build is `%LOCALAPPDATA%/NVMatrixEngineCUDA/build`, separate from the
playable lab. `-CudaToolkit` and `-Architectures` select the local toolkit and
GPU code generation; this RTX 5090 checkpoint uses CUDA 13.1 / SM 120.
`NVMATRIXENGINE_CUDA_FLUID=OFF` keeps CUDA out of ordinary builds. The test script uses
the toolkit's local Visual Studio integration without modifying the VS install.
It explicitly configures CMake and refreshes reference HLSL. `-NoBuild` skips C++
compilation but rejects stale executable timestamps. Source changes during tests
invalidate the evidence. `test-cuda-interop.ps1` runs only the smaller interop test.

On this RTX 5090, all eight interop cases passed: 98 successful handoffs, boundary
sizes through 1,048,577 words, tail guards, duplicate/invalid imports, repeated
partial-constructor cleanup with unchanged handle count, and callback failures
both before and after enqueueing a kernel. Tiny/no-op handoff measurements were
about 0.2 ms median, with observed spikes: this is a synchronization cost, **not**
a solver or frame-rate speedup. D3D12 GPU validation is unavailable on this
installation (HRESULT 0x887A002D); the test reports that rather than claiming a pass.

Eight transfer fixtures passed against independently dispatched production HLSL
binning/P2G/G2P: empty, APIC, FLIP delta, fractional mass, atomic scatter,
multi-block scans, non-binary cell spacing and 100,002 active particles. Bins have
identical membership (and order in deterministic mode); mass/side channels are
unchanged. Observed maximum particle-field differences were about 3.3e-6 in the
recorded run. These are isolated transfers, not complete incompressible substeps,
long-term conservation, collision, surface or caustic tests.

The machine-local `bin/Release/cuda-validation.json` records case results, binary,
shader and source hashes. At this earlier checkpoint, pressure, material forces,
density repair, collision and renderer integration were still outstanding; the
baseline checkpoint below supersedes those implementation-status statements.
Same-physics timing comparisons, bounded sparse adaptation, coupled multiscale
pressure and optical acceptance at variable-resolution seams remain required.

The normal CUDA-OFF Windows Release lab also rebuilt successfully after this
checkpoint; all 8 Windows CTests and 227 Node tests passed. Existing Bullet/RmlUi
header warnings remain. No normal launcher, simulation preset or renderer shader
was switched to CUDA, and no new full-game visual/performance result is claimed.

## Integrated CUDA baseline checkpoint — 14 September 2026

Build the separate CUDA-enabled executable with `engine/build-cuda.ps1`, then use
`engine/Play CUDA Fluid Lab.cmd`. Its default is the room scene with frame
generation off. The normal `NVMatrixEngine` installation remains CUDA-free.
The CUDA executable can run either `--fluid-backend=dx12` or `--fluid-backend=cuda`
for future same-binary comparisons. Unsupported mixed-pressure, bulk-ownership,
resampling, sparse-work and tiled-density combinations throw rather than silently
changing the solver.

`cuda/fluid_cuda_kernels.cu` owns persistent CUB scratch, pinned collider endpoint
metadata, a static mesh SDF and the complete substep order. There is one
DX12→CUDA→DX12 transaction for all substeps in a frame. Initialization, inlet
emission and previous-render position snapshots reuse the existing DX12 prefix.
No liquid particles or grid values are transferred through the CPU. All ordinary
kernel launches reuse allocations. Collider staging cannot be overwritten until
the preceding submission completes; the engine's existing frame fence provides
that boundary. CUDA failures poison the partially advanced state.

Faces and scratch ping-pong by swapping logical bindings, with the resulting
parity published back to `FluidSystem`; no full-grid copy is needed. Pressure uses
the same Jacobi discretization and iteration budget. Density repair reads its
decision on the GPU. Its final CUB rebin is currently unconditional even when
repair is skipped: correct membership, but avoidable launch/work overhead still
to optimize. CUDA graphs, async overlap and bounded sparse topology transactions
were not implemented at this baseline-only checkpoint; graph replay is added below.

The backend comparison exposed repeated slip damping caused by roundoff after
collision projection. Both HLSL and CUDA now ignore a penetration smaller than
`max(1e-7 m, cellSize * 1e-5)`. This is a numerical contact dead band, not reduced
physical wall friction. A repeated sphere-contact fixture requires bit-identical
particle state when there is no intervening motion.

Validation on RTX 5090 / CUDA 13.1:

- Nine core GPU fixtures compare pressure, viscosity, curvature, all five
  analytical collider types, mesh SDFs, moving-wall velocities and GPU-conditional
  density repair against production HLSL. Three exercise the assembled solver
  over three frames with two substeps each. Maximum observed particle-field
  difference was about 3.6e-5; rest-mass metadata and active counts are unchanged.
- All 16 existing solver scenarios pass through the CUDA-backed lab, including
  empty/odd capacities, analytic gravity and affine transfers, FLIP, controls and
  a 100k-particle run lasting 2,400 substeps.
- All 11 material scenarios pass, including diffusion subcycles, sphere curvature,
  stronger viscosity/capillarity and invalid-parameter rejection.
- All 11 room-water scenarios pass, including a 900-frame inlet run, bounded
  capacity, whitewater, fixed/NVAPI atomics and frame generation. Existing DXR
  intersection, photon accounting and whitewater validation gates are retained.
  The final flowing-room capture was visually inspected.
- All 12 procedural-surface/render scenarios pass, including analytic sphere and
  thin-sheet volumes, moving mesh colliders, isotropic/anisotropic reconstruction,
  SER variants and surface/reset controls. These are fixed-resolution baseline
  results, not variable-radius seam acceptance.
- Same-binary 320-frame CUDA/DX12 temporal sequences, with whitewater and FG off,
  pass `validate-water-temporal.mjs --preserve`. Across all five phases, maximum
  absolute mean-brightness change was 0.031%; maximum raw temporal-difference
  increase was 0.19%, and maximum RR increase was 0.52% (gate: 5%). These metrics
  include refracted reprojection error and physical motion, not just noise.
  `check-water-shadow-history.mjs` also passes: camera-only phases freeze liquid
  and foam, and rolling preserves the water atlas and RR history. Captures are
  `cuda-water-temporal-*` and `dx12-cuda-baseline-temporal-*` in the CUDA runtime.
- Mixed MAC, sparse work and separate multigrid-pressure requests are verified to
  reject explicitly with the CUDA baseline, without a partial-backend fallback.

Run `test-cuda.ps1` for the isolated interop/kernel matrix. Run
`test-cuda-engine.ps1 -Suite solver|material|surface|room|all` for the engine
acceptance matrix; `-CaseFilter` selects cases. Reports are machine-local under
the CUDA build's `bin/Release`, with binary/source/shader hashes. These are
correctness runs, not a controlled timing comparison. D3D12 GPU validation remains
unavailable on this installation (0x887A002D); numerical and optical probes do
not replace a future debug-layer/GPU-validation run.

The CUDA-OFF Windows Release build passes 8 CTests and 227 Node tests. Full
same-physics latency/VRAM measurements and the sparse/multiscale/seam acceptance
gates remain open. Do not promote CUDA to the default based on these tests.

## Bounded graph replay and latency checkpoint — 14 September 2026

`--fluid-cuda-graphs=on` captures the **same** substep body used by direct CUDA
launches. `--fluid-cuda-graphs=off` keeps the reference schedule. The dedicated CUDA
launcher now selects replay; ordinary launchers remain DX12. Four persistent
variants cover initial/reused bins and the two physical MAC-buffer parities.
Pressure's initial binding is canonicalized because classification clears both
banks; the final parity is published normally. A frame replays at most the existing
16-substep limit. No new equations, pressure iterations, density corrections,
particle timesteps or rendering budgets are introduced.

Graph preparation happens before submitting the DX12 prefix, never within the
ownership callback. Replacement is transactional and leaves the published host
bindings intact on failure. Camera/emitter metadata does not key the graph;
changing actual solver parameters replaces the bounded four variants after the
previous frame completes. Moving collider endpoints are copied into a persistent
device slot in stream order. External fences, uploads and solid baking remain
outside capture. Initialization/display-only P2G still uses direct launches.
This follows the [CUDA graph capture/instantiation contract](https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/cuda-graphs.html);
it is not device-wide synchronization, async simulation or sparse topology.

All 32 isolated GPU cases pass, including seven graph cases (odd parity, moving
colliders, zero/one/two/four substeps and parameter invalidation). The matched
direct/graph fixtures have identical reported errors against production HLSL;
the numerical tolerances were not relaxed. The 50 engine scenarios also pass in
graph mode: 47 successful rendered/solver reports plus three parameter rejection
cases. Use `test-cuda-engine.ps1 -Suite all -CudaGraphs`. Normal CUDA-OFF Release
and eight CTests pass; the Node matrix now has 231 passing tests. The unavailable
D3D12 GPU-validation layer remains an open environment limitation.

The initial 27-run comparison, `cuda-perf-graph-replay-manifest.json`, measured
only the render loop (excluding pre-render gameplay/UI). It motivated the complete
loop instrumentation below. Do not interpret its older `frame` column as the
whole-frame acceptance metric.

Three same-binary 320-frame optical sequences (`cuda-graphs-temporal`,
`cuda-direct-graphs-reference`, `dx12-graphs-reference`) pass the existing raw and
DLSS-RR preservation checks with whitewater and FG off. Across both comparisons,
maximum absolute mean-brightness change is 0.019%, maximum raw temporal-difference
increase is 0.892%, and maximum RR increase is 1.66% (gate: 5%). This retains the
previous behavior; it is not a new denoising improvement. Water atlas/RR histories
remain intact, and paused foam does not drift during camera movement.

### Latency instrumentation

`--profile-latency --frames=N` records every frame, including cold start, in a
preallocated bounded host array. The full-loop timer includes input, gameplay,
UI update, render submission, the existing GPU completion wait and presentation.
Memory-query cost is included and separately visible. Startup begins at
`wWinMain`, not before DLL loading. CUDA reports its CPU enqueue duration, GPU
work interval, ownership span, handoff remainder and cumulative graph setup.
The work interval includes GPU idle gaps from submission, not just busy cycles.
No new GPU wait or particle/grid readback is added.

VRAM is [DXGI's per-process local-segment usage](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiadapter3-queryvideomemoryinfo),
sampled once per completed frame with its current budget. Its maximum is a
**sampled** high-water mark, not an exact subframe allocation peak or device-wide
free-memory measurement. Simulated seconds accumulate actual advanced substeps;
dropped time is reported separately.

`profile-cuda.ps1` compares DX12/direct CUDA/graph CUDA in the same executable.
The additional `bursts` sequence starts with a closed inlet, opens it at frame
index 90, and drops the ball from 2 metres at index 180. Reports separate startup,
warmup, steady state, inlet onset, flow, collider-drop and settling windows. These
exercise existing fluid/render work changes, **not future sparse allocator
transactions**. `validate-cuda-profile.mjs` independently rejects missing samples,
lost physical time, omitted CPU duration, invalid memory and FG/validation runs.

Sparse pool/backlog measurements, per-kernel timing, conservative adaptive MAC
coupling and variable-radius surface/caustic seams still require implementation
and validation. This checkpoint does not complete the multiscale contract.

### Measured whole-frame acceptance

`cuda-perf-latency-manifest.json` records 36 same-build runs: four scenarios,
three backends/schedules, three alternating-order repeats. Every run renders 300
real 1920×1080 Balanced frames, advances 600 identical 120-Hz fluid substeps,
disables FG and numerical probes, and retains all cold/steady samples. All 36
reports also pass the independent Node validator. No simulation time is dropped.
Table entries are medians across repeats of each run's **median / p99**, in ms:

| Scenario | DX12 | Direct CUDA | Graph CUDA |
| --- | ---: | ---: | ---: |
| Play | 16.51 / 24.53 | 18.78 / 30.94 | 14.76 / 19.47 |
| Orbit | 14.76 / 19.98 | 16.83 / 30.20 | 13.77 / 19.99 |
| Rolling | 16.89 / 27.91 | 18.57 / 35.08 | 15.24 / 21.74 |
| Inlet / collider bursts | 14.67 / 27.81 | 15.77 / 32.34 | 13.50 / 21.68 |

Graph CUDA's simulation medians are 2.97–3.16 ms, comparable to DX12's
2.97–3.19 ms; direct CUDA is 6.79–7.15 ms. CUDA CPU enqueue medians fall from
4.72–5.43 ms to 0.09–0.12 ms. The graph handoff remainder is approximately
0.21–0.22 ms. These observations support a launch-scheduling benefit, not a
different numerical algorithm or a general CUDA hardware-throughput claim.

The aggregate median first-frame durations are roughly 0.19–0.22 seconds for
graph CUDA versus 0.09–0.10 seconds for DX12, after approximately 2.6–2.8 seconds
of application setup. Sampled process-local memory is approximately 0.97 GB for
DX12 and 1.49 GB for CUDA; graphs add about 2 MB over direct CUDA. Inlet and
collider windows retain their separate deadline/max distributions in the manifest.
Steady-state maxima across repeats still reach 76.56 ms for graph CUDA; a median
or p99 table must not conceal that outlier. Repeated-run variation is visible.

The 60-raw-FPS **p99 gate is not met**. Graph replay is preferable to direct CUDA
for this tested baseline, but normal launchers are not promoted to CUDA and the
adaptive/multiscale goal is not complete. Per-kernel bottleneck accounting,
bounded topology backlogs, coupled multilevel physics and optical seams remain
the next implementation/acceptance work.

## Sparse storage / mixed pressure fixture — 14 September 2026

`cuda/fluid_cuda_bricks.*` adds a persistent pool of physical 4³ bricks with
double-buffered fields, page mappings and active masks. A bounded page-change
budget spans all substeps in a rendered frame. Partial allocation remains staged;
only a complete initialized candidate can publish. GPU counters expose required,
resident, missing, overflow, allocation/retirement and peak-change counts. Cached
page reservations are not a claim that the inactive field bank contains live data.
This first bounded-domain implementation uses a dense virtual page table (at most
16,384 virtual bricks), not a general world-space hash or sparse hierarchy.

`cuda/fluid_cuda_mac.*` connects that pool to the existing two-level MAC
discretization as an **isolated CUDA module**, not a game setting. Pressure rows
and ping/pong values occupy resident bricks. The fine face arrays remain the APIC
transfer cache. Each coarse cell has one pressure unknown, shared normal face
fluxes and compatible reconstruction of its twelve interior child faces. The
mixed operator retains positive fine-sibling terms at coarse/fine T junctions;
this is one coupled solve, not independent pressure solves on each level.

Classification retains the existing two-cell wet/solid guard, velocity-range
hysteresis and eight-substep demotion age. An additional solid-velocity travel
margin forces local fine resolution. This is only an initial predictive guard:
advected flags, emitter sweeps, minimum wake residency, defined temporal error
and periodic fine error probes remain required. The page-change budget does
**not** yet bound all LOD flips, row assembly or hierarchy rebuild work.

When a requested topology exceeds residency/work capacity, the module explicitly
counts a deferral and runs the **complete unchanged fine CUDA projection** for
that substep. It never solves just the resident fragment or deletes fluid to fit.
This is a temporary dense-reference path, not accepted sparse scaling. A malformed
operator instead poisons the transaction: further projection writes and publication
stop. It must not fall back after mixed restriction has overwritten fine-stencil
scratch. Runtime wiring must propagate this status through the engine's error
handling before accepting the frame; that wiring is still outstanding.

Pool and MAC construction explicitly finish default-stream initialization before
returning to a nonblocking interop stream. This is a one-time setup wait, not a
per-frame synchronization. The numerical fixture exposed why relying on the
return of `cudaMemset` alone was unsafe.

`tests/cuda_bricks_gpu.cu` covers budgeted retirement/allocation, front-state
preservation, exhausted capacity, invalid-field rejection and graph replay.
`tests/cuda_mac_gpu.cu` adds ten numerical cases: rejected configurations,
bit-identical forced-fine and capacity-deferral results, budget recovery, closed
and free-surface mixed domains, captured graph execution, predictive solid
promotion, partial boundary bricks/even pressure parity and invalid-operator
rejection. The CPU oracle constructs a matrix independently from face energies
and integrated fluxes. It checks T-junction siblings, pressure gradients,
prolongated child divergence and FLIP cache preservation.

On the tested RTX 5090, the mixed fixtures' matrix coefficient error is at most
1.91e-6; RHS/actual-divergence disagreement is below 6.2e-9 per second and face
gradient error below 5.2e-10 metres/second. RHS acceptance uses physical divergence
units, matching the existing DX12 oracle, not an arbitrary absolute pressure
tolerance. **Remaining divergence** after 120/121 relaxation iterations is
1.53e-4–3.79e-4 per second: this does **not** pass the production 1e-4 target.
Accurate matrix/flux agreement must not be confused with a converged solve.

Next: add the coupled multilevel pressure preconditioner and true-residual gate,
bound topology/hierarchy work, then integrate and audit completed substeps. The
existing particle-owned fine baseline still carries all liquid mass. Conservative
bulk/particle ownership, variable-radius surfaces, optical seam acceptance and
equal-error end-to-end performance remain unfinished. No new adaptive FPS,
VRAM advantage, arbitrary-mesh collision or rendered caustic result is claimed
by these isolated tests. Ordinary DX12 and the playable CUDA baseline are unchanged.

Validation at this checkpoint: `test-cuda.ps1` passed all 47 cases and wrote
source/binary/shader hashes to `bin/Release/cuda-validation.json` (schema 3).
Twenty additional fresh-process MAC runs passed all 200 cases. CUDA Compute
Sanitizer memcheck reported zero errors for the ten MAC fixtures; its log is
`bin/Release/cuda-mac-memcheck.log`. This is CUDA memory checking, not the
unavailable D3D12 GPU validation layer. All 8 Windows CTests and 231 Node tests
also passed. Both CUDA-enabled and ordinary CUDA-OFF Windows Release lab builds
passed; existing Bullet/RmlUi header warnings remain. No new full-game
rendering/performance capture was taken for this isolated module.

## Coupled CUDA MGPCG checkpoint — 14 September 2026

The isolated MAC API now accepts `MacConfig.multigrid = true` and a bounded
`cgIterations` count (default/maximum 32). `fluid_cuda_pressure.*` supplies a
multilevel preconditioner for the **same coupled fine/2h physical operator**.
This is not yet an engine command-line setting; the playable uniform CUDA
substep continues to use its unchanged reference pressure path.

The correction hierarchy starts with 4h aggregates containing complete 2h
leaves, then doubles until the bottom contains at most 64 cells. Restriction
sums integrated residuals. Galerkin rows keep an explicit air-boundary diagonal
and bit-identical shared face weights. Two pre/post Jacobi sweeps per level
and a GPU Cholesky bottom solve form the preconditioner. A small bottom shift
regularizes only that preconditioner; no shift is added to physical pressure.
All storage is allocated once and all iteration decisions remain GPU-side.

Physical matrix application, RHS flux summation, pressure accumulation and
pressure-gradient subtraction use FP64. The true residual is assembled directly
from the canonical face-flux operator, including T-junction averaging and solid
boundary conditions. It is not inferred from a rounded diagonal row sum. This
preserves constant-pressure Neumann null modes and avoids false divergence from
cancellation of large neighboring pressures. FP32 rows, Krylov vectors,
smoothers and coarse operators remain the working preconditioner representation.
Dot products use a fixed geometric reduction tree, independent of leaf append
order. Convergence requires both relative residual norm <= 1e-5 and maximum
physical divergence <= 1e-4/s, recomputed after every pressure update.

The exact shared-face velocity average uses the otherwise idle scratch y/z
channels as an FP64 bit representation; x retains the old FP32 average. Only
the precise mixed path interprets those channels. The pooled cell is now 240
bytes: the original 216-byte row and two FP32 legacy pressures, plus an FP64
physical RHS and pressure. The hierarchy and correction/Krylov buffers remain
bounded, persistent allocations; they are not a fully sparse fluid representation.

`PressureView` exposes per-solve/cumulative iterations, convergence, caps,
invalid-state counters, residual scalars and the bounded iteration trace for
deferred diagnostics. A cap is not reported as convergence: it poisons the
candidate before velocity projection/publication. The small empty-domain test
converges without any CG updates. Kernels are graph-capture-safe; converged
iterations skip their shader work. This first CUDA schedule still records all
bounded launch nodes and scans coarse capacity, rather than claiming conditional
graph launch elimination or fully compact hierarchy work.

The MAC suite now contains 18 cases. In addition to the prior ten, it checks
closed/free-surface MGPCG, graph replay, a deeper correction hierarchy with a
prescribed moving wall separating liquid regions, a high-pressure column,
deliberate iteration-cap rejection, an empty domain and a manufactured affine
pressure field. The independent CPU oracle checks `P^T A P`, exact shared-face
symmetry, the bottom Cholesky factor, physical residual versus actual projected
divergence, and the recovered analytical pressure up to its volume-weighted
Neumann gauge. These are numerical projection fixtures, not full moving-body
fluid transport or rendered gameplay sequences.

Final tested states converge in 10–16 CG iterations, with maximum measured
divergence 1.75e-5/s across the nonempty MGPCG fixtures. The manufactured affine
case recovers pressure to 0.020 Pa after gauge removal and leaves at most
1.15e-6 m/s of its manufactured gradient velocity. The old 120/121-sweep
relaxation results above remain valid reference measurements, not the new
solver's convergence results. No adaptive FPS or end-to-end speedup follows
from the smaller iteration count.

Remaining at that checkpoint before runtime integration: propagate failed-solve status through
the **whole** substep so G2P, advection and density repair cannot consume a
rejected candidate; provide a convergence-qualified fine path during pool
deferral (the current explicit fine fallback is still bounded legacy Jacobi);
and validate complete substeps against the existing DX12 mixed solver and
matched fine reference. Then bound topology/hierarchy work, implement the
conservative bulk/particle ownership transactions and validate surface/optical
seams. The full CUDA/adaptive contract remains active and incomplete.

Validation: all 55 CUDA cases passed with current source/binary/shader hashes
in `cuda-validation.json`; the MAC executable contributes 18 of those cases.
Compute Sanitizer `memcheck` and `initcheck` each reported zero errors for that
executable (`cuda-mgpcg-memcheck.log`, `cuda-mgpcg-initcheck.log`). Both Windows
Release lab builds passed, along with all 8 Windows CTests and 231 Node tests.
Existing third-party Bullet/RmlUi header warnings remain. D3D12 GPU validation
is still unavailable, and no new gameplay/optical/performance acceptance is
claimed for the isolated MGPCG module.

## Convergence-qualified pool fallback — 14 September 2026

The precise MAC path no longer substitutes fixed-iteration Jacobi when its page
budget or pool capacity cannot accommodate a complete candidate. It solves the
entire fine pressure domain with the same coupled MGPCG implementation, physical
face operator and convergence thresholds. Fine fallback rows are generated from
solid/liquid geometry; they do not dereference absent pages or reuse a partially
restricted mixed-grid stencil. Proposal topology and solve topology now have
separate accessors, so classification can continue staging a mixed candidate
without changing the fine fallback's physical operator.

The only additional persistent fine storage is an interleaved FP64 RHS/pressure
cache, **16 bytes per fine cell** (16 MiB at the maximum 1,048,576-cell domain).
It reuses the existing Krylov buffers, multigrid hierarchy and reductions. This
is an explicit bounded dense safety path, not a claim that the entire solver is
sparse. No allocation, CPU iteration decision or particle download occurs during
these solves. Legacy non-MGPCG mode retains its bit-identical Jacobi fallback;
it remains a reference, not a convergence-qualified production path.

The sparse front field, ownership, active mask and version remain unchanged
while the candidate is incomplete. A converged fallback exports fine pressure
and fine velocities through the existing output views; it does **not** publish
the mixed candidate. Page staging can subsequently recover to a complete
resident solve. The added fixture exercises both initial deferral and a liquid
domain growing beyond an already-published pool, followed by recovery.

Iteration-cap or invalid-operator failure is sticky. Later queued projection
graphs cannot clear the first pressure failure, allocate more pages, reset its
hysteresis, overwrite velocities or publish pressure. Recovery from such a
numerical failure requires an explicit subsystem reconstruction; merely calling
`beginMacFrame(reset=true)` is not a failure reset. Ordinary capacity deferral
is not a failure and continues making bounded page progress. This is still
projection-level safety: downstream G2P/advection/density stages have not yet
been connected to that GPU failure latch in the full solver.

The MAC fixture now has 24 cases. Six new cases cover resident fine MGPCG,
capacity fallback with free surfaces/capillarity/moving-wall boundaries/partial
bricks, closed high-pressure fallback, budget-limited graph recovery, preservation
and recovery of an existing published front, and capped fallback rejection.
Both cap tests also replay four subsequent queued graphs and verify that the
failure, topology counters, history and velocities remain unchanged. The same
independent face-energy/Galerkin/flux oracle audits fine and mixed solutions;
fallback velocities also agree with a separately resident all-fine solve within
2e-8 m/s, without changing FLIP cache/weight metadata.

New successful fine/fallback final states converge in 12–15 iterations; the
worst measured divergence is 1.783e-5/s, below the 1e-4/s gate. This is numerical
evidence, not a latency result. The static iteration schedule, coarse-capacity
scans and full fine fallback can still cause costs that must be profiled and
bounded before claiming realtime adaptivity. Precise mode now omits the old
Jacobi launch loop altogether rather than recording its no-op kernels.

Next: wire the sticky GPU failure predicate through complete composed CUDA
substeps, including graph replay, collision and density repair; qualify the
full mixed-pressure solver against the existing DX12 and fine references;
then expose an explicit runtime mode. The playable uniform CUDA and ordinary
DX12 paths remain unchanged. Conservative bulk/particle transfers, predictive
refinement, bounded hierarchy/topology work, canonical surface seams and
end-to-end optical/performance gates remain required by this contract.

Validation: all 61 CUDA cases passed (24 MAC cases), with refreshed
source/binary/shader hashes and `qualifiedFineFallbackFixture=true` in
`cuda-validation.json`. Compute Sanitizer `memcheck` and `initcheck` both found
zero errors (`cuda-fallback-memcheck.log`, `cuda-fallback-initcheck.log`). Both
CUDA-enabled and ordinary CUDA-OFF Windows Release lab builds passed, retaining
the existing third-party Bullet/RmlUi warnings. All 8 Windows CTests and 231
Node tests passed. These are numerical/interop checks;
the report explicitly retains `rendererExercised=false` and `multiscale=false`.
No new gameplay, optical seam or adaptive performance acceptance is claimed.

## Composed mixed-pressure solver / guarded publication — 14 September 2026

`Config.mixedPressure=true` now connects the coupled MAC/MGPCG module to the
actual complete CUDA solver: binning, P2G, classification, forces/viscosity,
capillarity, pressure, face extrapolation, G2P/advection, moving SDF contacts,
density repair and conditional second repair. `forcedFinePressure` selects the
same qualified pressure path without coarsening for reference comparisons.
`pressureBrickCapacity`, `pressureChangesPerFrame` and `cgIterations` configure
the bounded pool/solve. The page budget starts once per rendered submission,
not once per substep. Unsupported ballistic/partial-test/ownership combinations
are rejected rather than silently running a partial mixed solver.

`fluid_cuda_transaction.*` owns a persistent private working set of the existing
14 fields. At frame start a GPU snapshot takes authoritative shared state,
including particles modified by the engine. All composed substeps operate on
that working set. A GPU-guarded copy publishes all fields only after the entire
submission succeeds. On failure the renderer-facing fields and their allocation
tails are unchanged, even if earlier substeps in the same submission succeeded.
Logical velocity and selected pressure are published into canonical external
views (`State{false,0}`), so speculative ping-pong parity never changes the
renderer’s interpretation of a rejected frame. Both engine imports and the
transaction use the same `bufferBytes()` ABI sizing routine.

This deliberately adds a bounded **GPU-to-GPU snapshot and publication per frame**,
not per substep/iteration. The 101,376-particle fixture allocates 11,951,792 bytes
(about 11.4 MiB) of logical staging capacity, separately from the pressure pool
and CUDA runtime allocations. That is not total measured VRAM. Copy time and
actual peak VRAM must be included in the later frame-budget comparison; neither
is claimed free. There is still no asynchronous simulation/render overlap.

Grid, transfer and contact kernels receive the sticky GPU failure predicate.
G2P, advection, density displacement and collisions cannot advance particles
after a rejected pressure solve. Later queued projection passes retain the first
failure. Binning can still rebuild **private** lookup scratch for frozen particles;
this first implementation does not claim conditional elimination of all remaining
graph nodes/CUB work. No rejected working set is copied to the renderer.

One 56-byte completion record carries pressure convergence/caps, iterations,
coarse count peak, page changes, residency and backlog. It is copied asynchronously
after publication and consumed through `collect()` at the existing owner frame
fence. There is no host pressure-iteration wait or particle/grid readback. A
failed submission reports its cause/iteration count and permanently poisons the
solver; recovery requires explicit subsystem recreation. The CUDA interop signal
is still queued normally, so a numerical failure is not a callback exception
that strands a DX12 semaphore wait. Telemetry distinguishes submitted work from
completed/rejected frames and includes staging/pressure allocation sizes.

The new `NVMatrixEngineCudaSolverTest` covers twelve composed cases: qualified fine
APIC direct/graph/moving-wall comparisons, mixed budget recovery, sustained
capacity fallback, direct/graph/fallback rejection, empty/paused operation,
late-substep failure in both schedules, and 101,376-active-particle FLIP. The
late-failure fixture first advances a valid free surface, then deliberately
creates incompatible inward closed-domain boundary flux. It verifies that
private particles equal the last valid substep while **every shared field**
still equals the original frame. Guards also preserve particle mass, inactive
particles, APIC side channels and 64-byte allocation sentinels.

The tall-pool fine oracle needed 6,000 Jacobi iterations: 1,000 iterations still
left approximately 0.0018 m/s of velocity error and was not an accuracy-qualified
reference. The new reference explicitly verifies the same 1e-4/s divergence
gate; production Jacobi limits are unchanged. Against that converged reference,
the tested composed fine cases differ by at most 2.39e-7 m in position,
3.53e-7 m/s in velocity and 2.18e-6/s in APIC terms. Mixed direct/graph results are
bit-identical in the tested budget/capacity sequences. The 100k fixture exercises
900 coarse leaves at peak and at most 15 CG iterations; these counts are not
an end-to-end performance result.

Three additional production-HLSL interop cases exercise the **forced-fine**
composed MGPCG path, including moving colliders, graph replay, FLIP and density
repair. They retain the existing field-specific comparison thresholds. Together
with the independent coupled face-energy/Galerkin oracle, this tests integration
and pressure discretization, but does not yet constitute a full mixed-CUDA versus
mixed-DX12 gameplay comparison or an optical seam test.

CUDA-enabled CMake configuration now fingerprints the CUDA headers into compiler
commands for the library and ABI consumers. This prevents Windows/WSL include
tracking from silently linking a newly compiled interface to stale CUDA objects.
The supported build/test scripts configure explicitly before building; manual
CUDA builds in the regeneration-disabled WSL build tree must do the same.

Next: expose an explicit playable mixed-pressure mode and validate complete
rendered sequences plus raw-frame timing, including staging and fallback costs.
Then continue bounded topology/hierarchy scheduling, conservative bulk/particle
ownership, predictive refinement and canonical variable-resolution surface
acceptance. Neither this checkpoint nor the unchanged faster default solver
fulfills the complete adaptive CUDA contract yet.

Validation: all 76 CUDA cases passed with refreshed source/binary/shader hashes
in `cuda-validation.json` (19 core, 24 MAC and 12 composed-solver cases, plus
the existing interop/transfer/brick tests). The report marks composed mixed
substeps and transactional publication as exercised, while retaining
`rendererExercised=false` and `multiscale=false`. Compute Sanitizer memcheck and
initcheck both report zero errors for **both** the composed solver and MAC
executables (`cuda-composed-Solver-{memcheck,initcheck}.log` and
`cuda-composed-Mac-{memcheck,initcheck}.log`). Both CUDA-enabled and ordinary
CUDA-OFF Windows Release builds passed, as did all 8 Windows CTests and 231
Node tests. Existing Bullet/RmlUi header warnings remain; D3D12 GPU validation
is still unavailable on this installation. No new game-render/performance
capture is claimed at this checkpoint.

## Playable pressure modes and measured cost — 14 September 2026

The runtime controls described above now have complete engine-render coverage.
[Play CUDA Mixed Pressure Lab.cmd](Play%20CUDA%20Mixed%20Pressure%20Lab.cmd) opens
the experimental mixed-pressure room with CUDA graphs and frame generation off.
The regular CUDA launcher still uses uniform pressure, and ordinary builds still
default to DX12. This is a playable pressure milestone, **not** a completed
adaptive particle/grid/surface system.

### Validation

- Both CUDA-enabled and CUDA-OFF Windows Release builds pass. All 8 Windows
  CTests and 231 Node tests pass. Existing third-party header warnings remain.
- All **78 CUDA GPU cases** pass: 8 interop, 8 transfer, 19 core, 5 brick, 25 MAC
  and 13 composed solver cases. This includes final-FP32-flux rejection and
  paused refinement-history reset. `cuda-validation.json` records source,
  executable and shader hashes; separate engine manifests cover rendering.
- Compute Sanitizer **memcheck, initcheck and synccheck** each report zero
  errors for both MAC and composed solver executables. Evidence is in
  `cuda-runtime-{Mac,Solver}-{memcheck,initcheck,synccheck}.log` beside the binaries.
  D3D12 debug/GPU validation remains unavailable on this installation; CUDA
  sanitizer success is not a substitute for that missing check.
- Eight malformed CLI combinations and seven malformed telemetry reports are
  rejected; a valid report passes. Unsupported partial solver fixtures remain
  explicitly skipped rather than being counted as complete MGPCG coverage.
- Twelve distinct engine-validation sequences pass: five mixed room scenarios,
  four mixed surface scenarios, direct-schedule solver controls, and forced-fine
  still/flow room references. Room controls also pass three repeat runs. These
  exercise falling/splashing liquid, moving colliders, valve and capacity controls,
  pause/single-step/reset and explicit teleports.
- The long-flow run completes **1,800 pressure solves** with no rejected or capped
  solve. Final stored-face divergence is 2.99828e-5/s, peak CG iterations 11,
  final page backlog zero. Deeper falling/splashing fixtures exercise up to 640
  coarse leaves; the moving-collider fixture reaches 400, with stored-face
  divergence 4.76837e-5/s and peak CG iterations 18. These counts do not establish
  an adaptive performance win.

Three 320-frame temporal runs cover uniform, forced-fine and mixed pressure, each
with twenty consecutive captures across five camera/interaction phases. Guide,
energy, paused-foam and water-shadow-history checks pass; RR history and water
atlas resets remain at one in the mixed/fine comparison. Top-down floor mean
luminance is 0.721504 mixed versus 0.721477 forced-fine. Their consecutive-frame
relative changes are 0.006889 and 0.006914 respectively. These are scene/capture
metrics, not an estimator-bias proof, and this shallow scene does not exercise
variable-resolution optical seams.

### Matched raw-frame measurements

`cuda-perf-qualified-pressure-manifest.json` records twelve sequential runs from
the same executable and shaders: RTX 5090, 1920×1080 Balanced, FG off, 300 frames
and 600 substeps per run, five simulated seconds, no dropped simulation time.
The first 32 frames are separated from the 268-frame steady interval. These are
**one repetition per scenario/mode**, useful for bottleneck selection rather than
a repeated p99 acceptance claim. Minor CPU-side capture checking overlapped part
of the initial play run; repeat cleanly before treating its latency as a baseline.

Whole-frame median / p99 in milliseconds (including simulation, UI and present):

| Scenario | Uniform CUDA graphs | Forced-fine MGPCG | Mixed MGPCG |
| --- | ---: | ---: | ---: |
| Play | 13.50 / 19.91 | 22.28 / 28.49 | 22.41 / 27.20 |
| Orbit | 13.11 / 17.69 | 21.69 / 29.81 | 21.69 / 27.59 |
| Rolling | 14.13 / 21.44 | 23.35 / 32.18 | 23.45 / 30.70 |
| Inlet/impact bursts | 13.57 / 16.61 | 20.63 / 24.59 | 20.68 / 25.38 |

Fluid GPU medians are 2.96–3.04 ms for uniform pressure and 11.74–13.42 ms for
the MGPCG paths. Every steady MGPCG frame in these runs misses 16.67 ms. Recorded
outliers include 108.51 ms for mixed orbit and 132.69 ms for forced-fine rolling;
they are retained in the manifest, not removed from the claim. Frame-sampled
DXGI local usage increases by about 110 MiB, from approximately 1,422 to 1,532 MiB;
this is not an exact subframe VRAM high-water measurement.

The room has **zero eligible coarse leaves** under the conservative six-cell
interior guard, so mixed pressure has no coarse interior to exploit. All modes
retain the same particle and surface resolution. Moreover, uniform fixed-count
Jacobi is a cost reference, not the equal-error reference for convergence-gated
MGPCG. The evidence supports retaining the faster baseline, not claiming that
adaptive fluid is intrinsically slower or that full adaptive acceptance is met.

### Kernel attribution and next optimization

A separate Nsight Systems node-level CUDA-graph capture of 64 mixed room frames
(128 substeps, 960×540 Balanced) identifies pressure work as the next target:

- `dot` reductions / physical-operator application: 18.8% of CUDA kernel duration.
- `restrictBase` pressure restriction: 14.8%.
- Pressure hierarchy `assemble`: 11.2%.
- GPU transaction snapshot/publication `copyFields`: 1.3%, about 0.146 ms/frame
  in this instrumented capture. It protects rollback and is not the dominant cost.

The fixed 32-iteration graph also launches many kernels that return after early
convergence. Optimize active operator/gather work and investigate bounded
conditional iteration scheduling; do not attribute all added time to graph
no-ops or remove the final-face/publication checks to improve timing. Node-level
graph tracing itself adds overhead, so this capture is **kernel attribution**, not
another raw-frame performance result. Artifacts are
`cuda-qualified-mixed-kernels.nsys-rep`, its SQLite export and
`cuda-qualified-mixed-kernels_cuda_gpu_kern_sum.csv` in the CUDA runtime directory.

Remaining contract work includes bounded hierarchy/topology scheduling,
predictive refinement, conservative bulk/particle ownership and density changes,
actual multiresolution transfers, canonical variable-resolution surface/caustic
acceptance, and repeated equal-error deadline/VRAM qualification. ST-FLIP and a
pressure-coupled air phase remain deferred. No default or performance promise is
changed by this checkpoint.

## Parallel pressure gathers — 14 September 2026

The measured pressure hotspots now have two focused optimizations:

- First-level hierarchy assembly and residual restriction use one GPU thread
  per child, two 4³ aggregates per 128-thread block. This replaces each parent's
  serial walk through up to 64 sparse rows. Shared-memory reduction retains
  geometric child order, handles incomplete edge blocks, and does not depend on
  atomic active-list ordering. Higher hierarchy levels and canonical symmetric
  face weights retain the existing construction.
- With no coarse leaves, the physical operator uses its exact six-face form.
  It still sums FP64 pressure differences, preserves solid/free-surface boundary
  treatment and closed-domain null modes, and covers qualified fine fallback.
  Actual mixed interfaces retain the adjoint patch operator. Fine smoothing also
  reuses its fetched row rather than requesting that row twice.

No convergence tolerance, timestep, particle count, reconstruction, light budget
or shader changed. Floating-point execution order can differ; identical
trajectories are not assumed. Independent face-energy/Galerkin, fine-reference,
actual published-flux and failure-publication gates remain in force. There are
no new persistent device allocations or CPU readbacks.

### Repeated before/after timing

The previous executable was preserved as
`NVMatrixFluidLab-before-pressure-gathers.exe` (SHA256 beginning `CFAEBADE224A8C24`),
and compared with the optimized build (`7E2C7A259251CF8A`). The profiling script
now accepts `-Executable` and `-ModeFilter` so this comparison is reproducible
without replacing the installed executable. Full hashes and unchanged shader
hashes are recorded in `cuda-perf-gathers-{before,after}-{1,2,3}-manifest.json`.

Six sequential runs alternate before/after order across three repetitions of
the same mixed-pressure play scenario: RTX 5090, 1920×1080 Balanced, FG off,
300 frames / 600 substeps / five simulated seconds per run. Each repetition
separates the first 32 warm-up frames. All six pass the independent raw-profile
validator and report no capped/rejected pressure solves or dropped simulation
time. No builds, GPU tests or capture processing ran alongside these timings.

| Metric | Before | Optimized |
| --- | ---: | ---: |
| Whole-frame median, pooled 804 steady frames | 22.3164 ms | 17.5514 ms |
| Whole-frame p99 | 30.2115 ms | 26.6296 ms |
| Whole-frame maximum | 56.4588 ms | 76.3688 ms |
| Frames exceeding 16.67 ms | 804 / 804 | 684 / 804 |
| Median of three fluid GPU medians | 12.4894 ms | 7.84464 ms |

This is approximately **21% less whole-frame time and 37% less fluid GPU time**
for this particular convergence-qualified path. Each repetition independently
shows the reduction (whole-frame medians 22.30–22.32 versus 17.53–17.58 ms).
The larger worst-frame outlier is retained; this is not a claim of rock-solid
60 raw FPS. Logical staging/pressure allocations and the 4,366 captured graph
nodes are unchanged. There are still no eligible coarse leaves in this shallow
room, so this measures implementation efficiency, not adaptive-resolution savings.

The faster uniform solver remains the default. Conditional elimination of
post-convergence graph nodes, bounded topology work, conservative adaptive
particle/bulk ownership and variable-resolution optical acceptance remain open.

### Validation and follow-up attribution

The optimized Windows CUDA Release build passes all 78 GPU cases, including
the independent mixed operator/hierarchy oracle and complete 100k-particle
substeps. All six MAC/Solver Compute Sanitizer memcheck/initcheck/synccheck runs
report zero errors (`cuda-gathers-{Mac,Solver}-{memcheck,initcheck,synccheck}.log`).
All 240 engine/native Node tests and the 8 existing Windows CTests pass. D3D12
debug/GPU validation remains unavailable; this change only modifies CUDA code
and the profiling helper, not the ordinary renderer or DX12 solver.

Nineteen engine/scenario fixtures pass: four mixed room cases, twelve surface
cases (including empty and analytic surface fixtures), direct solver controls,
and two forced-fine room references. The 900-frame flow case completes 1,800
solves without rejection/caps, has zero final page backlog and peak CG count 11.
Its independently measured final-face divergence is 4.72879e-5/s, with GPU/CPU
flux mismatch 1.05697e-12/s. Dynamic mixed surface tests exercise 640 coarse
leaves, and moving-collider tests exercise 400; all retain the external 1e-4/s
flux limit. Source/executable/shader hashes are in the refreshed CUDA and engine
validation manifests.

Two new 320-frame mixed/fine temporal sequences pass capture validation, with
finite guides, accounted photon energy, frozen paused foam, and uninterrupted RR
and dynamic-water history. Both histories remain at one reset; the separate
general scene-atlas reset counter is not the water-history counter. Top-down
floor relative changes are 0.00688928 mixed and 0.00688977 forced-fine, with mean
irradiance Y 0.721377 and 0.721225. These are capture metrics, not an optical seam
or full perceptual acceptance claim. Reproduce with
`check-water-shadow-history.mjs <runtime> cuda-gathers-mixed-temporal cuda-gathers-fine-temporal`.

A matching 64-frame/128-substep node-level Nsight capture confirms that the
targeted kernel work shrank. Summed kernel durations in the instrumented captures:

| Pressure work | Before | Optimized |
| --- | ---: | ---: |
| Physical operator / dot kernels | 133.970 ms | 46.203 ms |
| First-level residual restriction | 105.748 ms | 13.013 ms |
| Pressure hierarchy assembly, all levels | 79.864 ms | 2.304 ms |
| Transaction snapshot/publication | 9.325 ms | 9.350 ms |

These are sums across the instrumented run, **not frame times**; graph-node
tracing overhead prevents substituting them for the raw timing table. Artifacts
are `cuda-pressure-gathers-kernels.nsys-rep`, its SQLite export and
`cuda-pressure-gathers-kernels_cuda_gpu_kern_sum.csv`. Pressure smoothers,
reductions and the static post-convergence nodes remain meaningful next targets;
removing the rollback copies is still not justified by their measured cost.

For the next scheduling experiment, the installed toolkit's
[CUDA 13.1 conditional-graph contract](https://docs.nvidia.com/cuda/archive/13.1.0/cuda-c-programming-guide/index.html#conditional-graph-nodes)
supports device-controlled WHILE bodies and capture into their owned graphs.
Its allowed body nodes exclude external-semaphore operations. The engine-specific
candidate is therefore to wrap only MGPCG iterations, keep DX12 handoff outside,
reset the condition on every replay, and enforce the existing iteration cap on
the GPU. Retain the direct/unrolled path for parity and test empty, capped,
rejected, paused/reset and graph-rebuild sequences. This scheduling change is
**not implemented** by the gather optimization above.

## Device-controlled pressure iterations — 14 September 2026

The CUDA MGPCG graph now supports a bounded WHILE body through
`--fluid-cuda-pressure-loop=conditional|unrolled`. It only affects captured
`fine`/`mixed` pressure; direct launches remain unrolled and uniform pressure
does not use this solver. The conditional selection is the default inside this
already opt-in experimental path. An unsupported conditional-handle API reports
an error with the unrolled selection as the explicit compatibility alternative.
It does not silently change pressure accuracy or replace the ordinary solver.

`Pressure` owns one reusable capture-only stream. During graph preparation,
`enqueuePressure()` attaches a conditional node to the current capture frontier,
captures one iteration into its owned body, then rejoins the outer stream after
that node. Initial convergence and final rejection/publication logic remain
outside the loop. DX12 external-semaphore handoff remains outside the complete
substep graph. There is no per-frame graph allocation or CPU iteration decision.

A single-thread device kernel sets the condition before entry and after each
body execution. Every graph replay starts with a reset condition, and empty or
already-rejected work explicitly disables it. The same iteration body runs in
direct, unrolled-graph and conditional schedules. Conditional reductions obtain
their iteration/trace index from the GPU counter; the existing maximum iteration,
relative residual and final stored-face divergence gates are unchanged. Reaching
the cap still rejects publication rather than being reported as convergence.

Telemetry includes `pressureLoop`, `pressureLoopSolves`, `pressureLoopIterations`
and `graphBodyNodes`. `graphNodes` includes nested static body nodes, not merely
the smaller top-level graph. Loop iterations count executed bodies, including
an attempted body that subsequently rejects; they are not a floating-point
convergence estimate. The deferred completion record grows from 64 to 72 bytes,
still once per submission, and the two GPU counters add eight persistent bytes.

The new composed fixture compares conditional and unrolled graphs bit-for-bit
through empty→wet→empty→wet transitions, moving colliders, reset and two physical
parameter changes that replace captured graphs. It verifies no body executions
on empty replays and exact accepted-iteration accounting. The fixture completes
24 solves / 216 body iterations, three four-variant graph generations, and counts
582 static nodes including 68 body nodes. Existing MAC and composed-solver tests
also pass, including direct/graph parity, coarse/fine interfaces, cap rejection,
late-substep rollback and the 100k FLIP case. Full sanitizer qualification
remains open; the successful uninstrumented tests do not waive that gate.

### Raw schedule comparison

Three alternating-order repetitions per schedule use the same executable and
shaders: RTX 5090, 1920×1080 Balanced, FG off, room inlet active, 300 frames /
600 substeps / five simulated seconds each. No competing build, GPU test or
capture analysis runs during timing. The first 32 frames are reported separately,
leaving 804 steady frames per schedule. Artifacts are
`cuda-perf-loops-{conditional,unrolled}-{1,2,3}-manifest.json` and their per-frame
reports. The measured executable SHA256 is
`28F63ED79AD1A7268E9395DCA976CB583008FA8559D6AEDA322C832241C59362`.

| Metric | Unrolled | Conditional |
| --- | ---: | ---: |
| Pooled whole-frame median | 17.4918 ms | 16.5756 ms |
| Pooled whole-frame p95 | 21.1191 ms | 20.1961 ms |
| Pooled whole-frame p99 | 34.0038 ms | 28.4046 ms |
| Worst retained frame | 150.997 ms | 66.7531 ms |
| Frames over 16.667 ms | 680 / 804 | 356 / 804 |
| Median of per-run fluid GPU medians | 7.82003 ms | 6.84928 ms |
| Static graph nodes, including nested bodies | 4,366 | 906 |

This is about 5.2% less median whole-frame time and 12.4% less fluid GPU time
for the convergence-qualified solver. Approximately 44% of conditional frames
still miss 60 Hz. These runs demonstrate scheduling savings, **not** the final
bounded-latency gate or adaptive-resolution savings: the shallow room still has
zero eligible coarse leaves. All six runs complete without pressure caps or
rejected frames. Full-game trajectories can differ between runs; the dedicated
composed fixture, not these live-game timings, establishes bit-exact schedule
parity. The ordinary uniform solver remains the faster default.

### Conditional-graph sanitizer investigation

The initial runtime memcheck run fails with CUDA error 999 at the first executed
conditional pressure graph. `NVMatrixEngineCudaConditionalTest` isolates the graph
mechanism without the solver or DX12: two device words, single-thread kernels,
replays requesting 0, 1, 7, 32, 0 and 8 iterations. It passes uninstrumented and
with an unrolled instrumented graph; runtime memcheck fails when its first
nonempty conditional body executes. Both the installed Compute Sanitizer
2025.4.0.0 and side-by-side 2026.3.0.0 reproduce this on driver 616.64. No driver,
Windows TDR setting or globally installed toolkit was changed.

Excluding only the handle-setting kernel avoids the runtime memcheck failure,
but is **partial coverage**, not a clean full check. The full MAC synchronization
checker likewise fails at the first conditional pressure graph; excluding that
single control kernel allows it to complete. Initialization checking passes the
full MAC and composed solver fixtures without exclusions. Compile-time memcheck passes
the standalone reproducer, yet instrumenting the entire numerical module later
fails inside sanitizer metadata handling in the budget-recovery fixture. A hybrid
instrumentation build also fails. These controls strongly implicate instrumentation
compatibility, but they do not prove the production program free of memory errors.
The failed logs are retained (`cuda-loops-Mac-isolated-memcheck.log`,
`cuda-loops-minimal-2026-conditional-memcheck.log`,
`cuda-loops-Mac-all-compiletime-memcheck.log`, and the hybrid logs).

`test-cuda-sanitizer.ps1` provides reproducible strict checks and explicit
diagnostic controls. The default has no exclusions and fails on the unresolved
runtime memcheck problem. `-MemcheckMode hybrid` selects separate test-only
instrumented targets. `-ExcludeLoopControl` explicitly requests partial memory
and synchronization coverage (initialization checking stays unfiltered) and
writes `cuda-sanitizer-partial-validation.json`, never the full
qualification manifest. Failed child exit/diagnostic output is an error even
when the tool also prints a zero-error summary. Compile-time targets require
CUDA 13.1+ and the matching sanitizer toolkit. They are excluded from normal
builds and never linked into `NVMatrixFluidLab`.

The loop control now has its own small compilation unit and passes only the
two counter pointers, iteration bound and conditional handle to its device
kernel. The numerical iteration body and publication gates are unchanged.
The raw table above predates this testability refactor; final-source validation
and timing confirmations are recorded separately below.

### Final-source regression checks

The refactored playable executable is
`2571A5F83541F41A1FA776632BFCB1A36C050BF4069770459752C9D038FDBE24`.
All 79 CUDA GPU cases pass again, including the bit-exact schedule/rebuild
fixture. The 240 engine/native Node tests and 8 Windows CTests pass, as do all
10 CLI rejection cases and the valid/11-invalid report-validator fixtures.
The CUDA-enabled and ordinary CUDA-OFF Windows configurations build successfully.
The test-only instrumentation flags/libraries are absent from the playable
target and its production CUDA library.

Nineteen rendered engine fixtures pass: four mixed room cases, twelve surface
cases, direct/unrolled room controls, and two forced-fine room references.
The 900-frame inlet run completes 1,800 conditional solves and 17,624 executed
iterations without caps, rejection or final page backlog. Independent final-face
flux divergence is 3.41563e-5/s, with GPU/CPU mismatch 7.63452e-13/s. The pool
surface fixture exercises 640 coarse leaves, and moving colliders exercise 400.
These deeper fixtures provide actual mixed-resolution pressure coverage; the
shallow playable room still cannot be used to claim adaptive savings.

Two 320-frame whitewater camera sequences compare the final conditional and
unrolled schedules. Both pass finite-guide, photon-energy, frozen-paused-foam
and history checks. Top-down floor-relative irradiance changes are 0.00689846
and 0.00690260 respectively, with mean Y 0.721466 and 0.721309. Water and RR
history reset counts stay at one; the separate scene-atlas counter is not the
water-history counter. Reproduce with:

```
node engine/check-water-shadow-history.mjs <runtime> cuda-loops-final-conditional-temporal cuda-loops-final-unrolled-temporal
```

This is temporal capture evidence, not complete optical-seam/perceptual
acceptance. Full conditional sanitizer qualification, deadline-tail reduction,
bounded topology work, conservative particle/bulk ownership and variable-scale
surface reconstruction are still outstanding.

### Final-build timing confirmation

The same three-repetition protocol after the control-kernel refactor confirms
the median gain (`cuda-perf-loops-final-{conditional,unrolled}-{1,2,3}-manifest.json`).
All six report hashes and 300-row timing contracts pass the independent Node
validator. No build, sanitizer or capture processing overlaps these runs.

| Pooled steady metric | Unrolled | Conditional |
| --- | ---: | ---: |
| Whole-frame median | 17.5977 ms | 16.5402 ms |
| Whole-frame p95 | 21.4642 ms | 20.0976 ms |
| Whole-frame p99 | 32.6648 ms | 51.5094 ms |
| Worst frame | 95.8923 ms | 143.122 ms |
| Frames over 16.667 ms | 681 / 804 | 347 / 804 |
| Median of fluid GPU medians | 7.84560 ms | 6.80634 ms |

The median gain is about 6.0% whole-frame / 13.2% fluid time. **The p99 and
worst frame are worse in this repetition set**, so the earlier tail improvement
is not repeatable evidence. Retain all outliers: median efficiency improved,
bounded-latency acceptance did not. Every conditional run still completes 600
loop solves, with loop/accepted-iteration counts matching and zero pressure caps
or rejected frames.

A single same-build uniform cost-reference run
(`cuda-perf-loops-final-uniform-manifest.json`) measures 13.6105 ms median whole
frame and 3.05430 ms fluid GPU time. It remains substantially cheaper, but uses
the old fixed-Jacobi acceptance contract rather than the convergence-qualified
MGPCG operator. This one uniform run is context, not a repeated equal-error
comparison or a claim that either path has met the realtime deadline gate.

The retained outlier rows identify a concrete next latency target. Conditional
run 2, frame 207 takes 143.122 ms whole-frame: CUDA work is 6.35632 ms, while
the measured DX12↔CUDA handoff interval is 126.847 ms. Frame 90 has 6.11498 ms
CUDA work versus 122.895 ms handoff. Unrolled run 1, frame 288 similarly has
7.76720 ms CUDA work versus 77.4264 ms handoff. These are measured intervals,
not proof of a particular driver or OS scheduling cause. Pressure iteration
arithmetic is not where those outlier frames spend most of their time. Investigate
queue/context scheduling and handoff before pursuing more median-only kernel
optimizations; do not remove required ownership fences to hide the wait.

The final-source partial sanitizer matrix completes all nine checks (standalone,
MAC and composed solver × memcheck/initcheck/synccheck). Logs have zero reported
errors and successful child/test exits. Exactly four checks exclude
`loopCondition`: MAC/Solver memory and synchronization checks. All initialization
checks and the standalone checks have no exclusions. The independently
hash-checked manifest explicitly records `partialCoverage=true`; this does not
close the outstanding full-qualification gate.

## CUDA-in-Graphics checkpoint — 14 September 2026

Playable executable SHA256:
`AE03E2FB96BAC25C835D8862F724AA95C5821CB8D87926C0C2A95E9AB2B09FA7`.
Tested on the same RTX 5090 / driver 616.64 / CUDA 13.1 configuration. CIG
reports an 86,016-byte graphics shared-memory limit. The strict fallback-disable
call succeeds. Streamline-aware queue submission remains unchanged; only the
native queue is supplied to CUDA context creation.

### Correctness and lifecycle evidence

- All 79 CUDA baseline/kernel/numerical cases pass after the interop changes.
  These are primary-context fixtures, not 79 new CIG solver tests.
- All nine dedicated CIG interop cases pass, including primary-context
  coexistence, nested scope/exception restoration, rejecting another submission
  queue and a compute queue, partial imports, injected enqueue failures and
  repeated resource teardown. Sixteen partial-construction failures leave the
  raw process handle count unchanged (519 → 519).
- Seventeen CIG full-engine fixtures pass: four mixed/graph room cases (flow,
  controls, Frame Generation, 900-frame inlet), all twelve surface cases, and
  direct/unrolled mixed room controls. The pool and moving-collider surface
  fixtures exercise real coarse pressure leaves; the shallow room does not.
- The long inlet run completes 1,800 conditional solves / 17,632 iterations
  with zero caps/rejected frames and no final page backlog. Independent final
  stored-face divergence is 1.11759e-5/s; GPU/CPU flux mismatch is 2.498e-13/s.
  The FG fixture presents 178 frames from 90 real frames, including 88 additional
  presents, while retaining all 180 physical substeps.
- The 240-frame uniform-CUDA CIG experience fixture passes the actual UI,
  sink/float, boat and underwater checks. Applying water settings rebuilds
  first a 600k-capacity / .20 m / 90 Hz system, then a 500k / .12 m / 120 Hz
  system. The recreated solver still reports CIG; private contexts are destroyed
  before replacement. Simultaneous multiple CIG contexts remain unqualified.
- Primary and CIG 320-frame whitewater camera sequences pass. Top-down floor
  relative irradiance deltas are 0.00689535 and 0.00689952 respectively; mean Y
  is 0.721112 and 0.721258. Water and RR reset counts remain one. Paused foam
  stays frozen. These checks do not claim complete optical/perceptual parity.
- Twelve CLI rejection cases, two valid / fourteen malformed report fixtures,
  240 Node tests and eight Windows CTests pass. Both CUDA-enabled and ordinary
  CUDA-OFF Release builds succeed (existing third-party warnings remain).

Artifacts live beside the executable: `cuda-validation.json`,
`cuda-interop-cig-validation.json`, `cuda-options-validation.json`,
`cuda-engine-*-cig-validation.json`, `cuda-cig-experience*.json` and
`cuda-context-{primary,cig}-temporal-*`. Engine manifests include source,
shader and binary hashes; mode assertions reject primary reports labeled as CIG.

Recheck the temporal captures with:

```
node engine/check-water-shadow-history.mjs <runtime> cuda-context-cig-temporal cuda-context-primary-temporal
```

### Same-build raw latency

Three repetitions per context and pressure mode; context order alternates
primary/CIG, CIG/primary, primary/CIG. Every run uses the same executable and
shaders, 1920×1080 Balanced, an active room inlet, FG off, no validation probes,
300 real frames and exactly 600 physical substeps (five simulated seconds).
First 32 frames remain in each report as warmup/cold-start data; each pooled
steady condition below has 804 frames. No compiler, sanitizer, other GPU test
or capture-analysis work overlaps these timing runs.

| Pressure | Context | Whole-frame median | p99 | Worst | Over 16.667 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Uniform Jacobi | Primary | 13.5500 ms | 48.0312 ms | 132.052 ms | 76 / 804 |
| Uniform Jacobi | CIG | 13.6698 ms | 46.0193 ms | 188.006 ms | 79 / 804 |
| Mixed MGPCG | Primary | 16.5447 ms | 33.7605 ms | 108.115 ms | 345 / 804 |
| Mixed MGPCG | CIG | 16.3178 ms | 69.4261 ms | 170.111 ms | 269 / 804 |

Median-of-run fluid GPU medians are 3.03805 → 2.94320 ms for uniform and
6.82778 → 6.61066 ms for mixed. Whole-frame mixed median improves about 1.4%,
but its tail worsens in this set. Uniform has no whole-frame median improvement.
The normal/reference context therefore remains `primary`. These numbers do not
establish a 60-Hz deadline win or an adaptive-pressure speedup: uniform is a
different fixed-Jacobi error contract, and all room runs still have zero coarse
leaves. The relevant comparison is CIG versus primary *within* a pressure mode.

All twelve reports pass independent timing-schema, report-hash, binary-hash
and shader-hash validation. Every mixed run completes 600 conditional solves,
has matching executed/accepted iteration counts, and has zero caps/rejections.
Evidence: `cuda-perf-context-{primary,cig}-{1,2,3}-manifest.json` and their
`*-play-1-cuda-{graphs,mixed}.json` reports. Reproduce individual conditions with
`profile-cuda.ps1 -Comparison pressure -CaseFilter '^play$' -Repeats 1`
`-ModeFilter '^cuda-(graphs|mixed)$' -Context primary|cig -Tag <unique-tag>`.

CIG lowers ordinary handoff overhead, not the large stalls. Mixed handoff
median falls 0.221888 → 0.0925762 ms. Yet CIG mixed run 2, frame 78 still takes
170.111 ms whole-frame: CUDA work 6.39370 ms, measured handoff 154.706 ms,
callback CPU enqueue 0.1884 ms. Uniform CIG run 1, frame 208 takes 188.006 ms
with 2.88675 ms CUDA work and 174.311 ms handoff. Shared-context creation alone
is insufficient. These intervals do not identify a particular OS/driver cause.
Next isolate CPU time in the individual DX12/CUDA fence/event APIs and correlate
queue scheduling; the existing callback timer excludes those calls. Do not
remove required fences or lower physical accuracy to hide this interval.

### Tooling qualification remains open

The additional CIG interop memcheck attempt does **not** pass. The installed
sanitizer reports failure to initialize the WDDM debugger interface, then
"Device not supported"; the target also fails its strict partial-import
handle-count check under that run. It exits 91 with two reported errors.
Logs are `cuda-cig-interop-memcheck.log` and `.stderr.log`; no passing sanitizer
manifest is generated. The plain nine-case test above is separate evidence,
not a substitute for instrumentation. No Windows debugger registry settings,
driver settings or TDR limits were changed.

D3D12 debug-layer/GPU validation remains unavailable (0x887A002D), and the
previous full conditional-pressure sanitizer gap is also still open. The
bounded topology-work, conservative particle/bulk ownership, adaptive transfers
and variable-scale surface-reconstruction requirements remain unfinished.

### Local instrumentation note

Some historical sanitizer runs used a locally provisioned administrator terminal.
Machine-specific elevation helpers are intentionally excluded from the public
repository. Normal builds and demo launches require no elevation.

## CPU submission / UI upload latency checkpoint — 14 September 2026

### Diagnosis and integration

The former `cudaHandoff = DX12 ownership span - CUDA event interval` includes
more than context/fence scheduling. Its final DX12 timestamp resides in the
resumed command list, which is submitted only **after** recording the surface,
photon/camera passes, RR and UI. Slow CPU recording can therefore inflate this
GPU interval even when CUDA and the external-fence API calls are short.

`gpu_submission_profile.h` adds a bounded CPU wall-clock timeline, routed through
`Renderer` → `FluidSystem` → `FluidCuda` → `gpu::CudaInterop` and back through
RR/UI recording, submission, presentation, the existing completion fence and
collection. It adds no GPU query, fence, readback, synchronization or kernel.
Ordinary play supplies a null pointer; clock sampling/record storage is enabled
only by `--profile-latency`. The existing 23 timing columns remain unchanged.
`latency.cpuSubmission` has its own versioned schema with 31 stage offsets plus
frame identity. Unexecuted stages are JSON `null`, including CUDA stages on DX12
and event registration when the frame fence is already complete.

`profile-cuda.ps1` manifest version 5 includes CPU interval summaries and worst
frames. `cpu-submission-profile.ps1` checks them after each process exits;
`validate-cuda-profile.mjs --require-cpu <capture.json>` independently checks the
schema, ordering, backend holes and CPU/GPU frame identity. Legacy captures can
still be analyzed without inventing CPU observations. Windows PowerShell parses
fractional JSON values as `Decimal` here; the initial validator rejected a valid
first capture for that reason. It was corrected before the comparison below.
The aborted `cpu-submit-primary-v1` capture is not part of that comparison.

The worst uniform baseline frame took 179.078 ms: UI recording 161.5322 ms,
CUDA work 2.85174 ms, apparent handoff 159.202 ms. The worst mixed frame took
162.153 ms: UI recording 150.811 ms, CUDA work 6.43706 ms. The UI backend created
two committed upload buffers whenever RmlUi rebuilt geometry (including changing
FPS/status text), then destroyed retired geometry at the next `begin()`.

The shared native `UiRenderer` now recycles power-of-two geometry upload buffers
through the existing frame-fence retirement boundary. Released geometry cannot
be reused in the same frame, even after its draw has been recorded. The idle
cache is capped at **256 buffers / 8 MiB**; excess entries are released only at
the existing safe boundary. Texture/descriptor retirement and UI rendering are
unchanged. No fluid, pressure, timestep, caustic, RR, resolution or FG setting
was reduced. Reports include committed-allocation/reuse counts and cached bytes.

### Matched raw-frame measurements

Three runs per mode before and after, using the primary CUDA context, active
room inlet, 1920×1080 Balanced, FG off, 300 real frames / 600 substeps / five
simulated seconds per run. The first 32 frames are retained separately; each
table row pools 804 steady frames. Uniform/mixed order alternates within each
set. These are **before/after binaries**, not a same-build backend comparison.
Shaders and profiling helpers are identical. No compiler, other GPU test,
sanitizer or capture analysis overlapped these raw captures.

| Pressure | UI buffers | Median | p99 | Worst | Over 16.667 ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Uniform Jacobi | Allocate/retire | 13.5853 ms | 40.2217 ms | 179.078 ms | 56 / 804 |
| Uniform Jacobi | Reuse | 13.6213 ms | 18.8858 ms | 28.5494 ms | 47 / 804 |
| Mixed MGPCG | Allocate/retire | 16.5204 ms | 61.8428 ms | 162.153 ms | 338 / 804 |
| Mixed MGPCG | Reuse | 16.4417 ms | 23.6018 ms | 32.4664 ms | 307 / 804 |

UI CPU p99 falls from 29.571 / 52.2674 ms to 0.1888 / 0.1988 ms for uniform /
mixed. Worst UI recording after the change is 0.2617 / 0.2867 ms. The typical
300-frame run creates 40 geometry buffers and reuses roughly 280–300, with a
2-KiB idle-cache high water in the sampled final runs. This removes the dominant
reproduced allocation/retirement tail, not all possible stutter. Median frame
time barely changes; **the p99 60-Hz gate still fails**.

Remaining outliers have distinct signatures: a uniform 28.50-ms frame spends
16.12 ms in CUDA CPU enqueue; another 28.55-ms frame has short CPU recording but
15.09 ms of apparent handoff. A mixed 32.41-ms frame has an 18.36-ms CUDA event
interval; another 32.47-ms frame has 14.77 ms of apparent handoff. These are
observations, not proof of a specific OS/driver cause. Required fences remain.

Evidence: `cuda-perf-cpu-submit-{before,after}-manifest.json` and their twelve
`*-play-{1,2,3}-cuda-{graphs,mixed}.json` captures. Report/binary/shader hashes,
physical time, CPU/GPU alignment and PowerShell-versus-Node interval summaries
were independently checked for all twelve. Before binary SHA256:
`9DBF979DEDFF8D4A6CCD0525FD68FC3DEC52D96FDFB2FF21658EE00795795F1B`
(`NVMatrixFluidLabCpuBefore.exe` retained in the CUDA runtime). After binary:
`3AA2FE78DAB5019A1D423D49439EB59C0BA759AF9966046372C856D706ED709B`.

### Regression evidence and remaining scope

- CUDA-enabled and CUDA-OFF lab Release builds pass, as does the native mainline
  build sharing `UiRenderer`. Nine normal lab Windows CTests, the native physics/
  optics CTest, and 244 Node tests pass. The new portable timeline test also
  passes GCC with warnings as errors. Existing dependency/WSL path warnings and
  mainline video signedness warnings were not changed.
- Primary and CIG interop suites pass all 8 / 9 cases, including failure cleanup
  and context restoration. No sanitizer/admin tool was launched.
- `NVMatrixEngineUiUploadTest` renders 64 alternating-color frames and checks all
  16,384 pixels. It deliberately releases the first draw's geometry before
  compiling the second draw, proving same-frame storage is not recycled early.
  Only four geometry buffers are allocated throughout the steady draw sequence,
  with 252 reuses. Additional oversized/many-buffer releases exercise both cache
  caps (608 total fixture allocations; final/high-water idle bytes 65,536).
  Evidence: `ui-upload-cache-validation.json` and its hashed log. Build the lab
  and `NVMatrixEngineUiUploadTest`, then run `NVMatrixEngineUiUploadTest.exe "<runtime>"`
  from the lab runtime directory. This fixture requires the lab's Agility files.
- Rendered mixed-pressure room controls pass in primary and CIG contexts
  (180 frames each), including pause/reset/inlet and moving-collider checks.
  The primary 90-frame FG fixture also passes: 178 presented frames, 88 generated
  extras, 180 physical substeps. Stored-flux divergence remains below 1e-4/s;
  all three runs have zero bad/truncated fluid roots. The captured primary HUD
  was visually inspected and has no visible glyph/geometry corruption.
- An additional CUDA-OFF DX12 raw capture validates the null CUDA-stage schema
  and the shared cache (`cuda-perf-cpu-submit-dx12-play-1-dx12.json`): 300 frames,
  600 substeps, UI p99 0.1992 ms. It is not a repeated backend speedup comparison.
- The CPU validator rejects 12 malformed real-capture variants in PowerShell;
  Node tests additionally cover missing/reordered stages, legacy evidence,
  backend holes, cold-start separation and CPU/GPU causality separation.

D3D12 debug/GPU validation is still unavailable (0x887A002D); the new GPU fixture
reports that explicitly. Conditional-pressure sanitizer qualification, bounded
hierarchy/topology work, conservative particle/bulk ownership, adaptive transfers
and variable-scale surface/optical acceptance remain open. No CUDA/CIG/pressure
default was promoted. Continue these requirements; the UI fix is a latency
prerequisite, not completion of the multiscale contract.

## Advected pressure-refinement checkpoint — 14 September 2026

### Implemented policy and publication boundary

`cuda/fluid_cuda_mac.h/.cu` replaces the fixed-cell, eight-substep classification
history with a dimensionless error policy on the existing two-level MAC lattice:

- Measure local velocity range and trilinearly backtrace the previous velocity,
  occupancy, filtered score and wake lifetime through the current velocity.
- Compare the current forced velocity against the **previous projected** velocity
  plus known gravity. Storing pre-projection history would count gravity twice
  and prevent a calm hydrostatic pool from coarsening. Empty air does not receive
  a fictitious gravity prediction or generate liquid-velocity error. Occupancy
  changes still identify newly wet/vacated regions.
- Keep the existing fully wet 6-cubed fine-cell surface/solid guard and swept
  moving-solid margin. Pad fresh disturbances by one coarse-cell layer, without
  recursively growing already-padded decisions each step. Advect and decay wakes.
- Use exponential score smoothing, immediate high-error promotion, distinct
  promote/demote thresholds and a minimum **physical quiet time**. The timer uses
  FP64 accumulation: FP32 summation had made a 40-ms threshold take an extra step
  when split into 5-ms intervals. No threshold tolerance was loosened.
- Separate immutable measured fields from classification writes. Two persistent
  56-byte-per-cell history generations avoid neighbor read/write races. Publish
  history only after projection and the existing final stored-flux validation.
  Rejected solves leave published history unchanged. Capacity-deferred fine
  solves do not publish a merely requested coarse-retention bit. Reset clears
  both generations, including paused rebuilds and graph replay.

`MacConfig::refinement` exposes the policy to engine code. Defaults: velocity
scale 0.5 m/s, velocity-range scale 0.08 m/s, normalized velocity error 0.08,
occupancy error 0.1, response 25 ms, wake 150 ms, quiet time 8/120 s, and
demote/promote scores 0.5/1.0. These are heuristics, not a measured fine-reference
error bound; periodic fine probes and generalized predictive support remain work.

The occupancy signal is the unit-particle CUDA baseline's normalized cell count,
clamped to [0,1]. It is **not** an authoritative conservative liquid-volume field
for mixed particle/grid ownership. At this checkpoint CUDA rejected ownership
modes; the later ownership checkpoint adds weighted particle-owned occupancy,
not mixed particle/grid support.
Particle radii/counts, P2G/G2P spacing, pressure acceptance limits, canonical
surface sampling, DXR, photons and RR are unchanged by this policy. Uniform CUDA
does not allocate/run the MAC policy; forced-fine skips the expensive measurement.

The existing single deferred completion record grows to 88 bytes. Reports add
`refinementPolicy="advected-error-v1"` in mixed mode and three classification
cell-step totals: wake, temporal-error and padding triggers. These count policy
work, not accepted coarse cells or simulated mass. Their GPU uint32 counters are
unwrapped into uint64 totals at the mandatory submission collection boundary;
bounded grid size/substeps prevent an entire wrap between collections. No new
readback, CPU topology decision, per-particle work or iteration synchronization.
`cuda-test-mode.ps1` checks policy selection and inactive-mode diagnostics.

### Correctness and rendered evidence

The CUDA Release build passes. `test-cuda.ps1 -NoBuild` records **90 passing
cases**: 8 interop, 8 transfer, 19 core, 5 brick-pool, 35 MAC and 15 composed-solver.
Ten new MAC cases cover empty air, hydrostatic coarsening, co-moving gravity,
velocity/occupancy changes, nonrecursive padding, direct/captured wake advection,
constant/variable timestep dwell and invalid configuration. A new composed test
injects counter values across rollover and checks repeat collection is idempotent.
The rejection fixture also checks unchanged published refinement history.

The manufactured 100-g and affine-pressure operator tests explicitly enlarge
their **test-only refinement scales** to retain T-junction coverage; these inputs
are repeatedly installed pressure RHS fields, not physical trajectories. Their
independent matrix, Galerkin, face-gradient and divergence tolerances are unchanged.
The normal-gravity refinement test uses production policy settings. The composed
101,376-particle FLIP fixture exercises 900 peak coarse cells and converges within
15 pressure iterations. This is numerical evidence, not a deep-pool FPS result.

Fresh final-build rendered tests pass:

| Fixture | Frames / substeps | Peak coarse cells | Final independently audited max divergence |
| --- | ---: | ---: | ---: |
| Pool | 240 / 480 | 418 | 2.16067e-5 /s |
| Moving colliders | 180 / 360 | 65 | 2.81143e-5 /s |
| Room controls, primary | 180 / 160 | 0 | 3.28175e-5 /s |
| Room FG | 90 / 180 | 0 | 1.55065e-5 /s |
| Room controls, CIG | 180 / 160 | 0 | 2.99275e-5 /s |

All five report zero bad/truncated fluid roots; CPU/GPU final-face audit mismatch
is at most 7.33528e-13 /s. FG still presents 178 frames with 88 generated extras.
The pool and room-control captures were visually inspected for obvious surface/UI
breakage. These snapshots do not prove temporal or variable-scale optical-seam
acceptance. Source/binary/report hashes were independently checked in
`cuda-validation.json` and the final `cuda-engine-{surface,room}-mixed-*` manifests;
their filters identify the exact five rendered cases above. The existing nine
normal Windows CTests and all 244 engine/native Node tests also pass.

### Same-build raw timing, not a speedup claim

`profile-cuda.ps1 -Comparison pressure -CaseFilter '^play$' -Repeats 3
-Tag advected-refinement-v1 -Context primary` records nine sequential runs:
1920x1080 Balanced, active room inlet, FG/probes off, 300 real frames and
600 substeps (five simulated seconds) each. No build, other GPU fixture or
capture analysis overlaps them. The first 32 frames are retained separately;
each steady row below pools 804 frames. Order reverses on the second repetition.

| Pressure mode | Whole median | Whole p99 | Worst | Over 16.667 ms | Fluid median |
| --- | ---: | ---: | ---: | ---: | ---: |
| Uniform Jacobi | 13.5169 ms | 19.3612 ms | 31.6390 ms | 32 / 804 | 3.02256 ms |
| Forced-fine MGPCG | 16.4841 ms | 23.4919 ms | 29.7939 ms | 314 / 804 | 6.83392 ms |
| Adaptive mixed MGPCG | 16.7260 ms | 23.5657 ms | 36.0805 ms | 424 / 804 | 7.00080 ms |

**No room run creates a coarse pressure cell.** Its shallow liquid does not
provide a coarsenable region under the safety guards. Mixed adds about 0.17 ms
median fluid time over forced-fine in this set, without saving pressure work.
The old uniform Jacobi mode remains a cost reference with a different convergence
contract, not proof of equal numerical error. None meets the p99 60-Hz gate.
First-frame durations span 229.874–323.243 ms across these runs, including graph/
render warm-up; no cold outliers were dropped. Sampled process-local VRAM peaks
are about 1.49 GB uniform and 1.59 GB MGPCG, not exact subframe allocation peaks.

Evidence: `cuda-perf-advected-refinement-v1-manifest.json` and its nine raw JSON
captures. The independent Node validator checks all timing/CPU schemas and
steady quantiles; report, binary, shader and helper hashes match. Final binary
SHA256 `081B6F9D2C3E5097A83F11C5907CE9EE6C5AED9727F67C69C3AF019B55C2DA0B`
is also retained as `NVMatrixFluidLabRefinementV1.exe` in the CUDA runtime. These are
same-build mode comparisons, **not** a matched old/new classifier benchmark.

### Remaining scope

This advances predictive pressure refinement, not the complete CUDA/multiscale
goal. Conservative particle/bulk ownership, adaptive APIC transfers, bounded
hierarchy/topology updates, periodic fine-error probes, variable-scale surface
and optical acceptance, and deadline-tail optimization remain required. Full
conditional-loop sanitizer qualification and D3D12 GPU validation remain open;
the latter still reports unavailable (0x887A002D). No elevated tool was launched.
No normal backend, pressure mode, CIG setting or quality default was promoted.

## Shared particle-ownership checkpoint — 14 September 2026

### Integration and publication

The CUDA solver now connects to `FluidParticleGridExchange`'s existing authority
rather than maintaining a parallel mass inventory. Enable it explicitly:

```text
NVMatrixFluidLab.exe --fluid-room --fluid-backend=cuda --fluid-cuda-graphs=on --fluid-owned-particles
```

The option also composes with `--fluid-cuda-pressure=mixed` and
`--fluid-cuda-context=cig`. Normal launchers, backend selection and pressure/quality
defaults are unchanged. Unsupported legacy particle-resampling, dormant-interior
and bulk-replica combinations remain rejected.

- `fluid_system.cpp` runs the existing exchange reset/seed in the DX12 prefix,
  after initialization/inlet emission. Only actual birth IDs get initialized;
  previous-position birth history and pause/reset semantics remain shared.
- `fluid_particle_grid_exchange.*` optionally exposes shared quantity, reference
  velocity and control buffers. `fluid_cuda.*` imports these plus the cell-mass
  cache through the existing interop boundary: **18 buffers when owned, 14 when
  unowned**. Only the initialized 17-word control prefix is imported/copied.
  Dormant grid inventories are not imported or counted as liquid.
- `cuda/fluid_cuda_ownership.*` validates the live ledger and applies every
  substep's cached velocity increment in FP64, retaining exact rest volume and
  sub-FP32 momentum. The ledger stores volume-weighted velocity, not kilograms;
  density supplies the conversion to physical mass/momentum. APIC affine rows
  remain FP32 caches, as in the existing DX12 contract.
- CUDA binning gathers FP64 particle volume per cell, rounding once to FP32 mass
  units without 1/16 quantization. P2G reads volume and linear velocity directly
  from the ledger. Capillarity and mixed-pressure occupancy consume the mass
  cache rather than confusing particle population with liquid volume. Density
  repair/reconstruction still read validated rounded particle weights.
- The existing CUDA transaction extends to these four optional views. Uniform
  owned and mixed owned modes stage and publish simulation fields and the ledger
  together under one GPU failure latch. Invalid input or a later failed pressure
  substep cannot publish just one half. Default uniform unowned mode still avoids
  this staging. Scratch is persistent; there is no per-particle CPU work or new
  per-iteration synchronization. Completion reuses the 88-byte deferred record.

### Numerical and live evidence

The CUDA Release build and CUDA-disabled Release build pass. The new source has
no remaining compiler warning; existing Bullet/RmlUi dependency warnings remain.
`test-cuda.ps1 -NoBuild` records **101 passing cases**: 8 interop, 8 transfer,
19 core, 5 brick-pool, 35 MAC and 26 composed-solver cases. Eleven new solver cases
cover owned direct/graph substeps, mixed pressure and moving solids, fractional
P2G plus composed uniform/mixed steps, preserved sub-FP32 momentum, malformed
input, aliased/missing bindings, late-pressure-failure rollback and actual
coarse/fine owned FLIP at 100k particles.

The independent fractional face gather differs by at most 3.32279e-6 in its
tested velocity/weight components. Deliberately corrupted FP32 mass/velocity
caches cannot change that P2G result. With valid caches restored, six real
substeps preserve each non-dyadic FP64 rest volume exactly; normalized momentum
increment error is at most 4.26373e-19. Four unit-weight ownership/reference
fixtures have **zero** difference in all 20 particle components. Rejection tests
compare all 18 shared fields against their pre-submission bytes, including a
failure after the first accepted pressure substep.

The larger owned FLIP fixture runs 101,376 active particles over 12 substeps,
reaches 900 peak coarse pressure cells, and converges within 15 pressure iterations.
It checks exact particle rest volumes, current reference velocities and ledger
momentum agreement after actual coarse/fine coupling. The room matrix below does
not exercise that topology: its shallow water stays entirely fine.

The extended `test-fluid-owned-particles.ps1` retains source, binary, shader and
report hashes and passes these engine invocations:

| Selection | Cases |
| --- | --- |
| CUDA primary graphs, owned | empty, short, affine, FLIP, inlet, mixed, reset |
| Same CUDA modes, unowned reference | the same seven cases |
| CUDA CIG graphs, owned mixed pressure | short, inlet, reset |
| CUDA primary direct, owned | short, mixed, reset |
| CUDA-disabled DX12, owned | the original seven cases, including DX12 mixed/sparse checks |

The three one-frame affine cases are partial transfer fixtures, not full optical
coverage. Other cases execute actual surface/DXR/photon/RR paths and report zero
bad/truncated fluid roots. The inlet adds 1,258 particles in 64 frames; the reset
sequence ends with 101,376 particles after 180 rendered frames / 160 post-reset
substeps. All owned captures report zero relative rest-volume/cache-mass error;
the largest CUDA velocity-cache error in this matrix is 4.38798e-18.

Independent comparison of short, FLIP, inlet, mixed and reset pairs finds every
captured raw-lighting/guide/caustic channel identical. Their final PPMs are also
identical outside the top-right FPS counter (the mixed pair is entirely identical).
This establishes unchanged unit-weight output for these sequences, **not**
variable-scale optical acceptance. The inlet image was visually inspected.

An additional 90-frame 1280x720 Balanced mixed-owned CUDA run enables the real
inlet, foam/bubbles and 2x frame generation. It passes ownership, medium/root,
whitewater and finite-guide/caustic-energy audits: 180 physical substeps,
178 presented frames and 88 generated extras. This is feature-composition
evidence, not a raw-frame performance result.

All 244 engine/native Node tests, nine normal Windows CTests and the existing
seven exchange / eleven flowing-band GPU cases also pass. The latter continue
to exercise actual DX12 retire/transport/restore and fractional P2G independently;
they do not prove those operations are wired into the live CUDA solver.

Evidence lives beside the CUDA/DX12 build runtimes: `cuda-validation.json`,
`cuda-*-{owned,reference}-validation.json`,
`dx12-uniform-primary-graphs-False-owned-validation.json`, and
`cuda-owned-room-frame-gen{,-validation}.json`. The paired raw buffers are the
matching `.inputs` captures. Source/binary/shader/report hashes were independently
checked for the primary, direct and CIG manifests; FG report/capture hashes and
raw input validity were checked separately. CLI/report validation passes all
12 unsupported-option cases and 19 malformed-report cases.
Lab executable SHA256:
`80DF46E1E75C19EF3AD5A332D41A449BD4AF372B1B6C029CB85F0999614A9084`.

### Still required

This is the canonical ownership-consumer connection, not a new completed
narrow-band solver. Next, couple **actual grid-owned** mass and momentum to
joint P2G/prediction, pressure support and conservative interface transport.
Only then may the flowing-band policy retire live particles. Canonical density,
capillarity and surface reconstruction must include those owners, with conserved
mass/momentum, bounded topology work and cross-LOD optical tests. Variable-scale
APIC/surface work, predictive fine probes, deadline-tail improvement and the full
contract's remaining acceptance gates remain open.

No speedup is claimed for the additional ownership work. No sanitizer was run;
D3D12 debug/GPU validation still reports unavailable (0x887A002D). The one
user-authorized persistent-admin launch was canceled by Windows; no replacement
elevation request, service, autostart or UAC-policy change was made.

## CUDA grid-owned transport checkpoint — 14 September 2026

### Implemented connection

`cuda/fluid_cuda_owned_transport.*` consumes the same FP64 volume and
volume-weighted momentum stored by `FluidParticleGridExchange`. It solves a
backward-Euler donor transport system with endpoint/start open capacities and
one signed volumetric rate per MAC face. Output ownership and signed face
transfers are written only after convergence and capacity/finite-value checks.
Invalid data, trapped closing cells, iteration caps and receiver overfill leave
the source and destination owners and face-transfer buffer unchanged. Failure
uses a caller-owned sticky GPU latch.

Scratch is preallocated (72 bytes per cell plus a 48-byte control record). Work
is capturable and GPU resident, with at most 256 paired Jacobi iterations and
no per-iteration CPU synchronization. Converged kernels early-return; the graph
still contains its bounded unrolled schedule. This is not a latency-qualified
conditional-loop optimization or a complete fluid solver.

The accompanying restriction integrates actual projected fine MAC velocities
over the exchange's 2h faces, including odd domain dimensions and partial edge
areas. A separate input check covers **all** real fine faces, including faces
eliminated by restriction. Nonfinite internal velocities and nonzero velocities
on any closed domain boundary reject the operation. Full-face area restriction
alone does not supply cut-cell apertures, exact geometric conservation for
moving solids, or joint particle/grid capacity-compatible phase fluxes.

`FluidParticleGridExchange::CudaSharing` independently shares particle and grid
inventories. Its existing bool constructor retains the previous particle-only
behavior; the playable CUDA backend still imports its original 14/18 views.
The new exchange fixture imports six real DX12 resources through production
`gpu::CudaInterop`: both grid inventories, capacities, face rates, face transfers
and the existing control prefix. Actual DX12 retirement precedes CUDA transport;
DX12 restoration, motion-history initialization and fractional production P2G
follow it. No CPU quantity staging or parallel ownership inventory is introduced.

### Reproduced numerical failures and corrections

The initial residual scale could declare a dilute, nonzero owner solved at zero
because it used geometric capacity as an absolute floor. A regression reproduced
loss of a 1e-30 volume and still smaller momentum components. The CUDA stopping
test now uses component-wise backward error relative to actual equation terms,
without that floor. Zero-capacity cells also cannot admit epsilon-sized owners.
The tiny-quantity fixture now matches its independent solution with zero measured
relative error. This correction is in the new CUDA transport; it does not certify
the older HLSL transport over arbitrarily tiny quantities.

A second regression found that restriction could overlook a NaN on an odd-indexed
fine face that it did not sample. The full fine-face validation fixes this and
checks all six domain boundaries plus retained/eliminated interior faces.

### Validation and limits

CUDA-enabled and CUDA-disabled Windows Release builds pass. The new numerical
source compiles without warnings; existing Bullet/RmlUi header warnings remain.
`test-cuda.ps1 -NoBuild` records **120 passing cases**: the previous 101 plus
12 transport cases and seven shared exchange cases. Its version-4 manifest
explicitly distinguishes these fixtures from live joint particle/grid flow.

The independent transport oracle uses face-column matrix assembly and pivoted
dense elimination rather than the GPU cell-gather Jacobi method. Tests audit
per-cell balance, total quantities, face transfers, translational kinetic energy,
source immutability, rejected destination bytes and buffer guards. The CFL-4
captured circulation converges in 124 iterations, with maximum measured equation
error 1.06071e-12. Closing/reverse-closing chains converge in four iterations.
The real projected-MAC odd-grid test has maximum equation error 3.1225e-16 and
nonzero restricted flow. The seven shared exchange cases have maximum joint
quantity error 2.77556e-17; restored fractional P2G remains independently checked.

Through the already-approved persistent host:

```powershell
& ./engine/test-cuda-sanitizer.ps1 -NoBuild -Fixtures OwnedTransport,Exchange
```

Both fixtures pass memcheck, initcheck and synccheck: **six checks, no kernel
exclusions, zero reported errors and successful target exits**. The pressure-rate
fixture uses unrolled MGPCG, not the unresolved conditional-loop instrumentation
path. Evidence is `cuda-sanitizer-OwnedTransport-Exchange-validation.json`;
`partialCoverage=true` correctly refers to the untested remainder of the engine.
The runner requires an already elevated token and preserves fixture-specific
manifests. Source, shader, executable and log hashes were independently checked.

A subsequent full MAC/Solver instrumentation attempt still fails in MAC memcheck
with exit -1073741819 (0xC0000005), despite elevation. It produces no completed
case output, and its zero-error-summary text is **not a pass**. The runner rejects
it and does not proceed to Solver. `cuda-loops-Mac-memcheck.*` retain the failure;
all 35 normal MAC cases pass again afterward. Full pressure/solver sanitizer
qualification therefore remains open. D3D12 debug/GBV is still unavailable
(0x887A002D); CUDA sanitizer success does not replace it.

Fresh live regressions pass CUDA graph inlet/mixed/reset (308 rendered frames)
and CUDA-disabled DX12 short/inlet (72 frames), with unchanged exact owned volume
and no reported bad/truncated optical roots. All 244 Node tests, nine native
CTests and the original seven DX12 exchange / eleven flowing-band GPU cases
pass. Current subset manifests retain their actual case filters, rather than
claiming a repeat of the previous complete live matrix. Their hashes and the
120-case CUDA manifest's 97 sources, 358 shaders and eight binaries were audited.
CUDA lab SHA256:
`2CFFD68E7D7599A45FC031E58579FE95A6053CED3639823EA9C72046B21FF758`.

**Next integration:** jointly predict particle/grid momentum, provide pressure
support for actual grid owners, and couple capacity-compatible phase transport
to particle advection. Then include those owners in canonical density,
capillarity and surface reconstruction before enabling live automatic retirement.
Mean-only grid transport does not yet preserve arbitrary affine/angular detail
or sharply advect a reconstructed free surface. Multiscale APIC, bounded topology
updates, optical seam acceptance and the full latency gates remain required.
No gameplay default or quality setting changed; no new speedup is claimed.

## Joint CUDA ownership prediction checkpoint — 14 September 2026

### Transfer and pressure connection

`createTransfer()` now optionally accepts `GridOwnership`: the existing 2h
FP64 grid quantities, per-fine-cell open volumes, and a shared sticky failure
latch. It reuses the current particle binning, P2G/G2P and MAC field layout.
Ordinary solver construction still supplies no grid view; no new gameplay mode
or default was enabled.

- During binning, a GPU kernel distributes each coarse mean quantity among its
  fine children in proportion to their open volumes. The resulting 32-byte
  fine-cell values are private transfer caches, **not additional liquid owners**.
  Invalid capacity, overfilled/zero-capacity owners, nonfinite quantities and
  unrepresentable cached velocities reject before face prediction.
- P2G combines particle momentum with volume-integrated quadratic B-spline
  contributions from the grid owners. It integrates the uniform fine-cell
  contribution rather than inventing particles at cell centers. Odd coarse
  extents and fractional fine open volumes use the same physical normalization.
- The cell-mass cache contains both ownership sources. `fluid_cuda_grid.cu`
  classifies liquid from this mass when supplied, instead of particle count.
  Thus cells with **zero particles but nonzero grid-owned water** participate
  in the existing globally coupled pressure solve. Counts/offsets remain real
  particle data for binning and particle-only consumers.
- `enqueueGridOwnershipReturn()` uses the same integrated grid basis and the
  particle path's PIC/FLIP blend to return momentum after forces/projection.
  It retains FP64 rest volume exactly and writes a distinct staged destination;
  invalid returned faces or quantities cannot publish that destination.
  This return is a velocity update, not spatial advection.

Both grid transfer directions normalize their clipped domain-boundary support.
Without this, missing basis functions would damp a constant velocity even before
applying physical boundary forces. The all-boundary constant-flow test now retains
quantities to 9.97466e-18 maximum error. This does not replace solid apertures or
moving-wall conditions with interpolation rules.

Extra scratch is preallocated: 32 bytes per fine cell plus 32 bytes per coarse
return cell (26,016 bytes in the small fixture). The all-particle and joint P2G
kernels are separate compile-time specializations. Final SM120 binary inspection
reports 64 registers / zero stack for the particle-only variant, versus 72
registers / 64-byte stack for the joint variant. Normal calls therefore do not
execute the optional volume-integration loop or use its kernel allocation.
This is code-generation evidence, **not a measured frame-rate improvement**.

The coupling tests follow the conservation concern highlighted by
[Narrow Band FLIP](https://visualcomputing.ist.ac.at/publications/2016/NarrowBandFLIP/):
naively mixing particle/grid velocities can introduce energy fluctuations. These
integrated finite-volume ownership transfers are original engine code, not a
claim to reproduce that paper's complete narrow-band algorithm.

### Numerical and composition evidence

`NVMatrixEngineCudaJointTransferTest` adds 12 cases: empty-grid baseline parity,
grid-only prediction/pressure classification, joint affine prediction, odd and
boundary capacities, PIC return, FLIP return, projected return, invalid
publication, captured replay, invalid API, constant boundary flow and a four-step
transport sequence. These are small 11×9×7 fixtures, not 100k joint-flow scaling
or the real-room adaptive acceptance case.

The independent CPU oracle integrates the quadratic pieces with Simpson's rule,
and sums actual valid face support rather than copying the GPU's weight table.
It gathers directly from all source particles and grid volumes, independently
of GPU bins. Maximum predicted velocity error is 9.39121e-9; grid momentum-return
error is at most 2.27682e-18. Tests check combined cell mass, constant states,
exact retained grid volume, face-impulse conservation and non-increasing
translational energy for unforced PIC/FLIP transfer. They do not certify arbitrary
APIC angular/affine conservation in the mean-only grid representation.

The composed sequence performs real binning → joint prediction → existing
MGPCG projection → particle/grid momentum return → particle advection and
canonical-face restriction → conservative grid transport. Both representations
move over four 0.005-second steps. Maximum total-volume error is 8.32667e-17 m³;
maximum grid-transport quantity error is 1.38778e-16; maximum particle travel
is 0.00147063 m. The fixture audits pressure before continuing. It does not yet
establish the live Solver's all-GPU, whole-frame rollback contract for these new
views, nor an upper bound on **combined** particle/grid phase occupancy.

### Final-source validation

CUDA-enabled and CUDA-disabled Windows Release builds pass. All **132 CUDA
cases**, 244 Node tests, nine native CTests and the original seven DX12 exchange /
eleven flowing-band GPU cases pass. The version-5 `cuda-validation.json` records
the joint fixture/sequence separately from live joint flow. Its 98 source hashes,
358 shader hashes and nine test binaries were independently verified.

Using the existing elevated host:

```powershell
& ./engine/test-cuda-sanitizer.ps1 -NoBuild -Fixtures JointTransfer,OwnedTransport,Exchange
```

All three fixtures pass memcheck, initcheck and synccheck: **nine checks with no
kernel exclusions**. The script verifies successful target exits, complete case
matrices and zero-error diagnostics before writing
`cuda-sanitizer-JointTransfer-OwnedTransport-Exchange-validation.json`. Its source,
shader, executable and log hashes were independently verified. The console client
reported a broken pipe after completion; the completed manifest and all checks
were recovered without rerunning them, and the same elevated host answered its
subsequent ping. No new elevation was requested. Full conditional-pressure/whole-
solver instrumentation remains unqualified; this focused manifest correctly
retains `partialCoverage=true`. D3D12 debug/GBV remains unavailable (0x887A002D).

Fresh live tests cover CUDA-primary graphs with inlet/mixed/reset in both owned
and unowned-reference modes (six runs, 616 frames), plus CUDA-disabled DX12
short/inlet (72 frames). Owned volumes retain zero reported error. Each paired
CUDA capture has **zero changed scalars across all ten raw lighting/guide/caustic
channels**, and passes finite-value/optical-energy checks. These are final-capture
comparisons for the existing all-particle path, not temporal or optical acceptance
for a grid-owned rendering surface. The three live manifests retain their actual
subset filters and were independently hash-checked.
CUDA lab SHA256:
`F30735B91462BFE7E90229D6E9F693FC62A27864BEBF186AC385EA1956F3A223`.

### Next required connections

Extend the existing Solver transaction/lifecycle to these actual grid views and
connect its pressure failure latch before joint publication. Couple particle
advection and grid phase fluxes against **shared** available capacity, including
moving geometry and restoration/allocation limits. The present per-child open-
volume model homogenizes porosity; it is not a geometrically resolved cut-cell
transport or a sharp free-surface update. Dilute transported tails cannot be
treated as full negative liquid boxes by a renderer.

Then connect canonical density/capillarity/surface reconstruction to grid-owned
water and qualify optical transitions before enabling the existing retirement
policy. Variable-scale APIC, affine/angular ownership, bounded topology work,
predictive error probes, sparse surface seams and the original numerical,
optical and raw-latency gates remain required. No new speedup or completed
realtime-adaptive system is claimed.

## Transactional joint Solver checkpoint — 14 September 2026

### Implemented connection

`Solver::create` now accepts an optional `GridInventory` containing the actual
FP64 coarse grid quantities and FP64 fine-cell open volumes. The existing
transaction snapshots these alongside the particle ledger: 20 views for joint
ownership, while ordinary solvers retain their existing 14/18-view contracts.
`FluidCuda` does not yet pass these optional views in gameplay.

`fluid_cuda_joint.*` owns persistent momentum-return, geometric-capacity,
restricted-rate and conservative-face-quantity scratch. The existing transfer
module performs joint P2G and matched grid return; the existing owned-transport
module then advances that returned grid inventory. Particle G2P/advection and
grid transport use the same projected MAC faces. Both share the Solver's sticky
failure latch, including the actual MGPCG failure state. Final binning refreshes
the joint mass cache before the next substep. No per-substep CPU readback,
device allocation or separate simulation framework was added.

A GPU audit sums actual particle rest volumes and grid rest volume against each
coarse owner's geometric capacity, both initially and after transport. Invalid
capacity, malformed quantities or combined overfill reject the entire trial
frame; they do not clamp or discard liquid. **This is an acceptance guard, not
a coupled capacity-compatible interface flux solver.** The transport equation
still uses geometric volume, not an incorrectly substituted capacity remaining
after particles. Fine-cell porosity remains homogenized within its coarse owner.

All publication occurs through the original transaction. A failed later substep
leaves every shared view unchanged, even if earlier substeps already advanced
both private inventories. Completion reports the failure at the existing fence
and poisons the Solver until rebuilt. Paused submissions also validate freshly
handed-off grid owners. External seeding/reset is observed through the next
snapshot; resetting metadata alone does not invent or erase shared inventory.

The existing four bounded graph variants include the joint work. Physical
parameter changes replace those variants; normal steps, seeding and reset do
not allocate replacement workspaces. The odd 11×9×7 fixture uses 895,588 bytes
of whole-transaction staging and 65,664 bytes of joint transfer/transport
workspace. `gridTransportBytes` is exposed in solver statistics and runtime
diagnostics; ordinary gameplay reports zero for this optional workspace.

### Validation

`NVMatrixEngineCudaSolverTest` now has **35 passing cases**, including nine new joint
cases:

- Uniform-Jacobi and forced-fine MGPCG direct/graph lifecycle comparisons:
  all 20 shared views are bit-identical, including guards. Twelve substeps plus
  paused rebuild/no-step frames preserve total volume to 1.55431e-15 and
  1.99840e-15 m³ absolute error respectively. PIC→FLIP/gravity changes produce
  the expected eight total graph builds; both inventories actually move.
- Five malformed-input variants in each submission mode, including paused
  invalid input and grid-only-admissible but jointly overfilled capacity:
  no shared field or guard changes on rejection.
- Missing/aliased views and unsupported solver combinations are rejected.
- A completed reference sequence identifies a genuinely harder second pressure
  step. A ten-iteration cap accepts the first step and rejects the next. Both
  direct and graph Solver submissions leave all 20 shared views unchanged;
  private particles **and grid quantities** exactly match the accepted prefix,
  and further queued steps do not erase the first failure.
- Grid-only simulation preserves volume and pressure support without synthetic
  particle counts; GPU-to-GPU external seeding, paused reset and subsequent
  empty replay preserve the four unchanged graph variants.

The final CUDA Release lab and all nine CUDA fixture executables build. The
CUDA-OFF Release build, nine Windows CTests, 244 Node tests, seven DX12 exchange
and eleven flowing-band GPU cases also pass. Existing Bullet/RmlUi dependency
warnings remain; the new CUDA files compile without new warnings.

`cuda-validation.json` version 6 records **141 passing CUDA cases**. Its 100
source hashes, 358 shader hashes and nine executable hashes were independently
checked. The manifest separately marks joint Solver lifecycle and late-pressure
rollback coverage; coupled interface flux and grid-owned surface rendering remain
false.

Final lab SHA256:

- CUDA: `385E317BBBF7A06C9A6C2254D68FB2F267EA9D88013436BFB2957F8BC862F727`.
- CUDA-OFF: `437AFBF17EBAF580A0B5AF044198A52F5627F29BEC7052B718501BF3AF9572DF`.

Fresh live primary-context CUDA graph regressions cover owned and reference
inlet/mixed/reset scenarios, with CUDA-OFF short/inlet checks: eight runs and
688 frames. All numerical/optical gates pass; sources, shaders, reports and
executables match their manifests. The three CUDA final-capture pairs have
**zero differing scalars across all ten channels**, with finite-value and bounded
caustic-energy audits passing. These are regressions of the existing
all-particle gameplay path, not rendered joint-grid acceptance or a full temporal
sequence comparison. No new raw-performance result is claimed.

No new-source sanitizer run was attempted after the single replacement elevation
request was canceled. Earlier sanitizer manifests apply to their recorded older
sources, not this checkpoint. D3D12 debug/GBV remains unavailable (0x887A002D).

### Required next work

Connect particle/grid phase transport to shared available capacity and moving
geometry, including restoration/allocation limits and a coherent free-surface
update. The new API explicitly rejects moving-capacity/collider submissions,
particle-only density repair, particle-only capillarity and partial/ballistic
fixture modes rather than silently applying those to only one inventory.

The grid-only test also exposes an important unfinished interface issue:
backward-Euler transport creates nonzero dilute tails, and after eight substeps
all 693 cells participate in pressure despite zero particle counts. This is not
a sharp liquid-air interface or evidence that the whole domain is full of water.
Do not fix it by deleting tiny quantities or rendering every positive owner as
a negative liquid box. Canonical phase-aware pressure/density/capillarity and
surface reconstruction must consume these owners coherently before live
retirement is enabled.

Variable-scale APIC and affine/angular ownership, conservative topology changes,
predictive fine error probes, sparse surface seams, moving optical acceptance,
sanitizer qualification and bounded raw-frame latency remain required. The full
CUDA/multiscale goal remains active.

## Geometric phase transport checkpoint — 14 September 2026

### Implementation

`cuda/fluid_cuda_geometry.cuh` reconstructs a plane from **total particle plus
grid liquid volume**, not the fraction owned by the grid. Its original analytic
Cartesian clipping integrates that plane over fine children and swept face slabs.
Plane coordinates are measured from the liquid-side corner, preserving tiny
intervals near either side of a cell. Analytic inverse branches and a relatively
bounded search interval avoid losing thin regions with almost axis-aligned normals.
Zero-gradient partial cells explicitly remain unresolved homogeneous mixtures;
the implementation does not invent their missing interface orientation.

`fluid_cuda_geometric_transport.*` uses the existing area-restricted projected
MAC rates. Low-order donor fluxes and geometric candidates feed multidimensional
flux correction, with both net-capacity and gross-donor budgets. The accepted
liquid flux transports all four grid-owned quantities with the same donor weight.
This preserves shared-face accounting, positivity and a nonnegative retained
old-fluid contribution. Proven complete geometric drainage transfers the exact
owned quantity, avoiding one-ulp residue without a small-volume cutoff.

The component computes its CFL schedule on the GPU, targeting at most 0.45
incoming/outgoing swept-capacity fraction per transport substep. Persistent
scratch and a bounded captured launch sequence support up to the configured
substep cap; exceeding it rejects publication rather than shortening simulated
time. These are transport subdivisions, not independently qualified larger fluid
timesteps. Malformed inputs, incompatible Cartesian capacities, invalid planes,
overfill and nonfinite fields also reject the trial. Device diagnostics retain
the first failing stage/index across further queued work. No per-substep host
readback or allocation was introduced.

The actual joint Solver now uses this transport and the same reconstructed
total-phase plane to distribute grid-owned volume over its fine-cell transfer
cache. Existing particle-only callers and the older implicit transport component
remain intact. Canonical face restriction is shared without allocating the
unused implicit-solver workspace. The odd joint fixture uses 117,256 bytes of
transfer/transport workspace plus the unchanged 895,588-byte transaction staging.

Geometric VOF and multidimensional flux-correction concepts are described in
[Weymouth and Yue (2010)](https://repository.tudelft.nl/record/uuid:d030e6e1-9153-4f27-be72-dc987ac208d6) and
[Zalesak (1979)](https://data.coaps.fsu.edu/pub/eric_back/OCP5930/Papers/FCT-JCPv31.pdf).
This is original engine-integrated code, not a claim to reproduce either complete
paper algorithm.

### Validation and fixes found

The moving-front Solver fixture exposed a real plane-volume rejection despite
converged pressure. A unit-cell bisection interval had insufficient relative
precision for a thin region. The inverse reconstruction was corrected rather
than relaxing its volume check. The independent clipped-polygon integration
oracle also required axis permutation to avoid ill-conditioned endpoint areas;
its tolerance was not relaxed.

`NVMatrixEngineCudaGeometricTransportTest` has **12 passing cases**:

- 150 planes checked against independent polygon clipping and piecewise
  integration, including near-degenerate normals and 1e-30 fractions:
  maximum absolute volume-fraction error 3.33067e-16.
- Forward/reverse analytic slab translation, direct and captured replay, with
  three GPU-selected subdivisions per update. Maximum error is 1.77636e-15;
  analytically dry cells are exactly empty, not dilute positive tails.
- Simultaneous three-axis circulation with partial, full and odd-extent cells.
  Independent face balances, global volume/linear momentum and translational
  energy checks pass; maximum face-balance error is 2.22045e-16.
- Nine rejection variants, retained failure evidence, ten invalid API variants,
  unchanged source buffers and allocation guards.
- Tiny ownership survives transport. A sub-ulp sweep transfers a complete
  far-corner 1e-30-volume owner exactly. A varying ownership tag in fully occupied
  water follows an independent donor-flux oracle without creating an air boundary.

All **35 Solver cases** pass. A localized water-block reference identifies an
actual later increase in pressure work after 12 reference steps. An eight-iteration
cap accepts the selected first step and rejects the next, in direct and graph
submission. All 20 shared views remain unchanged; private particle and grid
quantities exactly match the accepted prefix. The grid-only case retains volume
and zero particle counts while activating 359 of 693 pressure cells after eight
substeps, instead of the prior domain-wide dilute support.

Both Release builds pass. The complete `cuda-validation.json` version 7 records
**153 passing CUDA cases** with 104 source, 358 shader and ten executable hashes,
all independently verified. Nine Windows CTests, 244 Node tests, seven DX12
exchange and eleven flowing-band GPU cases pass. Existing Bullet/RmlUi dependency
warnings remain; the new CUDA files compile without new warnings.

Final lab SHA256:

- CUDA: `2E26B1C2E2E9629BC3E567739E358FCF72591134F523774E71D4B052E3D3638D`.
- CUDA-OFF: `0205B8F07544A66C559C2C63A368210C2C693AA2E83FC5D48B4040DD1F2CB1DF`.

Fresh primary-context CUDA graph tests cover owned/reference inlet, mixed-pressure
and reset runs; CUDA-OFF short/inlet tests bring this to **eight runs, 688 frames**.
Their executable, source, shader and report hashes match their manifests. All
captures pass finite-value and bounded-caustic-energy checks. Each of the three
CUDA comparison pairs has zero changed values among 8,181,860 scalars across ten
channels. This is existing all-particle gameplay regression coverage, not rendered
joint ownership or a full temporal-sequence comparison. No performance claim is
made from these correctness runs.

The user's subsequent single persistent-admin request was issued once; Windows
reported cancellation and the retained launcher exited 1. No further UAC request
was made. The new source has **not** been sanitizer-qualified. The sanitizer
runner now includes the geometric fixture, but requires an already approved live
elevated session. D3D12 debug/GBV remains unavailable (0x887A002D).

### Remaining acceptance work

This is finite-domain phase reconstruction and conservative transport, not a
finished coupled incompressible free-surface solver. Actual particle cell
crossings and the Eulerian phase guide still need capacity-compatible coupling.
The next bins reconstruct total phase from the two authoritative inventories;
the guide is not another mass owner. In particular, the existing FP32 pressure
divergence tolerance alone does not establish the much tighter full-cell capacity
bound. Do not fix that gap by clipping volume or weakening acceptance.

Partial/moving cut-cell geometry remains unsupported by this geometric path.
Fine-child wet volumes are integrated geometrically, but each child's existing
B-spline momentum transfer still assumes a uniform contribution within that
child; exact subcell wet-polyhedron integration is not implemented. A single
coarse plane also cannot represent every disconnected sheet/droplet configuration.

Canonical density/capillarity and `FluidSurface`/DXR/caustic support for grid owners,
moving-capacity coupling, conservative bounded retire/restore, affine/angular and
centroid ownership, predictive fine error probes, variable-scale surface halos,
optical seams, sanitizer qualification and raw-frame latency gates remain open.
Gameplay still passes particle-only views. The full CUDA/multiscale contract and
its original numerical, optical and performance requirements remain active.

## Full-pool capacity admission checkpoint — 14 September 2026

### Conservative correction and GPU scheduling

A new fully occupied grid-owned pool failed at `GeometricLowOrderBounds` even
after pressure converged. Small divergence in the stored FP32 MAC faces made
the low-order reference overfill a cell, invalidating the subsequent flux
correction's assumption. The pressure and external capacity tolerances have not
been relaxed, and no owned water is clipped or deleted.

`fluid_cuda_geometric_transport.cu` now admits incoming low-order transfers
against each receiver's free volume plus the outflow accepted by its neighbors.
Monotonically decreasing receiver factors propagate downstream restrictions
through shared faces, including fully occupied cycles. An independent endpoint
reduction requires excess below 2e-14 relative capacity before the existing FCT
pass; the external 2e-13 capacity gate remains unchanged. Exhausting the iteration
budget rejects publication through the sticky failure latch. Momentum uses the
same accepted donor-volume transfers. This is an original conservative flow
constraint, not another pressure solve or a minimum-energy correction proof.

The solver reuses FCT scratch; the new control record adds 40 bytes, bringing the
odd joint fixture's transfer/transport workspace to 117,296 bytes. Transaction
staging remains 895,588 bytes. `maxCapacityIterations` defaults to 128, accepts
1–512, and bounds device work. During graph replay a conditional WHILE body
exits on convergence/failure. The existing separately instrumentable pressure
loop translation unit also owns this handle-setting kernel. Unrolled execution
remains selectable; no per-iteration CPU readback or allocation is introduced.

### Verified evidence

Both CUDA-enabled and CUDA-OFF Release builds pass. `cuda-validation.json`
version 8 records **157 passing cases**; all 105 source hashes, 358 shader hashes
and ten executable hashes were independently checked. Nine Windows CTests,
244 Node tests, seven DX12 exchange and eleven flowing-band GPU cases also pass.
Existing Bullet/RmlUi dependency warnings remain.

- The full-pool Solver test runs eight steps and compares all 20 shared views
  bitwise between conditional and unrolled execution. Independent capacity
  excess is at most 3.94794e-16; absolute volume error is 2.22045e-16 m³.
  Admission takes 81 total iterations, addressing an initial relative excess
  of 2.5744e-8. Maximum face-transfer reduction is 7.88779e-10 m³.
- Captured node counts are 1,620 versus 35,912. These are scheduling counts,
  **not measured frame-time or FPS improvements**.
- Independent four-cell circulation tests recover the analytic bottleneck
  flux in three iterations, including direct and graph execution. A one-iteration
  cap preserves outputs, transfers and failure diagnostics on repeated replay.
  Active → zero-flow → active replay reuses one graph and restores the identical
  active result. Twelve invalid API variants include both capacity-budget bounds.

Final lab SHA256:

- CUDA: `FFFDB8ED3683DEA3B65D1D0550F29E55C976036A12E38A567383B07E7DD46F2E`.
- CUDA-OFF: `C3878BC5B7205317FAAD8FA0478F554951F9FEA1FCB25A04FEF71143200727F9`.

Fresh CUDA primary-context graph owned/reference inlet, mixed and reset runs,
plus DX12 short/inlet runs, total **eight runs and 688 frames**. Each manifest's
188 source hashes, executable, shader and report hashes match. Finite guide and
bounded caustic-energy checks pass. Each CUDA comparison has zero changed scalars
out of 8,181,860 across ten channels. These are final-capture regressions of the
existing all-particle renderer, not rendered joint-grid or full temporal-sequence
acceptance. No raw-performance profile was run.

The user's latest explicitly requested single elevation attempt ended with
Windows reporting cancellation. No additional prompt was issued. The geometric
hybrid-memcheck target compiles, but no current-source sanitizer run was attempted;
compilation is not sanitizer qualification. The runner now accounts for both
pressure and capacity handle kernels when exclusions are explicitly requested.
D3D12 debug/GBV remains unavailable (0x887A002D).

### Next integration boundary

This fixes the low-order phase reference for grid-owned water. Actual particle
endpoint cell crossings still do not share this flux constraint. `Joint::capacity`
and `gatherGridPhase` currently assign each particle's entire rest volume to its
containing coarse bin, which is not a continuous physical-support model. Do not
mistake the new full-pool test (zero particles) for coupled-interface acceptance.

The next coupling change must reconcile particle support and the advected total
phase without freezing particles at ownership boundaries, losing mass, or
inventing a second inventory. In particular, incoming particles can require
outflow from an already full grid owner; limiting only incoming grid flux is
insufficient. [Narrow Band FLIP, section 2](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2016/NBFlip/nbflip.pdf)
uses passively advected samples rather than this engine's exact mass ledger;
its sampling-density handling is not a proof of local capacity for our ownership
model. Preserve the ledger while establishing coherent physical support and
cross-representation phase transport.

Moving cut cells, canonical grid-owned density/capillarity and DXR surfaces,
affine/angular and centroid conservation, bounded retire/restore, variable-scale
surface seams, optical acceptance, sanitizer qualification and raw-frame latency
remain required. The full CUDA/multiscale objective remains active.

## Transactional geometry publication checkpoint — 14 September 2026

### Engine connection

The optional `SurfaceGeometry` pair in `fluid_cuda_kernels.h` exposes one
`double2(totalLiquidVolume, geometricCapacity)` and one `double4(planeNormal,
intercept)` per coarse owner. These are **non-owning geometry descriptors**,
not a second water inventory or a signed-distance field. The plane uses the
existing reflected liquid-corner convention, including negative normal axes;
a zero normal denotes an unresolved homogeneous fraction.

`fluid_cuda_transaction.cu` extends the same guarded publication from 20 to
22 shared views when outputs are requested. It copies directly from the final
validated bin/reconstruction cache, after all substeps. Output destinations are
never snapshotted as inputs and need no duplicate private working allocation.
The odd fixture retains its 895,588-byte simulation staging allocation. Geometry
exports themselves require 48 bytes per coarse owner in caller-owned shared
buffers. Complete-pair, joint-ownership, enum and alias validation precedes use.

`FluidCuda` now accepts optional grid inventories and geometry resources, imports
them through the existing `gpu::CudaInterop`, and returns ownership on the same
external fence. Its report identifies `gridOwnedInventory` and
`surfaceGeometryOutputs`. Ordinary callers still pass their original 14 or
18 buffers. `FluidSystem` has not enabled grid retirement or this output pair.

The production HLSL helper `shaders/fluid/owned-phase.hlsli` interprets occupancy
inside a reconstructed cell using double-precision reflected-plane arithmetic.
It explicitly is **not** a continuous field or a function to sphere-trace across
cell boundaries. Neighboring descriptors must first become a single continuous
node field before `FluidSurface`, camera/photon/laser DXR intersections and water
medium handling consume them. No particle proxies or full negative boxes were
introduced for small grid quantities.

### Validation

The complete version-9 `cuda-validation.json` contains **162 passing CUDA cases**.
All 110 source, 360 shader and eleven executable hashes were independently
verified. Both Release builds, nine Windows CTests, 244 Node tests, seven DX12
exchange and eleven flowing-band GPU cases pass. Existing dependency warnings
remain; no new source warning was emitted.

- **39 Solver cases:** new direct/graph lifecycle tests compare published phase
  against independently enumerated final particle and grid owners, with zero
  observed volume difference. They cover initial paused publication from invalid
  destination contents, motion, paused refresh, empty reset and allocation guards.
  Six invalid API variants are rejected. Existing first- and later-failure tests
  now also protect both geometry outputs: all 22 shared views remain unchanged.
- **Two engine-adapter cases / ten handoffs:** `NVMatrixEngineCudaSurfaceTest` uses
  the actual `FluidCuda::run`, not a replacement test adapter. DX12 seeds grid
  water on the GPU; CUDA produces the phase/planes; a DX12 shader reads them
  immediately after the return fence. The independent analytic slab oracle
  checks both normal orientations, quarter-filled cells, odd owner extents,
  accepted substeps, empty reset and unchanged output after a rejected capacity
  update. No simulation-data CPU transfer occurs between producers/consumers;
  only the final diagnostic snapshot is read back.

Final lab SHA256:

- CUDA: `59435AAA65C7CCEBF7522F45659BFD69DBC8F85A847514F6003A91B590429A06`.
- CUDA-OFF: `68A19AC6D935D0FC970553333B4180959B48288224EFC593A48C9EC59030D9E9`.

Fresh owned/reference CUDA graph inlet, mixed-pressure and reset cases plus DX12
short/inlet cases total **eight runs, 688 frames**. Each manifest's 190 source,
360 shader, executable and report hashes match. Finite-guide and bounded-caustic
energy checks pass. Each CUDA owned/reference pair has zero changed values among
8,181,860 scalars across ten channels. This is current-build all-particle
regression coverage, not an old/new-build comparison, a full temporal-sequence
comparison, or rendered joint-grid acceptance.

No new elevation request, sanitizer execution or performance profile was made.
Current-source sanitizer qualification remains open; older sanitizer results do
not cover this change. D3D12 debug/GBV remains unavailable (0x887A002D).

### Next work, unchanged requirements

Construct and validate a continuous, mass-aware surface from these descriptors
and particle support, including thin regions and ownership-transition seams.
The existing weighted-center particle formula cannot simply treat a tiny grid
owner as a full-sized particle. Publishing a descriptor is not rendered volume,
surface-normal, caustic-energy or motion-vector acceptance.

Particle endpoint/phase capacity coupling, moving cut cells, density/capillarity,
bounded retirement/restoration, affine/angular and centroid ownership, sparse
variable-scale reconstruction, optical acceptance and raw-frame latency gates
remain open. The original full CUDA/multiscale objective remains active.

## Continuous phase-field checkpoint — 14 September 2026

### Subsystem connection

`FluidSurfaceInput` now separates the existing GPU view/frame metadata from the
owning `FluidSystem`. The ordinary `record(FluidSystem, ...)` wrapper remains in
place. An explicit caller can additionally supply the joint Solver's output-only
total-phase and plane resources, after the existing CUDA/DX12 fence handoff.
The implementation reuses the current sparse page table, 9-cubed node blocks,
indirect reconstruction, conservative cell masks, tight AABBs and procedural BLAS
builder. It adds no simulation inventory, synthetic particles or CPU brick readback.

`owned-surface.hlsli` computes each canonical node from the analytically integrated
liquid volume in a half-MAC-cell-wide box. Its support intersects at most eight
coarse owners. `owned-phase.hlsli` implements the stable reflected-plane integral
with explicit FP64 scalar arithmetic; full/empty owners take short paths. Node
coordinates come from the shared integer lattice, including duplicate brick-face
nodes and odd-width boundary owners. The output is a continuous scalar, **not**
a distance bound; the existing trilinear DDA/polynomial intersection remains
unchanged. Adding particle kernels to this total-phase field would double-count
their volume, so that is deliberately not the pending hybrid-blending solution.

The initial wall test exposed contour shrinkage when the box filter treated
out-of-domain samples as air. Reconstruction now normalizes the domain-truncated
filter and intersects it with the exact bounded-domain boundary. A second failure
exposed the existing diagnostic's all-nonpositive tetrahedral degeneracy: if any
corner is negative and none positive, trilinear phi is negative throughout the
open cell, even when a linear tetrahedron has four zero vertices. That volume is
now counted exactly; genuinely crossing cells retain the diagnostic tetrahedral
estimate. These corrections do not edit or clamp simulation mass.

Material displacement uses the projected staggered MAC velocity times the actual
advanced simulation time. On pause it is cleared once, then the stationary field
is reused. This is a first-order guide, not validated deformation/reprojection
through a changing water surface. Phase mode requires paired, nonaliasing,
sufficiently sized UAV buffers and FP64 support. Unqualified particle-only surface
LOD, legacy dormant interiors, fixtures and SDF carving are explicitly rejected.
Normal particle reconstruction remains a separate shader variant and the default.

### Focused hardware evidence

The real `FluidCuda` adapter test now invokes the production `FluidSurface` and
BLAS builder after CUDA publication, without a simulation-data CPU round trip.
Both direct and graph replay pass five handoffs (bottom pool, reflected upper
pool, empty reset, replacement and rejected-capacity preservation). Each checks
7,066 duplicate shared nodes bitwise. An independent physical axis-box overlap
oracle observes zero nodal error and zero planar contour-volume error. Three
additional surface frames per path validate constant-MAC material displacement,
pause clearing and no unnecessary second stationary rebuild.

A separate GPU case checks 324 box integrals against an independent
inclusion/exclusion oracle, including all eight normal sign combinations,
oblique clipping and positive/negative axial thicknesses down to 1e-30. Maximum
absolute error after FP32 output is 2.6491e-8. These are **integral precision**
tests, not proof that the finite node lattice renders a 1e-30-thick sheet.

### Final-build regression evidence

- Both full Release configurations build; the final motion-guide fix was then
  rebuilt into both lab binaries. Existing Bullet/RmlUi dependency warnings remain;
  the new surface/test translation units compiled without new warnings.
- All nine Windows CTests and 244 Node tests pass.
- `test-cuda.ps1 -NoBuild` passes **163 cases**. The version-10 manifest records
  116 source hashes, 363 shader hashes and 11 executable hashes; these were
  independently recomputed and verified after the suite completed. It explicitly
  marks canonical phase-field and procedural BLAS construction coverage, while
  keeping phase optical acceptance and live grid-owned rendering false.
- Five sequential live runs cover 320 frames with FG off: CUDA uniform inlet
  and mixed-pressure inlet, each owned and reference, plus DX12 owned inlet.
  Each runs 64 frames and emits 1,258 particles. All simulation, ownership and
  existing optical checks pass. Each live manifest's 191 source hashes, shader
  hashes, executable hash and report hashes were independently verified.
- Both CUDA owned/reference **final-capture** pairs have zero differing scalars
  out of 8,181,860 across ten channels. Finite guides, caustic-energy bounds and
  history-age checks pass. This compares ownership modes in the current build;
  it is not an old/new binary comparison, full temporal sequence or new phase
  surface optical validation.

Final lab SHA-256:

```text
CUDA:     F7CC641CA1162218517DBEC0C335C7B354EFAE761D6D38AA0C182B2616594EFF
CUDA-OFF: D7AFBB2745931AFC862E184FC792010E435C84574975CB84BDAA0C7F48950406
```

No performance profile, current-source sanitizer run or elevation request was
made. D3D12 debug/GBV is still unavailable on this installation (0x887A002D);
passing numerical fixtures are not a replacement for that validation. All test
and build processes finished; the lab was closed by the bounded test workflow.

### Remaining acceptance boundary

The new field is not yet selected by playable `FluidSystem`, and the tests above
build the procedural BLAS but do not dispatch camera/photon/laser rays through it.
The ordinary rendered path must remain unchanged until those optical comparisons
pass. A fixed-resolution, filtered single-plane-per-owner representation cannot
by itself preserve arbitrary subcell droplets, sheets or two interfaces inside
one owner. Particle-supported detail, variable-scale reconstruction, seam normals,
moving contour volume, nested media, caustic energy and RR sequences remain
required; this checkpoint does not substitute the phase-only field for that goal.

Actual particle endpoint/phase capacity coupling, moving cut cells, canonical
density/capillarity, bounded retirement/restoration, centroid/affine/angular
ownership, predictive fine probes and the original latency gates remain open.
No automatic retirement, new launcher default or speedup claim is introduced.

## Phase-surface DXR checkpoint — 14 September 2026

### Actual GPU intersection and dielectric use

`tests/cuda_surface_rays.h` extends the existing surface adapter fixture with a
bounded TLAS and `DispatchRays` pipeline. It consumes the production `FluidSurface`
BLAS, field and page table immediately after the real CUDA Solver publication and
surface reconstruction. It does not reconstruct a second rendering field or use
compute traversal. Static ray inputs and a manufactured optical table are uploaded
once; the evolving fluid geometry remains GPU-resident between these producers.

`cuda-surface-rays.hlsl` includes the production `common.hlsli`,
`fluid/field.hlsli` and `fluid/intersection.hlsli`. The unchanged `glass` and
`interfaceMedia` functions were extracted from `transport.hlsl` into
`interface-media.hlsli`, shared by the ordinary renderer and this fixture. Thus
the test exercises the actual scalar sampling, filtered normals, ambient-medium
classification, interface selection, Fresnel and extinction functions. It sets a
known water material rather than exercising the scene object's material lookup.

Every phase handoff now checks both tight software bounds and the existing
full-brick traversal reference in separate, completed submissions. The same
195 primary rays cover top/bottom faces, sides, inside starts, near-parallel
grazing paths, exact and nearby brick seams, misses and instance masks. Each also
tests opaque visibility with closest-hit shading skipped. Transmitted rays entering
water continue through the same procedural geometry to their exit boundary.
Empty reset must remove intersections and interior-medium classification; a
rejected capacity update must preserve the previously accepted geometry.

An independent double-precision slab oracle checks distances, visibility and
inside/outside classification. Analytic normals are checked on planar face
interiors; deliberately filtered sharp edges/corners instead require finite unit
normals and correct hit geometry. Independent Snell/Fresnel and Beer calculations
check the optical outputs. The manufactured table has IOR 1.333 and RGB extinction
0.135/0.165/0.21 per metre; it is not a new physical-water preset or a validation
of spectral water data. TIR checks the unit reflection probability and zero
transmission direction, not a complete internally reflected path tree.

### Focused hardware results

For **each** of direct CUDA launch and CUDA graph replay:

- 1,950 primary-ray checks, with matching visibility checks;
- 1,262 hits, including 1,220 planar-normal checks;
- 1,140 transmitted water segments and 14 TIR checks;
- maximum hit-distance error: 3.52432e-5 m;
- maximum planar-normal chord error: 5.67763e-5;
- maximum Fresnel/refraction component error: 1.18109e-5;
- maximum transmitted exit-distance error: 6.32352e-5 m;
- no exhausted production DDA traversal.

These are actual DXR `TraceRay` fixtures over CUDA-published fields, not SER
performance measurements or executions of the full camera/photon/laser raygens.
Existing continuous-field, seam, contour-volume, pause and clipped-integral checks
remain in the same executable.

### Final-source regression evidence

Both full Release builds and shader refreshes pass. All nine Windows CTests and
244 Node tests pass. The CUDA matrix now has **165 passing cases**; its version-11
manifest's 122 source, 364 shader and 11 executable hashes were independently
verified. `phaseDxrIntersections` and `phaseDielectricFixtures` are true; full phase
optical acceptance and playable joint-grid rendering remain explicitly false.

All six production `Transport-{0,1,2}-{0,1}.dxil` variants have byte-identical
SHA-256 hashes before and after extracting the shared interface functions. They
also match between the CUDA and CUDA-OFF builds. This confirms the extraction did
not change the compiled camera/photon/laser transport programs.

Five fresh, sequential rendered regressions pass: CUDA uniform inlet and mixed
inlet in both owned/reference modes, plus DX12 owned inlet. They total 320 frames
with FG off. Each live manifest's 193 source hashes, shader hashes, executable
hash and report hashes were verified independently. Both CUDA owned/reference
final-capture pairs again have **zero differing values out of 8,181,860 scalars**
across ten channels; finite guides, caustic-energy bounds and history-age checks
pass. These remain current-build ownership comparisons on the ordinary particle
surface, not new phase-surface rendered sequences or before/after image captures.

Final lab SHA-256:

```text
CUDA:     3FBD55328B9C23A3A58BE420154175871B1899F22978969CD7D13A2069A5A3A3
CUDA-OFF: 9C3235C143DDB71D4260065B4C299E2CF5F29A1A9A8C5B28308847A4458E3B55
```

No new UAC request, current-source sanitizer run or performance profile was made.
The shader refresh used process-scoped execution policy only, without changing
machine/user policy. D3D12 debug/GBV is still unavailable (0x887A002D). All builds,
tests and lab processes completed; `git diff --check` passes.

### Remaining work

Before enabling playable joint-grid rendering, extend geometric/error qualification
to nonplanar and moving interfaces, then connect the engine's full camera/photon/
laser, nested-media and RR paths. The planar oracle is not evidence for caustic
focusing, temporal radiance or arbitrary subcell sheets/droplets. In particular,
particle-supported detail and physically coupled particle/phase capacity remain
prerequisites for automatic retirement; the fixed-grid phase field is not a
replacement for the requested adaptive reconstruction. Other density/capillarity,
moving-cut-cell, conservation, sparse scheduling and latency gates remain open.

## Moving-plane surface checkpoint — 14 September 2026

### Failures found and corrected

The new manufactured-plane fixture exposed an actual 0.0187805 m DXR hit error
when a horizontal interface moved from a field node to y=1.325 m. The old
half-interval occupancy filter saturated too early; linearly interpolating its
remapped occupancy displaced the zero even though the published PLIC plane was
correct. Merely widening that filter then exposed a 0.055912 normal-chord error
near the domain wall: interpolating nodal `max(phi, domain)` and filtering its
gradient bent an otherwise flat free surface.

`owned-surface.hlsli` now integrates a symmetric box with radius one fine MAC
cell (two field intervals), with the corresponding activation halo. Cut portions
contribute normals in a common physical metric, including odd-width owners.
The shader inverts the integrated half-space volume to obtain a local
volume-matched plane position. Axis-aligned and half-volume inversions are exact
closed forms; other cases use a bounded 32-step FP64 inversion. Normal scaling
avoids narrowing tiny accumulated magnitudes before normalization. This is a
local reconstruction approximation, **not** a certified distance bound or a new
simulation inventory. Its cost has not yet been qualified at gameplay scale.

The canonical phase representation is now the raw trilinear free-surface field
**intersected with the exact Cartesian simulation domain**. The existing page-map
tail carries the enable word and six bound components (28 additional bytes).
`SurfaceClear` publishes that metadata with the field transaction; ordinary
particle surfaces explicitly disable it. No new renderer binding or CPU field
readback is introduced. Shared scalar sampling, filtered free-surface normals,
analytic wall normals and production DXR entry/exit clipping use this same
representation. Thus camera/photon/laser code and the existing buoyancy/whitewater
scalar consumers share the boundary convention.

Surface masks include the domain boundary conservatively. The volume diagnostic
integrates raw scalar values only in interior domain cells, avoiding the old
wall-clamping bias. Integer lattice coordinates classify these cells and boundary
nodes, rather than comparing rounded world coordinates. Arbitrary moving mesh
cut cells are not covered by this Cartesian-domain implementation.

### Hardware qualification

The existing actual CUDA Solver → DX12 surface fixtures still pass direct and
graph launch, reset/rejection, paused-motion cleanup and shared-node checks.
Their planar box-volume and raw-node errors remain zero. The separate GPU-seeded
geometry fixture supplies exact PLIC descriptors, so it isolates renderer
reconstruction from the Solver's separate plane-estimation algorithm.

Across 23 successive manufactured positions/orientations, including all eight
normal-sign combinations and small offset increments:

- 8,970 primary DXR ray checks over tight and full-brick bounds;
- 7,274 hits, 7,082 analytic-normal checks, 6,610 transmitted segments and 76 TIR checks;
- maximum hit-distance error 3.36801e-5 m;
- maximum normal-chord error 4.41401e-7;
- maximum Fresnel/refraction component error 2.45129e-7;
- maximum transmitted exit-distance error 3.76764e-5 m;
- 43,289 identical shared-node checks and 5,535 near-interface affine-node checks;
- maximum affine-node error 1.643e-8 m; no exhausted DDA traversal.

An independent inclusion/exclusion volume oracle checks the oblique domain cuts.
The existing tetrahedral diagnostic's maximum error is 0.00146493 m³, within its
derived per-cut-cell 1/256-volume quantization bound. That is a diagnostic error,
not a measurement of physical volume drift. These are geometry-update sequences,
not time-integrated fluid advection or temporal radiance/RR acceptance sequences.

### Final-source regression evidence

Both full Release builds and explicit shader refreshes pass. All nine Windows
CTests and 244 Node tests pass. The final CUDA matrix has **167 passing cases**;
its version-12 manifest's 123 source, 365 shader and 11 executable hashes were
independently verified. Full phase optical acceptance remains false.

Five fresh sequential rendered runs pass, totalling 320 frames with FG off:
CUDA uniform/mixed inlet in owned and reference modes, and DX12 owned inlet.
The three live manifests' 193 source hashes, shader hashes, executable and report
hashes were verified. Each CUDA owned/reference pair is bit-identical across
8,181,860 scalar values in ten channels. Finite-guide, caustic-energy and
history-age checks pass. These runs still use the ordinary particle surface.

Before overwriting captures, the prior checkpoint's three owned captures and
manifests were archived in `/tmp/ptpuzzle-phase-before-eBD98P`; their report hashes
match the prior manifests/binaries. Unlike the current-build ownership pairs,
**old/new rendering is not bit-identical**. Across the three final captures:
raw-RGB relative L1 difference is 0.01008–0.01128%; RR-output RGB difference is
0.5497–0.5637%; water-caustic RGB difference is 0.00710–0.01348%. Depth, diffuse
albedo and the non-water caustic atlas are identical. Integrated caustic Y changes
by at most 2.921e-6 relative. These comparisons exclude alpha/history values from
RGB metrics and are final-capture comparisons, not a full temporal series or
proof of unchanged performance. All six Transport shader variants match between
the CUDA and CUDA-OFF builds, but are not the previous checkpoint's binaries.

Final lab SHA-256:

```text
CUDA:     9B960B72A666EFA70446C709E0BEEBBAD98248BD1F1863F99E8441F47102E6EE
CUDA-OFF: D638D718226A62CD29F5D832F3DB550FE4A19DA8648A913B37C49906A8F5710D
```

The user's one requested elevation attempt was canceled by Windows; no new
elevated session was established and no retry was issued. All work above ran
without it. D3D12 debug/GBV remains unavailable (0x887A002D); no current-source
sanitizer or performance qualification is claimed.

### Remaining requirements

Curved interfaces, opposing/unresolved local orientations, detached sheets and
droplets, variable particle support and LOD transitions still need explicit
geometric/error qualification. A local plane fit cannot substitute for that
reconstruction. Full camera/photon/laser focusing, nested media, moving caustics
and RR sequences over **phase** geometry remain open. Actual particle endpoint
capacity coupling, canonical density/capillarity, moving cut cells, conservative
centroid/affine/angular ownership, bounded retirement/restoration, predictive
refinement and the original latency gates remain required. No new gameplay
default or automatic retirement is enabled by this checkpoint.

## Deep-pool cost batch (2026-09-14)

The 900k-particle, 8m-deep pool exposed a mixed-pressure cost of ~35.64ms
simulation / ~56.84ms raw frame, versus ~9.94ms / ~30.55ms for DX12 uniform
(medians across three pre-change runs, FG off). These are cost comparisons,
not equal-error solver comparisons: uniform uses fixed Jacobi, mixed uses
convergence-qualified MGPCG.

This batch addresses four sources of avoidable work without changing particle
count, reconstruction resolution, pressure tolerance or caustic quality:

1. Canonical FP64 pressure application uses local six-face stencils on regular
   fine/fine and coarse/coarse faces. Only T-junctions gather mixed patches.
2. Refinement computes eight-cell guard summaries once, then combines 27 summaries
   for the exact previous 216-cell halo. Exact per-cell topology tags gate reuse
   of accepted front-buffer rows, Galerkin hierarchy and bottom Cholesky factor.
   Every RHS is still rebuilt; changed wet/solid/ownership/residency state and
   reset invalidate the cache. No host readback drives these decisions.
3. Compact-support early-outs avoid zero-weight APIC/ownership, covariance and
   reconstruction work. G2P precomputes separable quadratic basis values. This
   is **not** narrow-band particle retirement: all particles remain authoritative.
   Safely removing deep particles still depends on the ownership work above.
4. Pressure warm-starts from geometrically indexed accepted pressure. A fresh
   FP64 residual must improve both norms over zero initialization, otherwise the
   guess is discarded. Relative convergence remains referenced to the fresh RHS,
   with the same final stored-face divergence gate. History commits only after
   that gate, survives page-buffer swaps, and is invalidated by reset.

Reports expose `pressureHierarchyBuilds`, `pressureHierarchyReuses` and
`pressureWarmStarts`. Added GPU regressions cover repeated direct/graph solves,
bad warm guesses, RHS changes, topology changes, reset and an independent 6³
guard oracle on odd-sized grids. Validation/performance results follow after
the single end-of-batch check; no speedup is assumed from code inspection.
