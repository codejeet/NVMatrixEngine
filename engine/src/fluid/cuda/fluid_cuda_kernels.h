#pragma once
#include <cstdint>
#include <cstddef>
#include <array>

namespace lab::cuda_fluid {
enum Buffer : uint32_t {
    Particles,
    Counts,
    Offsets,
    Cursors,
    Indices,
    Faces,
    Pressure0,
    Pressure1,
    Cells,
    Scratch,
    Density,
    Solid,
    Material,
    DensityArguments,
    BufferCount
};
struct Config {
    uint32_t nx, ny, nz, capacity, pressureIterations, densityIterations, viscositySubsteps;
    uint32_t transferTest, materialTest;
    bool ballistic, deterministic;
    bool graphs = false;
    // Isolated opt-in composed solver. Runtime/UI promotion is separately gated.
    bool mixedPressure = false, forcedFinePressure = false;
    uint32_t pressureBrickCapacity = 512, pressureChangesPerFrame = 64, cgIterations = 32;
    bool pressureConditionalGraphs = true;
    bool ownedParticles = false;
    bool narrowBand = false; // live, conservative particle/grid ownership
};
// Optional views of the existing DX12 particle/grid exchange ledger. No second
// mass inventory: quantity.xyz is volume-weighted velocity and .w is volume.
enum OwnershipBuffer : uint32_t {
    OwnedQuantity,
    OwnedReference,
    OwnedCellMass,
    OwnedControl,
    OwnershipBufferCount
};
using Ownership = std::array<void *, OwnershipBufferCount>;
size_t ownershipBytes(const Config &, OwnershipBuffer);
void validateOwnership(const Config &, const Ownership &);
// Optional actual grid inventory, separate from the renderer's bulk cache.
// Both views are snapshotted with the particle ledger; ordinary solvers retain
// their original 14/18 views. Capacity is static during a submitted frame.
enum GridInventoryBuffer : uint32_t { GridOwnedQuantity, GridFineCapacity, GridInventoryBufferCount };
using GridInventory = std::array<void *, GridInventoryBufferCount>;
size_t gridInventoryBytes(const Config &, GridInventoryBuffer);
bool validateGridInventory(const Config &, const GridInventory &);
// Optional renderer outputs, NOT additional mass owners or simulation inputs.
// One element per ceil(fine/2) owner: double2(total liquid m^3, capacity m^3)
// and double4(PLIC normal, liquid-corner intercept). Negative normal components
// reflect the unit-cell coordinate; the intercept is not ordinary n dot x.
// A zero normal explicitly denotes an unresolved homogeneous phase fraction.
enum SurfaceGeometryBuffer : uint32_t { SurfacePhase, SurfacePlane, SurfaceGeometryBufferCount };
using SurfaceGeometry = std::array<void *, SurfaceGeometryBufferCount>;
size_t surfaceGeometryBytes(const Config &, SurfaceGeometryBuffer);
bool validateSurfaceGeometry(const Config &, const GridInventory &, const SurfaceGeometry &);
struct Solver;
struct Mac;
struct State {
    bool facesSwapped;
    uint32_t pressureIndex;
};
Solver *create(const Config &, void *const (&buffers)[BufferCount], const float *mesh, size_t meshCount,
               const Ownership & = {}, const GridInventory & = {}, const SurfaceGeometry & = {},
               void *narrowPreviousPositions = nullptr);
void destroy(Solver *) noexcept;
State state(const Solver *);
struct Statistics {
    uint64_t graphBuilds = 0, graphReplays = 0, directSteps = 0, graphNodes = 0;
    uint64_t graphBodyNodes = 0, pressureLoopSolves = 0, pressureLoopIterations = 0;
    double preparationMs = 0;
    uint64_t stagingBytes = 0, mixedPressureBytes = 0;
    uint64_t gridTransportBytes = 0;
    uint64_t completedFrames = 0, rejectedFrames = 0, pressureSolves = 0, pressureIterations = 0,
             pressureCaps = 0, coarsePeak = 0, pressureFallbacks = 0;
    uint32_t pressurePeakIterations = 0, pressurePageChanges = 0, pressureBacklog = 0, pressureResident = 0;
    uint32_t pressureHierarchyBuilds = 0, pressureHierarchyReuses = 0, pressureWarmStarts = 0;
    double pressureDivergence = 0;
    uint64_t refinementWakeCells = 0, refinementTemporalCells = 0, refinementPaddedCells = 0;
    uint32_t narrowRetired = 0, narrowRestored = 0, narrowDeferred = 0, narrowActiveParticles = 0,
             narrowGridCells = 0;
    double narrowGridVolume = 0, narrowParticleVolume = 0;
};
Statistics statistics(const Solver *);
// Called after the existing frame completion fence; never waits per iteration.
// A failed mixed-pressure frame is not published and permanently poisons Solver.
void collect(Solver *);
// Read-only GPU views for deferred numerical/profiling fixtures. No CPU copies.
const Mac *solverMac(const Solver *);
const void *solverParticles(const Solver *);
const void *solverGridQuantity(const Solver *);
struct GeometricTransportMetrics;
const GeometricTransportMetrics *solverGeometricMetrics(const Solver *);
// Shared logical ABI sizes, excluding allocation padding/guard bytes.
size_t bufferBytes(const Config &, Buffer);
// First-frame/configuration preparation only; does not execute simulation or
// capture external DX12 fences. Prebuilds a bounded set of substep variants.
void prepare(Solver *, const void *frame);
// All pointers here are host-side metadata, never liquid particle readbacks.
void enqueue(Solver *, void *stream, const void *frame, const void *colliderSlices, uint32_t steps,
             bool moving, bool rebuild, bool reset = false);
} // namespace lab::cuda_fluid
