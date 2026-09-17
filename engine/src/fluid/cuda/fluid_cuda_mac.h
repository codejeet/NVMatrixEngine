#pragma once
#include "fluid_cuda_bricks.h"
#include "fluid_cuda_kernels.h"
#include "fluid_cuda_pressure.h"

namespace lab::cuda_fluid {
struct RefinementConfig {
    // Importance is dimensionless. Thresholds refer to resolved coarse-cell
    // velocity variation and error relative to the advected previous sample.
    float velocityScale = .5f, velocityDifference = .08f;
    float relativeVelocityError = .08f, fractionError = .1f;
    float responseSeconds = .025f, wakeSeconds = .15f, quietSeconds = 8.f / 120;
    float demoteThreshold = .5f, promoteThreshold = 1.f;
};
enum RefinementFlags : uint32_t {
    RefinementValid = 1,
    RefinementSurfaceGuard = 2,
    RefinementMovingSolid = 4
};
struct RefinementState {
    float vx, vy, vz, liquidFraction;
    float importance, filteredImportance, wakeSeconds;
    float velocityError, fractionError;
    uint32_t flags, history;
    // Accumulate physical dwell without FP32 summation changing the decision
    // when the same interval is divided into smaller simulation substeps.
    double quietSeconds;
};
static_assert(sizeof(RefinementState) == 56);
struct MacConfig {
    uint32_t nx = 0, ny = 0, nz = 0, iterations = 120;
    uint32_t brickCapacity = 512, changesPerFrame = 64;
    bool forcedFine = false;
    float predictionSeconds = .15f;
    bool multigrid = false;
    uint32_t cgIterations = 32;
    bool conditionalGraphs = true;
    RefinementConfig refinement;
};
enum MacCounter : uint32_t {
    MacLeaves,
    MacCoarse,
    MacJunctions,
    MacInvalid,
    MacFallbacks,
    MacSolves,
    MacCoarsePeak,
    MacPromotions,
    MacDemotions,
    MacWakeRefinements,
    MacTemporalRefinements,
    MacPaddedRefinements,
    MacCounterCount
};
struct MacView {
    BrickView pool;
    uint32_t cx, cy, cz, coarseCount;
    uint32_t *map[2], *state[2], *list[2], *history, *counters;
    // Distinct read/write generations prevent neighbor-read/write races.
    // History advances only after a successfully validated projection.
    RefinementState *previousRefinement = nullptr, *nextRefinement = nullptr;
    bool precise = false;
    double *fineSolution = nullptr; // Pressure-owned interleaved FP64 RHS/pressure.
    uint32_t *refinementGuards = nullptr;
    const uint32_t *pressureCacheState = nullptr;
};
struct Mac;
Mac *createMac(const MacConfig &);
void destroyMac(Mac *) noexcept;
MacView macView(const Mac *);
PressureView macPressureView(const Mac *);
size_t macBytes(const Mac *);
uint64_t macPressureCapturedBodyNodes(const Mac *);
void beginMacFrame(Mac *, void *stream, bool reset = false);
// Consumes the existing divergence/stencil and pre-force MAC cache. Produces
// pressure, shared boundary velocities and compatible fine child faces. No P2G,
// G2P, particles, mass or rendering representations are replaced here.
// Capacity/budget deferral runs a complete fine projection: convergence-qualified
// MGPCG in precise mode, original reference Jacobi otherwise. A
// BrickInvalid counter instead poisons the transaction: subsequent writes and
// publication stop. The owner must audit this counter at its completion fence
// and reject the frame before consuming it. The composed Solver additionally
// guards particle advancement and publication of the entire renderer-facing state.
uint32_t enqueueMac(Mac *, void *stream, void *const (&buffers)[BufferCount], const void *frame,
                    const void *cellMass = nullptr);
} // namespace lab::cuda_fluid
