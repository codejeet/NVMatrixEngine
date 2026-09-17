# Flowing particle/grid ownership — integration work

## Live implementation

The opt-in `--fluid-narrow-band` mode now connects actual particle retirement,
flowing grid inventory, automatic restoration and mixed-owner rendering through
the complete CUDA solver. See [NARROW_BAND.md](NARROW_BAND.md) for the ownership
contract, launcher, validation command and remaining limitations. The checkpoint
history below describes earlier component stages; its retirement-disabled
statements do not describe this new mode. `--fluid-owned-particles` alone remains
the all-particle reference.

## Earlier integration record

The full adaptive specification remains the objective. `FluidInterior` is a
dormant lattice approximation; merely raising its wake-speed threshold would
not implement Eulerian flow. `FluidBulk` currently transports a pressure-support
replica, so its volume must not be added to particle mass.

## Chosen ownership contract

Introduce an explicit GPU exchange layer. A fluid quantity has exactly one
owner: a particle's FP64 mass/momentum record or a grid cell's FP64 inventory.
The existing 80-byte particle remains an FP32 simulation/reconstruction cache,
not an independently counted second mass. A separate particle reference velocity
allows solver velocity increments to update momentum without repeatedly rounding
the authoritative record to the cached velocity. New source requests initialize
only their actual new particle IDs; reset initializes both ownership stores.

Deposit retires the exact particles it transfers. Restoration reserves all
needed free IDs before committing a cell, distributes its full mass/momentum
(the last sample receives the arithmetic remainder), then clears the grid owner.
Insufficient slots or missing valid restoration sites leave the entire cell
owned by the grid. There is no dropped remainder, hidden pending sink, or CPU
particle loop. Restoration sites must be generated/validated on the GPU by the
geometry/band policy; the exchange layer does not invent points inside solids.
`reusableParticleLimit` can restrict recycling to the emitter's already-issued
prefix, so detail restoration cannot steal future birth IDs. A unified GPU
source allocator may instead lend the whole pool.
Seeded and restored IDs initialize their previous-position cache to the new
position. Recycling therefore cannot inherit a different particle's motion
history; this is a birth reset, not a general fluid-motion reprojection model.

## Concrete integration sequence

1. `FluidParticleGridExchange` + GPU tests: real retire/restore transactions,
   arbitrary fractional mass, momentum and bounded allocation. Borrow
   `FluidImplicitTransport` to exercise moving grid-owned mass between exchanges.
2. Connect the exchange records to `FluidSystem` binning, P2G/G2P, source admission
   and validation. Replace dyadic-only mass-quanta assumptions in this mode with
   actual weighted mass. Existing modes keep their contracts.
3. Wire `FluidComplexity`/current wet and solid bands to ownership requests and
   validated restoration sites. Deposit only low-error interior flow; protect
   interfaces, moving solids, thin features and APIC detail. Add a GPU energy/error
   policy rather than admitting every requested cell indiscriminately.
4. Make grid/particle momentum prediction consistent across the moving interface,
   borrowing the actual mixed MAC projection and its canonical flux. Preserve
   coarse global pressure coupling. Remove the pressure-support replica ambiguity.
5. Feed actual owned volume to density, capillarity and the canonical DXR surface;
   validate sharp free-surface transport and cross-LOD optical continuity before
   enabling the new path in normal launchers.

The first transaction component is not a completed Narrow Band FLIP solver or a
playable flowing-interior mode. In particular, mean grid momentum alone does
not preserve arbitrary APIC affine detail or angular momentum; the admission/error
policy and joint velocity coupling are required before automatic demotion. Sparse physical
storage, multirate scheduling and the rest of the specification remain open.

`--fluid-owned-particles` now instantiates the component in `FluidSystem`.
It initializes the exact initial/inlet birth ranges, imports solver and collision
velocity increments after every substep, and preserves reset/pause behavior.
This mode still keeps all live water particle-owned: automatic flowing-interior
transactions have not been enabled. Legacy merge/split, dormant interiors and
the bulk replica cannot be combined with this ownership mode until their
mutations share the same authoritative ledger. Normal launchers are unchanged.

### Live consumer connections

* `fluid_system.cpp`: lifecycle, source IDs, substep velocity deltas, validation.
* `binning.hlsl`: exact count/scan/scatter stays unchanged; an FP64 gather writes
  rounded FP32 cell mass without the old 1/16-unit quantization. `common.hlsli`
  reads this separate encoding for pressure/capillary consumers.
* `transfer.hlsl`: the authoritative P2G variants read mass and linear velocity
  directly from FP64 records; APIC affine rows retain the existing FP32 cache.
* `fluid_work.cpp`: compact P2G's same-state reference selects the same
  authoritative kernel, rather than comparing against a different mass source.
* `fluid_complexity.cpp` / `complexity.hlsl`: importance's velocity reduction
  reads fractional cell mass, independently of sample population.
* Optional CUDA: `fluid_cuda.*` imports the **same** FP64 quantity/reference
  records, initialized control prefix and cell-mass cache. DX12 still seeds births;
  `cuda/fluid_cuda_ownership.*` imports each substep's velocity increment.
  `fluid_cuda_transfer.cu` gathers fractional volume and reads authoritative P2G
  mass/linear velocity; the existing transaction publishes all 18 shared views
  together. Uniform/mixed, direct/graphs and CIG modes are tested. Default unowned
  CUDA retains 14 buffers. This does not yet import or transport grid-owned mass.
  A separate CUDA transport/shared-exchange fixture now consumes the actual grid
  owners, as described below; it is not yet enabled in the live solver.
* Density, anisotropy and DXR reconstruction retain rounded FP32 particle
  weights. Validation checks that these caches agree with the authoritative
  records; coarse-owned density and surface coverage remain the next connection.

The main simulation root signature now uses 60 DWORDs and reuses one UAV binding
for the records. Seed/velocity-only operation does not allocate fake restoration
sites or require absent geometry buffers. All normal-path data stay GPU resident;
only explicit bounded validation copies particle quantities to the CPU.

The CUDA ownership checkpoint in [CUDA_FLUID.md](CUDA_FLUID.md) records 101 CUDA
GPU cases, same-physics raw capture parity, live source/reset tests and a combined
foam/bubbles/DLSS-FG run. All-particle ownership is now connected on both compute
backends; joint flowing-grid transport and canonical grid-owned reconstruction
remain required before live automatic deposits.

### Error-based flowing-band policy

`FluidFlowingBand` now supplies GPU ownership requests and collider-validated
restoration sites to the same exchange component. It is compiled into the engine
and exercised by `NVMatrixEngineExchangeGpuTest`; `FluidSystem` does **not** yet invoke
automatic deposits. There is no new playable `--fluid-flowing` switch.

The concrete connections are:

* Existing `FluidSimulationConstants`, particle buffers and production GPU bins.
* FP64 particle quantities plus **only grid-owned** quantities, distributed over
  the matching fine/coarse cut-cell open volumes. The old bulk replica is not read.
* Current `SolidGrid` and the same analytical/mesh `colliderPhi` used by collisions.
* Optional `FluidComplexity` requested physics LOD. Current occupancy and geometry
  can always veto stale/frozen importance; optical importance does not weaken physics.
* Outputs directly bind `FluidParticleGridExchange::{deposit,restore}` requests,
  sites and site counts. No readback or CPU particle loop in the component.

Five original kernels gather joint ownership, estimate error, pad/hysteretically
classify the band, generate sites, and request forced restoration. The dedicated
root signature is 56 DWORDs; buffers persist between calls and passes carry PIX
events and UAV barriers. Forced restoration does not read bins invalidated by a
deposit, but regenerates sites from **current** collider geometry.

Admission measures velocity fluctuations in a comoving frame, APIC affine energy,
angular-momentum error, centroid error and covariance error against a fixed 4³
coarse-cell quadrature. This admits smooth translation rather than raising the
dormant interior's absolute speed limit. The current/step-swept surface and solid
band, neighboring flow differences, one coarse padding layer, promotion dwell
and separate retention thresholds protect detail. Spatial moments are heuristic
error estimators, **not an exact bound on every P2G kernel or optical error**.
The mean-only grid representation still cannot conserve arbitrary affine detail.

Eleven hardware policy cases use 32,768 particle slots and independently assembled
FP64 moment oracles. They run real retire → implicit shared-face transport → GPU
rebin → joint particle/grid classification → restore transactions. Smooth cases
retire 512 particles. A fully blocked restoration retains all eight grid owners;
it does not invent geometry, delete water, or consume free IDs. The circulation
is a manufactured conservative transport fixture, **not live pressure coupling**.
The existing seven transaction cases and fractional production P2G probes remain.

Remaining before live flowing ownership: joint MAC prediction/projection and
canonical phase-flux coupling, capacity-safe particle/grid interface advection,
restoration under finite allocation/rapid geometry changes, and density plus
canonical DXR surface support for grid-owned water. In particular, small transported
tails must not become full negative boxes in the scalar field. This checkpoint
does not enable grid-owned water in the playable renderer or claim a speedup.

`test-fluid-owned-particles.ps1` covers empty, APIC/FLIP, affine transfer,
inlet, mixed-MAC/sparse work and pause/reset/valve scenarios. The hardware exchange
fixture additionally runs production binning and P2G on restored fractional
quantities, then compares every face to an independent CPU gather. One cached
particle's mass/velocity is deliberately corrupted after the ownership snapshot:
P2G must still consume the authoritative record. This checks the consumer, not
just the presence of its buffer binding.

The [live integration receipt](fluid-owned-particles-validation.json) records
seven live cases (353 frames, including one solver-only affine frame), seven
hardware transfer cases, 205 JavaScript tests and seven native tests. Short,
inlet and mixed-grid A/B runs have byte-identical captured raw lighting, guides
and caustics; displayed scenes are identical outside the wall-clock FPS widget.
These are final-frame snapshot comparisons, not a complete temporal-sequence or
performance claim. Existing coupled and launcher regressions also pass.

## Transaction validation

Build `NVMatrixEngineExchangeGpuTest` and run it with the built lab runtime directory
as its only argument. `compile-fluid-exchange.ps1 -OutputDir <runtime>/shaders`
builds its seven original kernels; the normal shader build invokes it too.

The hardware fixtures retire actual particles, borrow the existing FP64 implicit
solver to move their grid-owned quantities, then restore non-dyadic fractional
particle quantities. Independent snapshots check all seven stages, including
unchanged-cache and applied-velocity-increment cases. Tests cover complete
restoration, insufficient slots, absent/invalid sites, capacity refusal, a
closing grid cell and protected future emitter IDs. Failed cell transactions
retain grid ownership. Invalid CPU API ordering/bounds are rejected before
dispatch. Shader FP64 finiteness checks inspect exponent bits, avoiding HLSL's
implicit conversion to FP32 for `isfinite(double)`.
Every live seed/restored sample is also checked against deliberately stale
previous-position data at all seven snapshot stages.

The [checkpoint receipt](particle-grid-exchange-validation.json) records seven
hardware cases, 204 JavaScript tests, seven native tests and fresh lab regressions
(405 rendered frames), with source, shader and executable hashes. This is a
correctness checkpoint, not a speed measurement or a D3D12 debug-layer/GBV run.

The current fixtures check mass/linear momentum and translational kinetic
energy, not arbitrary affine/angular transfer, automatic band selection,
whole-scene pressure coupling or optical continuity. Those gates belong to the
subsequent integration steps, not to this isolated component receipt.

## CUDA transport connection — 14 September 2026

`cuda/fluid_cuda_owned_transport.*` now transports this exchange's existing FP64
grid quantities, with convergence/capacity-gated publication and canonical MAC
face-rate restriction. `CudaSharing` can expose both grid inventories through the
existing DX12/CUDA interop layer without changing ordinary particle-only sharing.
The shared test runs the same seven retirement/restoration scenarios and original
independent quantity, energy, motion-history and fractional P2G checks, replacing
only their HLSL transport with CUDA. Maximum joint quantity error is 2.77556e-17.

Twelve separate CUDA transport cases cover circulation, captured high-CFL work,
closing cells, invalid input, overfill, sticky rejection and preservation of
tiny nonzero owners. The MAC-rate test includes odd dimensions and validates
both retained and eliminated fine faces. All six focused memory/initialization/
synchronization sanitizer checks pass without exclusions. See the final checkpoint
in [CUDA_FLUID.md](CUDA_FLUID.md) for executable/source evidence and limits.

The subsequent joint-transfer checkpoint connects these quantities to CUDA P2G,
mass-based pressure support and matched PIC/FLIP return inside the existing
transfer module. It integrates a per-fine-cell volume contribution, with the same
normalized finite-domain basis in both grid transfer directions. A four-step
fixture composes those operators with actual particle advection and the grid
transport above; total-volume error is 8.32667e-17 m³. Twelve new numerical cases
and all nine focused sanitizer checks pass; see [CUDA_FLUID.md](CUDA_FLUID.md)
for final-source evidence and the separate particle-only kernel specialization.

The Solver now stages the actual grid inventory and fine open capacities with
the existing particle ledger, connects its shared pressure failure latch, and
executes prediction/return/transport without substep CPU synchronization. The
coarse combined-capacity audit rejects overfilled trial states. Composed tests
include late-pressure rollback of all 20 shared views, graph parity, external
inventory seeding/reset and grid-only pressure support.

This is not yet live automatic retirement. Capacity-compatible joint phase flux,
moving-geometry interfaces and the canonical rendering field remain unfinished.
Mean-only grid
quantities do not retain arbitrary APIC affine/angular detail, and a diffuse
transported tail is not a full liquid cell for surface reconstruction. The older
HLSL implicit transport's tiny-quantity residual scaling was not changed by these
CUDA checkpoints.

The joint CUDA Solver now uses total-phase geometric reconstruction and bounded
conservative transport instead of the implicit component's infinite dilute tail.
The same plane distributes grid owners into fine transfer children; a varying
ownership fraction in full water is not treated as an air interface. Exact
complete-drain transfers preserve tiny owners without a volume cutoff. See the
geometric checkpoint in [CUDA_FLUID.md](CUDA_FLUID.md) for 153 CUDA cases and fresh
gameplay regressions. This remains static Cartesian support: coupled particle/grid
capacity flux, moving geometry, affine/angular ownership and the canonical
grid-owned rendering field are still required before automatic retirement.

The subsequent full-pool checkpoint adds conservative receiver admission before
geometric flux correction. It handles pressure-rounding overfill without clipping
owned quantities, using bounded GPU conditional iteration. All 157 CUDA cases and
fresh gameplay regressions pass. This qualifies grid-only full-cell transport,
not actual particle endpoint capacity: binning a point sample's entire rest volume
into one cell is still an unresolved physical-support assumption. Coupled phase
transport and canonical grid-owned rendering remain prerequisites for retirement.

The real `FluidCuda` adapter now also accepts joint grid inventory and optional
output-only total-phase/plane resources. Solver publication commits these with
the same guarded frame and existing external fence, without reading them back
through the CPU. A real DX12 seed → CUDA Solver → HLSL probe validates the ABI,
reflected planes, pause/reset and failed-frame preservation. See the 162-case
geometry-publication checkpoint in [CUDA_FLUID.md](CUDA_FLUID.md). These are
non-owning cell descriptors, not a continuous nodal surface; `FluidSurface`/DXR
integration and physical particle/phase coupling remain unfinished.

The next checkpoint connects those descriptors to the production continuous
`FluidSurface` node field and procedural BLAS builder. It integrates neighboring
phase volumes, preserves shared nodes, handles domain walls without pool-volume
shrinkage, and clears material-motion guides on pause. Direct/graph hardware
fixtures and independent clipped-volume checks pass. This is still an opt-in
subsystem path: particle-detail blending and actual camera/photon optical
qualification are required before playable grid ownership or retirement is enabled.

The subsequent DXR fixture now traces that same field/BLAS with production
intersection and shared medium-interface functions. Planar hit geometry, shadow
visibility, refraction, TIR and attenuation pass for both direct and graph CUDA
publication, including reset/rejection and brick-seam rays. This is bounded
geometric/dielectric qualification, not curved caustics, complete camera/photon
rendering or automatic particle-retirement acceptance.

The moving-plane checkpoint now corrects off-lattice phase-surface displacement
and wall-induced normal distortion. The canonical representation combines a
volume-matched trilinear free surface with exact Cartesian-domain clipping shared
by DXR, medium queries and other scalar consumers. Twenty-three manufactured
geometry updates pass oblique/reflected ray, seam, normal and volume oracles;
the matrix now has 167 CUDA cases. This does not establish curved/multisheet
reconstruction, actual particle endpoint capacity, variable-support seams or full
phase caustic/RR acceptance. See [CUDA_FLUID.md](CUDA_FLUID.md) for measured errors,
ordinary-render before/after differences and the unchanged remaining requirements.

## Research constraint

[Ferstl et al., Narrow Band FLIP (2016)](https://research-explorer.ista.ac.at/record/1415)
describes a particle surface band coupled to an Eulerian interior and warns that
naive coupling can introduce energy fluctuations. Its approximate surface-volume
tracking is not evidence of exact conservation for this engine. The ownership
ledger and tests here are original engine-integrated code; whole-scene mass,
momentum, energy, density and optical checks remain separate acceptance gates.
