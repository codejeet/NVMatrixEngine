# Full adaptive-system delivery audit

Source: user attachment `70da98d5-57f3-4946-ac95-220e9febd181/pasted-text-1.txt`
(1286 lines, read in full). The active goal is the **entire** attached specification.
Checkpoints below are not substitutes for its end state. Initial audit: `553cbec`.

| Spec sections | Current evidence / remaining acceptance |
| --- | --- |
| 1, 6, 7 | `FluidComplexity`: GPU brick state, compaction, hysteresis, padding and freeze tested. Consumer work is still mostly uniform. |
| 2–5, 11 | Mass-weighted GPU merge/split consumes importance. Opt-in dormant coarse ownership now removes/restores actual particles and supplies pressure/density/render coverage. General flowing coarse bulk, new fine reseeding, source allocation and error policies remain; this is not yet Narrow Band FLIP. |
| 8–10 | Opt-in mixed 2:1 MAC has actual coarse pressure/face-velocity DOFs, an adjoint coupled operator and compatible flux prolongation. `--fluid-mac-solver=multigrid` now adds GPU MGPCG on this actual mixed operator, with independently audited Galerkin levels, bottom factorization and true residuals. Fine transfer allocation remains dense. Need sparse local fine grids, flowing coarse transport, cross-LOD particle sampling and a lower-overhead pressure schedule before default promotion. |
| 12–13 | Opt-in nested sampling feeds the canonical DXR cache with shared-node constraints, normal-stencil guards, current-input invalidation and fine-reference audits. A geometric interface band plus directional 5×9×5 sampling now coarsens actual calm particle-water surfaces without relaxing tolerances. Dynamic/deep cases still stay fine. Need useful net runtime savings, dynamic-surface adaptation, compressed storage and broader cross-LOD optical acceptance. |
| 14 | Rigid SDF poses/velocities now follow actual fluid substeps, including zero-step render frames and paused placement. Need coherent update-rate LOD and synchronized local substeps; no skipping pressure coupling. |
| 15–16 | Opt-in optical state now combines raw variance, geometry edges, reprojected confidence, motion and actual caustic receivers. It schedules normalized fresh diffuse/NEE samples and pinned RTXDI initial PT candidates before production radiance. Scoped empirical/temporal/paired-profile acceptance passes (`ADAPTIVE_OPTICS.md`); need broader reuse-error policies and more substantial water-room savings. This is not a general proof of unbiased reused transport. |
| 17–18 | Existing spectral photons now populate explicit receiver importance used by camera sample budgets and interface feedback without extra photon traversal. Photon budgets themselves remain uniform; need actual adaptive caustic candidate/work allocation. |
| 19 | RTXDI PT handles diffuse-primary indirect reuse. Need valid reusable caustic/specular paths and dynamic-water validation; photon EMA is not a reservoir. |
| 20 | Optional future guiding interface/distribution support must be explicit; do not label existing aperture sampling as learned guiding. |
| 21–23 | Surface sampling consumes physical disturbances, optical priors and visibility separately from physics LOD. New GPU feedback passes actual pixel optical/receiver error to interface bricks, while brick disturbances promote ray budgets. Physics scores are unchanged. Need broader optical-path coverage, measured downstream work/error gains, caustic budgeting and unified budget control. |
| 24 | Add representative calm/deep pool, moving-solid wake, waterfall and droplet sequences proving local work follows disturbances. |
| 25–26 | GPU-compacted quadratic-support P2G face tiles and globally coupled density tiles now provide indirect simulation work through `FluidWork`; original transfer arithmetic and pressure connections are retained. Still dense transfer allocation, uniform force/extrapolation work and a single direct queue. Need sparse physical grid storage and safe double-buffered async overlap. |
| 27–28 | Actual post-contact density error triggers a bounded extra GPU-indirect projection; nonzero density-stencil tiles now compact its iteration work. Need stable timing-budget feedback and replaceable error estimators, not only fixed quality settings. |
| 29 | Requested-LOD/importance, bulk and actual particle-mass views exist; F11 shows actual coarse MAC cells/T boundaries, F10 forces fine resolution, F12 shows actual P2G/density work coverage. Optical F4/F5 add variance, temporal, confidence, ray-budget and receiver views/freeze. Need active-band/pressure-hierarchy/reservoir overlays. |
| 30 | Frame, fluid, surface, BLAS, photons, camera and reuse timings exist. `FluidWork` adds classification/P2G/G2P/density timing and actual/capacity tile counts. Optics adds classifier time, actual camera/reuse rays, sample histograms and interface feedback counts. Need bin/SDF/intersection timing and complete per-LOD work counters. |
| 31 | Must prove mass, momentum, incompressibility, collision and surface continuity under LOD transitions, including long runs and optical temporal checks. Passive global mass tests do not prove this. |
| 32 | Track all 12 phases through executable integration and acceptance, not infrastructure-only status. |
| 33–35 | Preserve the original moving-complexity objective, existing engine abstractions and original integrated code. Inspect/build/run after meaningful changes. |

## Execution dependencies

Current carrier-capacity experiment (`FLUID_CARRIER_PROJECTION.md`): an opt-in
GPU nonlinear fraction/potential solve removes a false conservative-bound
rejection and passes analytical hardware correction/reversal/closure fixtures.
Immutable every-substep audits now distinguish certificate failures from actual
overfill. The moving room still fails: almost-full bulk loses physical pressure
support and a protected air component cannot evacuate incoming liquid. Real
pressure/capacity activation must be coupled before flowing ownership; neither
threshold relaxation nor this air-only correction establishes that requirement.

The next opt-in mode `--fluid-bulk-coupled` (`FLUID_CAPACITY_MAC.md`) connects
capacity correction before G2P to the actual mixed cut-MAC operator, reuses its
multigrid hierarchy, and applies one canonical flux to particles and inventory.
Every-substep snapshots audit final physical divergence, including swept
solid volume, separately from phase/mass bounds. The precision follow-up removes
redundant liquid/air pressure coordinates, includes internal-face stabilization,
requires a signed phase supersolution, and uses FP64 resident volume/momentum
and exact coarse restriction of fine geometric volumes. Coupled paused boundaries
retain their last transported pose until a real substep can process motion.
The 14-case coupled suite now passes (933 rendered frames), including emitters,
moving-solid closure, pause/single-step/reset and adaptive optics. See
`capacity-precision-validation.json` for exact source/binary/capture hashes.
This is **not** flowing particle/grid ownership, geometric VOF, sparse physical
storage, temporal image-quality acceptance or default promotion. Pressure
scheduling cost remains much too high; neither these bounded tests nor the
existence of a coupled API completes sections 8–10 or 31. The scheduling follow-up
uses one borrowed V cycle per outer iteration instead of four, with identical
convergence/bounds gates and a selectable four-cycle reference. Its separate raw
A/B and conservation receipt is `capacity-schedule-validation.json`; a lower
inner-work budget is not sparse physical storage or flowing ownership.

The next [flowing-ownership integration](FLUID_FLOWING_OWNERSHIP.md) introduces
explicit FP64 particle/grid quantities and real GPU retire/restore transactions.
The component borrows existing implicit transport in hardware fixtures, including
fractional mass and closing cells. `--fluid-owned-particles` now connects its
birth/velocity lifecycle, fractional bin mass and authoritative P2G to the live
solver. Automatic grid ownership, joint source allocation, band policy,
affine/error coupling and coarse-owned canonical surface coverage remain. This is not a new
playable Narrow Band FLIP mode or completion of sections 3/10/11/31.

1. Weighted particle contract across transfer/density/capillarity/reconstruction;
   conservative importance-driven merge/split with GPU allocation and validation.
2. Geometric bulk liquid/solid capacities and authoritative coarse MAC support;
   narrow-band grid/particle ownership and fine/coarse pressure/momentum coupling.
3. Sparse indirect fine work, multilevel pressure and actual LOD boundary checks.
4. Adaptive canonical surface sampling with shared boundary nodes and DXR bounds.
5. Optical error and receiver importance, adaptive rays, valid specular-path reuse.
6. Bidirectional feedback, multirate scheduling, budget control and profiler/views.
7. Requirement-by-requirement scene/performance/stability audit of all sections.

The flowing-bulk prerequisite now has an opt-in geometric cut-cell layer
(`FLUID_CUT_CELLS.md`): sampled solid cell capacities, matching shared face
apertures, conservative 2:1 restriction, substep endpoint history and independent
GPU audits. It also measures passive bulk inventory against actual open capacity.
This does not yet make bulk transport pressure-compatible, supply liquid volume
fractions or move ownership away from particles. Those dependencies remain open.

The cut-pressure follow-up (`FLUID_CUT_PRESSURE.md`) now connects those capacities
to the real mixed MAC solve: weighted shared-face fluxes, explicit physical
volumes/Dirichlet weights, swept-boundary sources, precise physical pressure
targets and a GPU-resident canonical volume-flux buffer. Independent physical
face assembly and Galerkin checks remain separate from particle acceptance.
Partial-cell density support and bounded, error-triggered nonlinear repair are
also connected. At that checkpoint the unresampled room exceeded the legacy
peak-density comparison; the follow-up below now passes that scoped gate.
This remains opt-in, not a default-promotion or full-conservation checkpoint.
Coarse liquid fractions, compatible bulk support/advection, source allocation
and conservative particle/grid exchange remain the next ownership dependencies.

The boundary-kernel follow-up (`FLUID_DENSITY_KERNEL.md`) replaces voxel-uniform
missing-solid support with an integrated B-spline measure of the actual sampled
cut geometry. Local input fingerprints restrict expensive recomputation to
changed interface neighborhoods; unchanged regions reuse their existing values.
This is boundary reconstruction work adaptation, not sparse physical allocation
or a replacement for conforming particle advection. Kernel accuracy, cached/full
optical parity, particle-density acceptance and runtime cost are checked
separately. The ordinary room and resampled room now pass their original
density/surface-volume comparison gates; cached/full fixed-point photon captures
match exactly in all ten channels. Camera/rolling temporal preservation also
passes. Raw room timing is still about 21.6 ms, and kernel caching saves only a
small fraction of total frame time; broader physical conservation and runtime
acceptance remain open, not inferred from these bounded cases.

The projected-bulk follow-up (`FLUID_BULK_PROJECTED.md`) now consumes the actual
canonical FP64 shared pressure flux and matching current/previous open volumes,
instead of extrapolated point velocities and full-box capacity. Source momentum
is weighted by measured fine open volume. Independent snapshots check flux and
capacity restriction, force sources and conservative donor transport. This closes
a concrete producer/consumer mismatch but does not make the replica's liquid
support authoritative: initialization/inlet overfill, pressure-support divergence,
cell closure, bounded phase fluxes and grid/particle handoff remain open. No
interior particles are removed by this mode and no adaptive speedup is implied.

The source-admission follow-up (`FLUID_SOURCE_ADMISSION.md`) separates resident
bulk from pending initial/inlet requests. GPU aperture-connected transactions
admit only available capacity and retain unplaced mass/momentum explicitly.
Independent initial and live-source snapshots check bounded admission and
conservation; existing advection excess is neither clipped nor repaired by the
allocator. Initial placement, not full flowing ownership, is addressed. Pending
resolution, boundary component-aware placement, bounded phase advection/pressure
support and conservative particle/grid handoff still precede ownership promotion.

The phase-flux follow-up (`FLUID_PHASE_FLUX.md`) adds opt-in receiver limiting
of shared liquid transfers, preserving donor positivity and mass/momentum
accounting without clipping cell inventory. Already feasible full-cell
circulation is preserved. Every-substep bounds/convergence and independent
snapshot replay are separate from render isolation. Existing capacity deficits
from moving solids remain explicit; limiting phase transfers is not a substitute
for compatible free-surface pressure support, swept closure and conservative
grid/particle ownership exchange. Full adaptive-system acceptance remains open.

The time-centered cut-pressure follow-up (`FLUID_CUT_TIME.md`) retains pressure
support and time-integrated shared apertures for cells that disappear during a
moving-solid substep. Endpoint collision/render geometry and phase capacities
remain separate. Independent geometry/operator audits and closing-cell snapshot
counters test the actual pressure consumer. This is not complete swept-solid
geometry or closing-cell transport: explicit donor limits, particle-only liquid
support and conservative flowing ownership still need work. No default change,
raw-frame speedup or full adaptive delivery is inferred from this prerequisite.

The filled-bulk pressure follow-up (`FLUID_BULK_PRESSURE.md`) connects admitted
full coarse interiors to actual MAC liquid classification, missing face velocity
and capillary occupancy. Positional density correction retains the same interior
connectivity without adding bulk mass to the measured particle/solid kernel RHS.
Existing particle transfers are preserved, and the
coupled pressure flux still drives conservative bulk transport. This addresses
missing pressure coverage without turning fractional transport tails into full
liquid or changing mass ownership. Closing-cell donor residue, geometric
free-surface fractions, conservative moving grid/particle exchange and sparse
physical allocation remain open. The independent coverage audit distinguishes
active final-substep snapshots from idle frames and retains reset history.
Its physical paired comparisons pass, but the strict temporal-preservation gate
remains open (static brightness -1.382%, top-down raw differences +5.236%). All
RR difference changes remain within 5%; this does not waive the other failures
or justify enabling the mode in default launchers.

No section is marked complete by a narrower test. The goal remains active until
the implemented runtime and scoped validation establish the entire requested state.

The flowing-band follow-up (`FLUID_FLOWING_OWNERSHIP.md`) adds a GPU error policy,
current/swept boundary protection, padded hysteresis and collider-validated
restoration sites. Hardware tests now retire translating particles, transport
their actual grid-owned quantities, re-bin on the GPU, preserve joint coverage
and restore or retain ownership transactionally. This is not yet a live NBFLIP
solver: joint MAC/phase coupling, finite-allocation interface transport and the
canonical optical surface remain open. Default launchers are unchanged.

The implicit-transport follow-up (`FLUID_BULK_IMPLICIT.md`) removes the explicit
donor cap in a separate opt-in path. Endpoint capacity and canonical shared
pressure flux define a backward-Euler volume/momentum solve. Partially filled
disappearing cells retain temporary pressure escape paths; the room placement
test exposed the missing rows. Independent matrix, balance and kinetic-energy
audits are separate from every-substep GPU convergence/positivity checks. This
addresses closing-cell residue without deleting inventory. It does not guarantee
the free-surface upper bound: local overfill remains observable. Compatible
bounded interface transport, conservative moving ownership exchange and the
other full-spec dependencies remain open. No default or performance claim follows.

The dormant-owner moving-solid peak-density regression now passes its unchanged
pre-correction gate and a new post-step gate after substep-aligned boundaries,
contact/density ordering, and error-triggered repair (`FLUID_BOUNDARIES.md`).
This is scoped correctness progress, not completion of the entire adaptive
system. Default launchers retain the particle ownership path; shared fluid
boundaries and density projection also benefit from these shared fixes.

The mixed-MAC impact precision floor is addressed with FP64 pressure accumulation,
true residuals and final projection, while the hierarchy/Krylov work stays FP32.
Small coarse hierarchies now have a single-group schedule. The precise path's
room-density regression required 120 positional sweeps; exact paired-tile
updates contain dispatch overhead. These are pressure/density improvements,
not completion of sparse flowing bulk or adaptive optics. Every-substep
convergence and independent scheduler comparisons are now acceptance gates.

The compact-work checkpoint adds same-state dense-reference replay for P2G and
the density operator, with exact GPU comparisons across all audited substeps and
independent final-substep CPU coverage checks. Ordered recycled-slot allocation
closes a reproducibility gap in the existing deterministic-bin validation mode;
normal play retains parallel allocation. This remains an execution-scheduling
milestone, not sparse physical allocation or general Narrow Band FLIP.

The nested-surface checkpoint now has same-state scalar/normal/seam audits,
actual mixed-resolution debug captures, caustic parity and camera/rolling
temporal preservation (`FLUID_SURFACE_LOD.md`). The initial camera-only caustic
history regression was fixed with a GPU actual-deformation flag. Cell-shared
input fingerprints remove repeated per-brick particle hashing. Real gameplay
water still stayed fine and paid a small scheduling overhead at that checkpoint.
The follow-up in `FLUID_SURFACE_BAND.md` replaces the scalar-magnitude band with
fine-reference zero-crossing masks and padded optical stencils. Directional
coarsening retains the nonlinear interface-normal profile and now admits real
calm particle-water surfaces under the original error gates. Moving/high-error
water still refines immediately; this is not completion of dynamic adaptive
surfaces, compressed storage, flowing bulk or adaptive optics.

The optical checkpoint now connects actual photon receiver importance and raw
image error to fresh sample budgets, with bidirectional interface-brick feedback
and independent GPU audits. Matched-cap live-room timing improves by 1.44%; this
is not a large speedup or a claim that the higher-quality mode beats the old
one-sample budget. Primary-hit reuse preserves every captured lighting/guide
channel exactly. Guided/reused caustic paths, flowing coarse bulk, sparse physical
allocation, multirate/async scheduling and budget control remain required.
Current-session actual FG presentation validation is also unresolved, including
the non-adaptive baseline; configuration success is not generated-frame evidence.
