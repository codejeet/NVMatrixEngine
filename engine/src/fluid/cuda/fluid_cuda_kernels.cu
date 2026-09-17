#include "fluid_cuda_kernels.h"
#include "fluid_cuda_geometric_transport.h"
#include "fluid_cuda_narrow_band.h"
#include "fluid_cuda_device.cuh"
#include "fluid_cuda_transfer.h"
#include "fluid_cuda_grid.h"
#include "fluid_cuda_collision.h"
#include "fluid_cuda_mac.h"
#include "fluid_cuda_transaction.h"
#include "fluid_cuda_ownership.h"
#include "fluid_cuda_joint.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
constexpr uint32_t colliderCapacity = 16, maxSteps = 16;
constexpr size_t colliderBytes = (maxSteps + 1) * colliderCapacity * sizeof(Collider);
void check(cudaError_t error, const char *where) {
    if (error != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(error));
}
// Only fields actually consumed by simulation kernels are captured. Camera,
// render jitter and inlet counters remain DX12 inputs and cannot rebuild graphs.
Frame simulationFrame(const Frame &f) {
    Frame result{};
    result.minimumCell = f.minimumCell;
    result.maximumRadius = f.maximumRadius;
    result.gravityDt = f.gravityDt;
    result.grid = f.grid;
    result.counts.x = f.counts.x;
    result.counts.w = f.counts.w;
    result.solver = f.solver;
    result.display.z = f.display.z;
    result.display.w = f.display.w;
    result.collision.x = f.collision.x;
    result.material = f.material;
    result.initialMinimum.w = f.initialMinimum.w;
    return result;
}
struct Graph {
    cudaGraphExec_t exec = nullptr;
    State end{};
    size_t nodes = 0, bodyNodes = 0;
    ~Graph() {
        if (exec)
            cudaGraphExecDestroy(exec);
    }
};
struct Completion {
    uint32_t invalid, capped, pressureInvalid, iterations, totalIterations, solves, coarsePeak, fallbacks,
        pageChanges, missing, peakIterations, resident, loopSolves, loopIterations, wakeCells, temporalCells,
        paddedCells, hierarchyBuilds, hierarchyReuses, warmStarts;
    double divergence, solverDivergence;
    NarrowBandMetrics narrow{};
};
static_assert(sizeof(Completion) == 144);
__global__ void completionStatus(MacView m, PressureView p, Completion *out, const uint32_t *failure) {
    if (blockIdx.x || threadIdx.x)
        return;
    if (!m.pool.control) {
        *out = {};
        out->invalid = *failure;
        return;
    }
    *out = {m.pool.control[BrickInvalid],
            p.counts[PressureTotalCapped],
            p.counts[PressureInvalid],
            p.counts[PressureIterations],
            p.counts[PressureTotalIterations],
            p.counts[PressureCalls],
            m.counters[MacCoarsePeak],
            m.counters[MacFallbacks],
            m.pool.control[BrickFrameChanges],
            m.pool.control[BrickMissing],
            p.counts[PressurePeakIterations],
            m.pool.control[BrickResident],
            p.counts[PressureLoopSolves],
            p.counts[PressureLoopIterations],
            m.counters[MacWakeRefinements],
            m.counters[MacTemporalRefinements],
            m.counters[MacPaddedRefinements],
            p.counts[PressureHierarchyBuilds],
            p.counts[PressureHierarchyReuses],
            p.counts[PressureWarmStarts],
            p.scalars[7],
            p.scalars[2]}; // Stored-face flux and pre-storage solver residual.
}
__global__ void narrowCompletion(const NarrowBandMetrics *metrics, Completion *out) {
    out->narrow = *metrics;
}
__global__ void integrate(Frame f, Particle *particles, const uint32_t *failure) {
    if (failure && *failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    Particle p = particles[id];
    if (!p.velocityFlags.w)
        return;
    float dt = f.gravityDt.w;
    float3 position = xyz(p.positionRadius) + xyz(p.velocityFlags) * dt + xyz(f.gravityDt) * (.5f * dt * dt);
    float3 velocity = xyz(p.velocityFlags) + xyz(f.gravityDt) * dt;
    float3 lo = xyz(f.minimumCell) + radiusVector(p.positionRadius.w);
    float3 hi = xyz(f.maximumRadius) - radiusVector(p.positionRadius.w);
    for (int a = 0; a < 3; ++a) {
        if (component(position, a) < component(lo, a)) {
            component(position, a) = component(lo, a);
            component(velocity, a) = fmaxf(component(velocity, a), 0);
        }
        if (component(position, a) > component(hi, a)) {
            component(position, a) = component(hi, a);
            component(velocity, a) = fminf(component(velocity, a), 0);
        }
    }
    setXYZ(p.positionRadius, position);
    setXYZ(p.velocityFlags, velocity);
    particles[id] = p;
}
} // namespace

struct Solver {
    Config config{};
    void *buffers[BufferCount]{};
    Transfer *transfer = nullptr;
    Mac *mac = nullptr;
    Transaction *transaction = nullptr;
    Joint *joint = nullptr;
    NarrowBand *narrow = nullptr;
    uint32_t *failure = nullptr, *ownedFailure = nullptr;
    Ownership owned{};
    GridInventory inventory{};
    Completion *hostCompletion = nullptr, *deviceCompletion = nullptr;
    Collider *deviceColliders = nullptr, *hostColliders = nullptr;
    Collider *activeColliders = nullptr;
    float *mesh = nullptr;
    size_t meshCount = 0;
    cudaEvent_t completion = nullptr;
    bool pending = false, poisoned = false;
    State current{};
    std::array<std::unique_ptr<Graph>, 4> graphs;
    Frame capturedFrame{};
    bool prepared = false;
    Statistics stats{};
    ~Solver() {
        // Ownership API requires the caller to drain its stream before destroy.
        // Also handles every partially constructed allocation without a leak.
        for (auto &g : graphs)
            g.reset();
        destroyTransfer(transfer);
        destroyJoint(joint);
        destroyNarrowBand(narrow);
        destroyTransaction(transaction);
        destroyMac(mac);
        if (ownedFailure)
            cudaFree(ownedFailure);
        if (hostCompletion)
            cudaFreeHost(hostCompletion);
        if (deviceCompletion)
            cudaFree(deviceCompletion);
        if (completion)
            cudaEventDestroy(completion);
        if (deviceColliders)
            cudaFree(deviceColliders);
        if (activeColliders)
            cudaFree(activeColliders);
        if (hostColliders)
            cudaFreeHost(hostColliders);
        if (mesh)
            cudaFree(mesh);
    }
};

Solver *create(const Config &config, void *const (&buffers)[BufferCount], const float *mesh, size_t count,
               const Ownership &owned, const GridInventory &inventory, const SurfaceGeometry &surface,
               void *narrowPrevious) {
    validateOwnership(config, owned);
    const bool joint = validateGridInventory(config, inventory);
    validateSurfaceGeometry(config, inventory, surface);
    if (config.narrowBand != (narrowPrevious != nullptr) ||
        (config.narrowBand && (!config.ownedParticles || !joint || surface[0])))
        throw std::runtime_error("Narrow-band solver requires joint ownership and particle motion storage");
    // Density repair and moving-capacity exchange must consume both owners
    // before those modes can be exposed. Never silently run a particle-only
    // repair on a joint inventory.
    if (joint && (config.ballistic || config.transferTest || config.materialTest ||
                  (!config.narrowBand && config.densityIterations)))
        throw std::runtime_error(
            "Joint CUDA inventory does not yet support particle-only repair/fixture modes");
    if (!config.pressureIterations || config.pressureIterations > 1000 || config.densityIterations > 1000 ||
        !config.viscositySubsteps || config.viscositySubsteps > 100000 || config.transferTest > 4 ||
        config.materialTest > 2 || (count && !mesh) || count > 0xffffffffu ||
        (config.mixedPressure && (config.ballistic || config.transferTest || config.materialTest)) ||
        (config.forcedFinePressure && !config.mixedPressure))
        throw std::runtime_error("Invalid CUDA fluid solver configuration");
    for (auto buffer : buffers)
        if (!buffer)
            throw std::runtime_error("Missing CUDA fluid solver buffer");
    for (size_t i = 0; i < count; ++i)
        if (!std::isfinite(mesh[i]))
            throw std::runtime_error("Nonfinite CUDA mesh SDF asset");
    auto p = std::make_unique<Solver>();
    p->config = config;
    std::copy(std::begin(buffers), std::end(buffers), p->buffers);
    // Validate layout before bounded dimension arithmetic/allocation below.
    bufferBytes(config, Particles);
    if (config.mixedPressure) {
        const uint32_t bricks = ((config.nx + 3) / 4) * ((config.ny + 3) / 4) * ((config.nz + 3) / 4);
        MacConfig mc{config.nx,
                     config.ny,
                     config.nz,
                     config.pressureIterations,
                     std::min(config.pressureBrickCapacity, bricks),
                     config.pressureChangesPerFrame,
                     config.forcedFinePressure,
                     .15f,
                     true,
                     config.cgIterations,
                     config.pressureConditionalGraphs};
        p->mac = createMac(mc);
        p->failure = macView(p->mac).pool.control + BrickInvalid;
        p->stats.mixedPressureBytes = macBytes(p->mac);
    }
    if (config.ownedParticles && !p->failure) {
        check(cudaMalloc(&p->ownedFailure, sizeof(uint32_t)), "CUDA ownership failure latch");
        check(cudaMemset(p->ownedFailure, 0, sizeof(uint32_t)), "CUDA ownership failure initialization");
        check(cudaStreamSynchronize(nullptr), "CUDA ownership initialization completion");
        p->failure = p->ownedFailure;
    }
    if (p->failure) {
        p->transaction =
            createTransaction(config, buffers, p->failure, owned, inventory, surface, narrowPrevious);
        transactionBuffers(p->transaction, p->buffers);
        p->owned = transactionOwnership(p->transaction);
        p->inventory = transactionGridInventory(p->transaction);
        p->stats.stagingBytes = transactionBytes(p->transaction);
        check(cudaMallocHost(&p->hostCompletion, sizeof(Completion)), "CUDA pinned pressure status");
        check(cudaMalloc(&p->deviceCompletion, sizeof(Completion)), "CUDA deferred pressure status");
        *p->hostCompletion = {};
    }
    p->transfer = createTransfer(config, p->buffers, p->owned,
                                 joint ? GridOwnership{p->inventory[GridOwnedQuantity],
                                                       p->inventory[GridFineCapacity], p->failure, true}
                                       : GridOwnership{});
    if (joint) {
        p->joint = createJoint(config, p->buffers, p->owned, p->inventory, p->failure);
        p->stats.gridTransportBytes = jointBytes(p->joint) + gridOwnershipTransferBytes(p->transfer);
    }
    if (config.narrowBand) {
        p->narrow =
            createNarrowBand(config, p->buffers, p->owned, p->inventory, gridOwnershipPhase(p->transfer),
                             transactionNarrowPrevious(p->transaction), p->failure);
        p->stats.gridTransportBytes += narrowBandBytes(p->narrow);
    }
    p->meshCount = count;
    check(cudaMalloc(&p->deviceColliders, colliderBytes), "CUDA persistent collider slices");
    if (config.graphs)
        check(cudaMalloc(&p->activeColliders, colliderCapacity * sizeof(Collider)),
              "CUDA graph collider endpoint");
    check(cudaMallocHost(&p->hostColliders, colliderBytes), "CUDA pinned collider staging");
    check(cudaEventCreateWithFlags(&p->completion, cudaEventDisableTiming), "CUDA solver ownership event");
    if (count) {
        check(cudaMalloc(&p->mesh, count * sizeof(float)), "CUDA static mesh SDF");
        check(cudaMemcpy(p->mesh, mesh, count * sizeof(float), cudaMemcpyHostToDevice),
              "CUDA mesh SDF upload");
    }
    return p.release();
}
void destroy(Solver *p) noexcept {
    delete p;
}
State state(const Solver *p) {
    if (!p || p->poisoned)
        throw std::runtime_error("Invalid CUDA fluid state");
    return p->transaction ? State{false, 0} : p->current;
}
const Mac *solverMac(const Solver *p) {
    return p ? p->mac : nullptr;
}
const void *solverParticles(const Solver *p) {
    return p ? p->buffers[Particles] : nullptr;
}
const void *solverGridQuantity(const Solver *p) {
    return p ? p->inventory[GridOwnedQuantity] : nullptr;
}
const GeometricTransportMetrics *solverGeometricMetrics(const Solver *p) {
    return p ? jointGeometricMetrics(p->joint) : nullptr;
}
void collect(Solver *p) {
    if (!p || p->poisoned)
        throw std::runtime_error("Invalid CUDA fluid completion");
    if (!p->pending)
        return;
    check(cudaEventQuery(p->completion), "CUDA fluid completion before owner fence");
    p->pending = false;
    if (p->hostCompletion) {
        const auto &c = *p->hostCompletion;
        auto &s = p->stats;
        s.pressureSolves = c.solves;
        s.pressureIterations = c.totalIterations;
        s.pressureLoopSolves = c.loopSolves;
        s.pressureLoopIterations = c.loopIterations;
        s.pressureCaps = c.capped;
        s.coarsePeak = c.coarsePeak;
        s.pressureFallbacks = c.fallbacks;
        s.pressurePeakIterations = c.peakIterations;
        s.pressureHierarchyBuilds = c.hierarchyBuilds;
        s.pressureHierarchyReuses = c.hierarchyReuses;
        s.pressureWarmStarts = c.warmStarts;
        s.pressurePageChanges = c.pageChanges;
        s.pressureBacklog = c.missing;
        s.pressureResident = c.resident;
        s.pressureDivergence = c.divergence;
        s.narrowRetired = c.narrow.retired;
        s.narrowRestored = c.narrow.restored;
        s.narrowDeferred = c.narrow.deferred;
        s.narrowActiveParticles = c.narrow.activeParticles;
        s.narrowGridCells = c.narrow.gridCells;
        s.narrowGridVolume = c.narrow.gridVolume;
        s.narrowParticleVolume = c.narrow.particleVolume;
        // Cell-step counters can wrap during long sessions. The mandatory
        // per-submission collect and bounded grid / maxSteps make each delta
        // smaller than 2^32, so unwrap into the existing 64-bit host statistics.
        s.refinementWakeCells += uint32_t(c.wakeCells - uint32_t(s.refinementWakeCells));
        s.refinementTemporalCells += uint32_t(c.temporalCells - uint32_t(s.refinementTemporalCells));
        s.refinementPaddedCells += uint32_t(c.paddedCells - uint32_t(s.refinementPaddedCells));
        if (c.invalid) {
            ++s.rejectedFrames;
            p->poisoned = true;
            // A failed frame is terminal. Read diagnostics only after its
            // completion fence, never synchronize the healthy simulation path.
            GeometricTransportMetrics transport{};
            if (p->joint)
                check(cudaMemcpy(&transport, jointGeometricMetrics(p->joint), sizeof(transport),
                                 cudaMemcpyDeviceToHost),
                      "CUDA rejected transport diagnostics");
            std::ostringstream geometryDiagnostic;
            if (p->joint && transport.failureStage == GeometricPlaneVolume) {
                double2 phase{};
                double4 plane{};
                check(cudaMemcpy(&phase,
                                 static_cast<const double2 *>(gridOwnershipPhase(p->transfer)) +
                                     transport.failureIndex,
                                 sizeof(phase), cudaMemcpyDeviceToHost),
                      "CUDA rejected phase diagnostic");
                check(cudaMemcpy(&plane,
                                 static_cast<const double4 *>(gridOwnershipPlanes(p->transfer)) +
                                     transport.failureIndex,
                                 sizeof(plane), cudaMemcpyDeviceToHost),
                      "CUDA rejected plane diagnostic");
                geometryDiagnostic << std::setprecision(17) << ", phase=" << phase.x << '/' << phase.y
                                   << ", plane=" << plane.x << ',' << plane.y << ',' << plane.z << ','
                                   << plane.w;
            }
            throw std::runtime_error(std::string(p->mac ? "CUDA mixed-pressure frame rejected ("
                                                        : "CUDA ownership frame rejected (") +
                                     (c.capped ? "iteration cap" : "invalid pressure/field state") +
                                     ", last iterations=" + std::to_string(c.iterations) +
                                     ", completed frames=" + std::to_string(s.completedFrames) +
                                     ", face divergence=" + std::to_string(c.divergence) +
                                     ", solver divergence=" + std::to_string(c.solverDivergence) +
                                     ", transport stage=" + std::to_string(transport.failureStage) +
                                     ", transport index=" + std::to_string(transport.failureIndex) +
                                     ", transport capped=" + std::to_string(transport.capped) +
                                     ", narrow invalid=" + std::to_string(c.narrow.invalid) +
                                     ", narrow stage=" + std::to_string(c.narrow.reserved) + ", retired=" +
                                     std::to_string(c.narrow.retired) + geometryDiagnostic.str() +
                                     "): no fields published; rebuild the fluid subsystem");
        }
    }
    ++p->stats.completedFrames;
}
Statistics statistics(const Solver *p) {
    if (!p)
        throw std::runtime_error("Invalid CUDA statistics");
    return p->stats;
}

static Frame validatedFrame(const Solver *p, const void *rawFrame) {
    if (!p || !rawFrame || p->poisoned)
        throw std::runtime_error("Invalid CUDA fluid frame");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto &c = p->config;
    if (f.grid.x != c.nx || f.grid.y != c.ny || f.grid.z != c.nz ||
        uint64_t(c.nx) * c.ny * c.nz != f.grid.w || f.counts.x != c.capacity ||
        f.counts.w != c.transferTest || f.material.w != float(c.materialTest) ||
        f.material.z != float(c.viscositySubsteps) || f.display.z ||
        f.display.w != (c.ownedParticles ? 2u : 0u) ||
        (c.ownedParticles && (!std::isfinite(f.initialMinimum.w) || f.initialMinimum.w <= 0)) ||
        f.collision.x > colliderCapacity)
        throw std::runtime_error("CUDA baseline frame/configuration mismatch or unsupported ownership mode");
    for (auto v : {f.minimumCell, f.maximumRadius, f.gravityDt, f.solver, f.material})
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.w))
            throw std::runtime_error("Nonfinite CUDA fluid frame");
    if (f.minimumCell.w <= 0 || f.gravityDt.w <= 0 || f.solver.x <= 0 || f.solver.w <= 0 || f.solver.y < 0 ||
        f.solver.y > 1 || (f.solver.z != 0 && f.solver.z != 1) || f.maximumRadius.w <= 0 ||
        f.material.x < 0 || f.material.x > .15001f || f.material.y < 0)
        throw std::runtime_error("Invalid CUDA fluid physical parameters");
    if (p->joint && !p->narrow && (f.collision.x || f.material.y > 0))
        throw std::runtime_error(
            "Joint CUDA inventory requires domain-only boundaries and no particle-only capillarity");
    return f;
}

// Exactly the same body is used by direct launches and captured replay.
// Boundary baking and endpoint uploads stay outside the graph; collision nodes
// use a persistent active endpoint populated in stream order before replay.
static void body(Solver *p, cudaStream_t stream, Frame f, const Collider *endpoint, bool binsCurrent,
                 bool rebuildOnly) {
    const auto &c = p->config;
    auto grid = [&](GridStage stage, bool conditional = false) {
        enqueueGrid(stream, p->buffers, &f, stage, p->current.pressureIndex, conditional, p->failure,
                    p->owned[OwnedCellMass], p->narrow ? p->inventory[GridOwnedQuantity] : nullptr);
    };
    auto contact = [&](bool conditional = false) {
        if (f.collision.x)
            enqueueCollision(stream, p->buffers, &f, CollisionStage::Collide, endpoint, p->mesh, conditional,
                             p->failure);
    };
    auto bin = [&] {
        enqueueTransfer(p->transfer, stream, &f, TransferStage::Bin, nullptr, p->failure);
        if (p->joint)
            enqueueJointCapacity(p->joint, stream, &f);
    };
    auto finishOwnership = [&] {
        if (c.ownedParticles)
            enqueueOwnership(stream, p->buffers[Particles], p->owned, &f, OwnershipStage::VelocityDelta,
                             p->failure);
    };
    auto transfer = [&](TransferStage stage) {
        enqueueTransfer(p->transfer, stream, &f, stage, p->buffers[Faces], p->failure);
    };
    auto swapFaces = [&] {
        std::swap(p->buffers[Faces], p->buffers[Scratch]);
        p->current.facesSwapped = !p->current.facesSwapped;
    };
    auto viscosity = [&] {
        for (uint32_t i = 0; i < c.viscositySubsteps; ++i) {
            grid(GridStage::Viscosity);
            swapFaces();
        }
    };
    auto solve = [&](uint32_t iterations, bool conditional = false) {
        p->current.pressureIndex = 0;
        for (uint32_t i = 0; i < iterations; ++i) {
            grid(GridStage::Jacobi, conditional);
            p->current.pressureIndex = 1 - p->current.pressureIndex;
        }
    };
    auto p2g = [&] {
        if (!binsCurrent || p->narrow)
            bin();
        if (p->narrow && !rebuildOnly) {
            // Restore immediately around newly changed boundaries. Dwell time
            // advances only once, at the end of a completed physical substep.
            exchangeNarrowBand(p->narrow, stream, &f, endpoint, p->mesh, c.capacity, false, false);
            bin(); // The old ranges contain retired/recycled IDs.
        }
        transfer(TransferStage::ToGrid);
        // Classify clears both banks. Canonicalize this otherwise irrelevant
        // binding so pressure parity cannot multiply graph variants.
        p->current.pressureIndex = 0;
        grid(GridStage::Classify);
        if (c.materialTest) {
            if (c.materialTest == 1) {
                grid(GridStage::MaterialFixture);
                viscosity();
            } else {
                grid(GridStage::SurfaceColor);
                grid(GridStage::SurfaceCurvature);
            }
            return;
        }
        if (c.transferTest == 1 || c.transferTest == 2 || c.transferTest == 4)
            return;
        grid(GridStage::Forces);
        if (!c.ballistic && !c.transferTest) {
            if (f.material.x > 0)
                viscosity();
            if (f.material.y > 0) {
                grid(GridStage::SurfaceColor);
                grid(GridStage::SurfaceCurvature);
            }
        }
        grid(GridStage::Divergence);
        if (p->mac)
            p->current.pressureIndex = enqueueMac(p->mac, stream, p->buffers, &f, p->owned[OwnedCellMass]);
        else {
            solve(c.pressureIterations);
            grid(GridStage::Project);
            grid(GridStage::Measure);
        }
    };
    auto g2p = [&] {
        if (!c.transferTest)
            for (uint32_t i = 0; i < 4; ++i) {
                grid(GridStage::Extrapolate);
                swapFaces();
            }
        transfer(TransferStage::ToParticles);
    };
    if (rebuildOnly) {
        p2g();
        if (c.transferTest == 4)
            g2p();
        finishOwnership();
        return;
    }
    if (c.ballistic) {
        integrate<<<(c.capacity + 127) / 128, 128, 0, stream>>>(
            f, static_cast<Particle *>(p->buffers[Particles]), p->failure);
        check(cudaGetLastError(), "CUDA ballistic integration");
        finishOwnership();
        return;
    }
    p2g();
    g2p();
    if (p->joint)
        enqueueJointTransport(p->joint, p->transfer, stream, &f, p->buffers[Faces]);
    contact();
    if (!c.transferTest && c.densityIterations) {
        bin();
        grid(GridStage::DensityGather);
        solve(c.densityIterations);
        grid(GridStage::DensityDisplace);
    }
    contact();
    bin();
    if (!c.transferTest && c.densityIterations) {
        grid(GridStage::DensityClearArguments);
        grid(GridStage::DensityGatherAdaptive);
        grid(GridStage::DensityPrepareArguments);
        solve(c.densityIterations, true);
        grid(GridStage::DensityDisplace, true);
        contact(true);
        // Still unconditional, exactly like the validated direct CUDA baseline.
        bin();
    }
    finishOwnership();
    if (p->narrow) {
        exchangeNarrowBand(p->narrow, stream, &f, endpoint, p->mesh, c.capacity);
        bin();
    }
}

void prepare(Solver *p, const void *rawFrame) {
    if (!p || !rawFrame || p->poisoned)
        throw std::runtime_error("Invalid CUDA graph preparation");
    collect(p);
    if (!p->config.graphs)
        return;
    const Frame f = simulationFrame(validatedFrame(p, rawFrame));
    if (p->prepared && !std::memcmp(&p->capturedFrame, &f, sizeof(f)))
        return;
    const auto start = std::chrono::steady_clock::now();
    cudaStream_t capture = nullptr;
    cudaGraph_t graph = nullptr;
    bool capturing = false;
    void *savedFaces = p->buffers[Faces], *savedScratch = p->buffers[Scratch];
    const State saved = p->current;
    std::array<std::unique_ptr<Graph>, 4> replacement;
    try {
        check(cudaStreamCreateWithFlags(&capture, cudaStreamNonBlocking), "CUDA graph capture stream");
        for (uint32_t i = 0; i < replacement.size(); ++i) {
            const bool swapped = (i & 1) != 0;
            p->buffers[Faces] = swapped == saved.facesSwapped ? savedFaces : savedScratch;
            p->buffers[Scratch] = swapped == saved.facesSwapped ? savedScratch : savedFaces;
            p->current = {swapped, 0};
            check(cudaStreamBeginCapture(capture, cudaStreamCaptureModeThreadLocal),
                  "Begin CUDA substep capture");
            capturing = true;
            const auto previousBodyNodes =
                macPressureCapturedBodyNodes(p->mac) + jointCapturedBodyNodes(p->joint);
            body(p, capture, f, p->activeColliders, (i & 2) != 0, false);
            const auto ended = cudaStreamEndCapture(capture, &graph);
            capturing = false;
            check(ended, "End CUDA substep capture");
            replacement[i] = std::make_unique<Graph>();
            auto &g = *replacement[i];
            check(cudaGraphGetNodes(graph, nullptr, &g.nodes), "CUDA graph node count");
            g.bodyNodes = size_t(macPressureCapturedBodyNodes(p->mac) + jointCapturedBodyNodes(p->joint) -
                                 previousBodyNodes);
            g.nodes += g.bodyNodes;
            check(cudaGraphInstantiate(&g.exec, graph, 0), "Instantiate CUDA substep graph");
            g.end = p->current;
            check(cudaGraphDestroy(graph), "Release CUDA graph template");
            graph = nullptr;
        }
        check(cudaStreamDestroy(capture), "Release CUDA capture stream");
        capture = nullptr;
    } catch (...) {
        if (capturing)
            cudaStreamEndCapture(capture, &graph);
        if (graph)
            cudaGraphDestroy(graph);
        if (capture)
            cudaStreamDestroy(capture);
        p->buffers[Faces] = savedFaces;
        p->buffers[Scratch] = savedScratch;
        p->current = saved;
        // Nothing was executed or published; existing state remains intact.
        throw;
    }
    p->buffers[Faces] = savedFaces;
    p->buffers[Scratch] = savedScratch;
    p->current = saved;
    p->graphs = std::move(replacement);
    p->capturedFrame = f;
    p->prepared = true;
    p->stats.graphBuilds += p->graphs.size();
    p->stats.graphNodes = p->stats.graphBodyNodes = 0;
    for (const auto &g : p->graphs) {
        p->stats.graphNodes += g->nodes;
        p->stats.graphBodyNodes += g->bodyNodes;
    }
    p->stats.preparationMs +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void enqueue(Solver *p, void *rawStream, const void *rawFrame, const void *rawColliders, uint32_t steps,
             bool moving, bool rebuild, bool reset) {
    if (!p || !rawStream || !rawFrame || steps > maxSteps || p->poisoned)
        throw std::runtime_error("Invalid CUDA fluid submission");
    const Frame f = validatedFrame(p, rawFrame);
    const auto &c = p->config;
    if (f.collision.x && !rawColliders)
        throw std::runtime_error("Missing CUDA collider metadata");
    if (p->joint && !p->narrow && moving)
        throw std::runtime_error("Joint CUDA inventory requires static capacity throughout a frame");
    const Frame captured = simulationFrame(f);
    if (c.graphs && (!p->prepared || std::memcmp(&p->capturedFrame, &captured, sizeof(captured))))
        throw std::runtime_error("Prepare CUDA graphs before the DX12 ownership handoff");
    // Reject malformed static mesh descriptors before any GPU submission. Only
    // bounded collider metadata crosses the CPU; particle/grid state stays GPU-side.
    const auto *colliders = static_cast<const Collider *>(rawColliders);
    for (uint32_t s = 0; s <= (moving ? steps : 0); ++s)
        for (uint32_t i = 0; i < f.collision.x; ++i) {
            const auto &v = colliders[s * colliderCapacity + i];
            const float *values = reinterpret_cast<const float *>(&v);
            for (uint32_t j = 0; j < 36; ++j)
                if (!std::isfinite(values[j]))
                    throw std::runtime_error("Nonfinite CUDA collider");
            if (v.extentType.w < 0 || v.extentType.w > 5 || v.extentType.w != std::floor(v.extentType.w))
                throw std::runtime_error("Unsupported CUDA collider type");
            if (v.extentType.w == 5) {
                auto d = v.meshDimensions;
                if (d.x < 2 || d.y < 2 || d.z < 2 || d.x > p->meshCount || d.y > p->meshCount ||
                    d.z > p->meshCount || v.meshMinimumSpacing.w <= 0 || uint64_t(d.x) * d.y > p->meshCount ||
                    uint64_t(d.x) * d.y * d.z + d.w > p->meshCount)
                    throw std::runtime_error("CUDA mesh collider leaves its SDF asset");
            }
        }
    collect(p);
    auto stream = static_cast<cudaStream_t>(rawStream);
    // Any exception after this point invalidates the partially advanced solver.
    // The interop transaction drains it without waiting on an unqueued signal.
    p->poisoned = true;
    if (p->transaction) {
        transactionBuffers(p->transaction, p->buffers);
        p->current = {false, 0};
        snapshotTransaction(p->transaction, stream);
        // One frame's page budget is shared by all of its substeps.
        if (p->mac)
            beginMacFrame(p->mac, stream, reset);
    }
    if (c.ownedParticles)
        enqueueOwnership(stream, p->buffers[Particles], p->owned, &f, OwnershipStage::Validate, p->failure);
    if (p->narrow) {
        if (reset)
            resetNarrowBand(p->narrow, stream, &f);
        beginNarrowBandFrame(p->narrow, stream, &f);
    }
    if (f.collision.x) {
        const size_t bytes = (moving ? steps + 1 : 1) * colliderCapacity * sizeof(Collider);
        std::memcpy(p->hostColliders, rawColliders, bytes);
        check(cudaMemcpyAsync(p->deviceColliders, p->hostColliders, bytes, cudaMemcpyHostToDevice, stream),
              "CUDA collider endpoint upload");
    }
    const Collider *endpoint = p->deviceColliders;
    auto bake = [&] {
        enqueueCollision(stream, p->buffers, &f, CollisionStage::BakeSolids, endpoint, p->mesh, false,
                         p->failure);
    };
    auto publishEndpoint = [&] {
        if (c.graphs && f.collision.x)
            check(cudaMemcpyAsync(p->activeColliders, endpoint, colliderCapacity * sizeof(Collider),
                                  cudaMemcpyDeviceToDevice, stream),
                  "CUDA graph active collider endpoint");
    };
    enqueueGrid(stream, p->buffers, &f, GridStage::DensityClearArguments, p->current.pressureIndex, false,
                p->failure, p->owned[OwnedCellMass]);
    bake();
    if (!moving)
        publishEndpoint();
    for (uint32_t step = 0; step < steps; ++step) {
        if (moving) {
            endpoint = p->deviceColliders + (step + 1) * colliderCapacity;
            bake();
            publishEndpoint();
        }
        if (c.graphs) {
            const auto &g = *p->graphs[(step ? 2 : 0) + (p->current.facesSwapped ? 1 : 0)];
            check(cudaGraphLaunch(g.exec, stream), "Replay CUDA substep graph");
            if (g.end.facesSwapped != p->current.facesSwapped)
                std::swap(p->buffers[Faces], p->buffers[Scratch]);
            p->current = g.end;
            ++p->stats.graphReplays;
        } else {
            body(p, stream, f, endpoint, step != 0, false);
            ++p->stats.directSteps;
        }
    }
    if ((c.ballistic && steps) || (!steps && rebuild))
        body(p, stream, f, endpoint, false, true);
    if (p->joint && !steps && !rebuild) {
        // Pausing skips integration, not validation of newly handed-off owners.
        enqueueTransfer(p->transfer, stream, &f, TransferStage::Bin, nullptr, p->failure);
        enqueueJointCapacity(p->joint, stream, &f);
    }
    if (p->transaction) {
        publishTransaction(p->transaction, stream, p->buffers, p->current.pressureIndex,
                           {const_cast<void *>(gridOwnershipPhase(p->transfer)),
                            const_cast<void *>(gridOwnershipPlanes(p->transfer))});
        completionStatus<<<1, 1, 0, stream>>>(p->mac ? macView(p->mac) : MacView{},
                                              p->mac ? macPressureView(p->mac) : PressureView{},
                                              p->deviceCompletion, p->failure);
        if (p->narrow) {
            measureNarrowBand(p->narrow, stream, &f);
            narrowCompletion<<<1, 1, 0, stream>>>(narrowBandMetrics(p->narrow), p->deviceCompletion);
        }
        check(cudaGetLastError(), "CUDA pressure completion diagnostics");
        check(cudaMemcpyAsync(p->hostCompletion, p->deviceCompletion, sizeof(Completion),
                              cudaMemcpyDeviceToHost, stream),
              "CUDA deferred pressure diagnostics");
    }
    check(cudaEventRecord(p->completion, stream), "CUDA solver completion record");
    p->pending = true;
    p->poisoned = false;
}
} // namespace lab::cuda_fluid
