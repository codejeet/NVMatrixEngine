#include "fluid_cuda.h"
#include <algorithm>
#include <vector>
#if PT_FLUID_CUDA
#include "../gpu_cuda_interop.h"

namespace lab {
struct FluidCuda::Impl {
    std::unique_ptr<gpu::CudaInterop> interop;
    cuda_fluid::Solver *solver = nullptr;
    cuda_fluid::Config config{};
    bool joint = false, surface = false;
    gpu::CudaInterop::Timing timing{};
    ~Impl() {
        // Also safe when a substep throws after partially enqueuing kernels.
        if (interop) {
            try {
                const auto context = interop->activate();
                interop->drainForTeardown();
                cuda_fluid::destroy(solver);
                return;
            } catch (...) {
                // Lost CUDA context: CUDA destruction calls may fail, but the
                // solver's noexcept destroy still releases its CPU ownership.
                // CudaInterop subsequently destroys the owned driver context.
            }
            interop->drainForTeardown();
        }
        cuda_fluid::destroy(solver);
    }
};
FluidCuda::FluidCuda(ID3D12Device *device, const cuda_fluid::Config &config,
                     const std::array<ID3D12Resource *, cuda_fluid::BufferCount> &resources,
                     std::span<const float> mesh, ID3D12CommandQueue *queue, bool graphicsContext,
                     const std::array<ID3D12Resource *, cuda_fluid::OwnershipBufferCount> &ownership,
                     const std::array<ID3D12Resource *, cuda_fluid::GridInventoryBufferCount> &grid,
                     const std::array<ID3D12Resource *, cuda_fluid::SurfaceGeometryBufferCount> &surface,
                     ID3D12Resource *narrowPrevious)
    : impl(std::make_unique<Impl>()) {
    impl->config = config;
    if (!config.nx || !config.ny || !config.nz || config.nx > 1048576 || config.ny > 1048576 ||
        config.nz > 1048576 || !config.capacity || config.capacity > 1048576)
        throw std::runtime_error("Invalid CUDA fluid dimensions/capacity");
    const uint64_t cells = uint64_t(config.nx) * config.ny * config.nz;
    if (cells > 1048576)
        throw std::runtime_error("CUDA fluid cell capacity exceeded");
    std::vector<gpu::CudaInterop::Binding> bindings(cuda_fluid::BufferCount);
    for (uint32_t i = 0; i < cuda_fluid::BufferCount; ++i)
        bindings[i] = {resources[i], cuda_fluid::bufferBytes(config, cuda_fluid::Buffer(i))};
    for (uint32_t i = 0; i < ownership.size(); ++i) {
        if (bool(ownership[i]) != config.ownedParticles)
            throw std::runtime_error("CUDA ownership resources do not match selection");
        if (ownership[i])
            bindings.push_back(
                {ownership[i], cuda_fluid::ownershipBytes(config, cuda_fluid::OwnershipBuffer(i))});
    }
    const cuda_fluid::GridInventory gridResources{grid[0], grid[1]};
    impl->joint = cuda_fluid::validateGridInventory(config, gridResources);
    impl->surface = cuda_fluid::validateSurfaceGeometry(config, gridResources, {surface[0], surface[1]});
    if (config.narrowBand != bool(narrowPrevious) || (config.narrowBand && (!impl->joint || impl->surface)))
        throw std::runtime_error("Narrow-band CUDA requires grid ownership and motion history");
    if (impl->joint)
        for (uint32_t i = 0; i < grid.size(); ++i)
            bindings.push_back(
                {grid[i], cuda_fluid::gridInventoryBytes(config, cuda_fluid::GridInventoryBuffer(i))});
    if (impl->surface)
        for (uint32_t i = 0; i < surface.size(); ++i)
            bindings.push_back(
                {surface[i], cuda_fluid::surfaceGeometryBytes(config, cuda_fluid::SurfaceGeometryBuffer(i))});
    if (config.narrowBand)
        bindings.push_back({narrowPrevious, size_t(config.capacity) * 16});
    impl->interop = std::make_unique<gpu::CudaInterop>(
        device, bindings, queue,
        graphicsContext ? gpu::CudaInterop::ContextMode::Graphics : gpu::CudaInterop::ContextMode::Primary);
    const auto context = impl->interop->activate();
    void *pointers[cuda_fluid::BufferCount]{};
    std::copy_n(impl->interop->pointers().begin(), cuda_fluid::BufferCount, pointers);
    cuda_fluid::Ownership owned{};
    if (config.ownedParticles)
        std::copy_n(impl->interop->pointers().begin() + cuda_fluid::BufferCount, owned.size(), owned.begin());
    cuda_fluid::GridInventory inventory{};
    cuda_fluid::SurfaceGeometry geometry{};
    const auto begin = impl->interop->pointers().begin() + cuda_fluid::BufferCount +
                       (config.ownedParticles ? owned.size() : 0);
    if (impl->joint)
        std::copy_n(begin, inventory.size(), inventory.begin());
    if (impl->surface)
        std::copy_n(begin + inventory.size(), geometry.size(), geometry.begin());
    impl->solver = cuda_fluid::create(config, pointers, mesh.data(), mesh.size(), owned, inventory, geometry,
                                      config.narrowBand ? *(begin + inventory.size()) : nullptr);
}
FluidCuda::~FluidCuda() = default;
void FluidCuda::run(ID3D12GraphicsCommandList *cmd, ID3D12CommandQueue *queue,
                    ID3D12CommandAllocator *allocator, const FluidSimulationConstants &frame,
                    const FluidColliderTimeline &colliders, uint32_t steps, bool rebuild, bool reset,
                    gpu::SubmissionTimeline *timeline) {
    if (steps > FluidColliderTimeline::maxSteps)
        throw std::runtime_error("CUDA fluid substep limit exceeded");
    // Capture/instantiate before submitting the prefix. Replays never capture
    // external semaphores or allocate solver memory inside an ordinary substep.
    gpu::stamp(timeline, gpu::SubmissionStage::CudaPrepareBegin);
    const auto context = impl->interop->activate();
    cuda_fluid::prepare(impl->solver, &frame);
    gpu::stamp(timeline, gpu::SubmissionStage::CudaPrepared);
    impl->interop->execute(
        cmd, queue, allocator,
        [&](void *stream) {
            cuda_fluid::enqueue(impl->solver, stream, &frame, colliders.slices.data(), steps,
                                colliders.moving, rebuild, reset);
        },
        timeline);
}
void FluidCuda::collect() {
    const auto context = impl->interop->activate();
    impl->timing = impl->interop->collect();
    cuda_fluid::collect(impl->solver);
}
cuda_fluid::State FluidCuda::state() const {
    return cuda_fluid::state(impl->solver);
}
std::array<double, 5> FluidCuda::telemetry() const {
    return {impl->timing.cudaMs, impl->timing.handoffMs, impl->timing.spanMs, impl->timing.enqueueMs,
            cuda_fluid::statistics(impl->solver).preparationMs};
}
void FluidCuda::report(std::ostream &o) const {
    const auto stats = cuda_fluid::statistics(impl->solver);
    o << "{\"device\":\"" << impl->interop->deviceName() << "\",\"submissions\":" << impl->timing.submissions
      << ",\"contextMode\":\"" << (impl->interop->graphicsContext() ? "cig" : "primary") << '"'
      << ",\"cigSharedMemoryBytes\":" << impl->interop->graphicsSharedMemoryBytes()
      << ",\"lastGpuMs\":" << impl->timing.cudaMs << ",\"handoffMs\":" << impl->timing.handoffMs
      << ",\"interopSpanMs\":" << impl->timing.spanMs
      << ",\"sharedBuffers\":" << impl->interop->pointers().size()
      << ",\"ownedParticles\":" << (impl->config.ownedParticles ? "true" : "false")
      << ",\"gridOwnedInventory\":" << (impl->joint ? "true" : "false")
      << ",\"surfaceGeometryOutputs\":" << (impl->surface ? "true" : "false")
      << ",\"ownershipBuffers\":" << (impl->config.ownedParticles ? cuda_fluid::OwnershipBufferCount : 0)
      << ",\"cpuEnqueueMs\":" << impl->timing.enqueueMs << ",\"graphBuilds\":" << stats.graphBuilds
      << ",\"graphNodes\":" << stats.graphNodes << ",\"graphReplays\":" << stats.graphReplays
      << ",\"graphBodyNodes\":" << stats.graphBodyNodes << ",\"directSteps\":" << stats.directSteps
      << ",\"graphPreparationMs\":" << stats.preparationMs << ",\"stagingBytes\":" << stats.stagingBytes
      << ",\"mixedPressureBytes\":" << stats.mixedPressureBytes
      << ",\"gridTransportBytes\":" << stats.gridTransportBytes
      << ",\"narrowBand\":" << (impl->config.narrowBand ? "true" : "false")
      << ",\"narrowRetired\":" << stats.narrowRetired << ",\"narrowRestored\":" << stats.narrowRestored
      << ",\"narrowDeferred\":" << stats.narrowDeferred
      << ",\"narrowActiveParticles\":" << stats.narrowActiveParticles
      << ",\"narrowGridCells\":" << stats.narrowGridCells
      << ",\"narrowGridVolume\":" << stats.narrowGridVolume
      << ",\"narrowParticleVolume\":" << stats.narrowParticleVolume
      << ",\"mixedPressure\":" << (cuda_fluid::solverMac(impl->solver) ? "true" : "false")
      << ",\"pressureMode\":\""
      << (!impl->config.mixedPressure       ? "uniform"
          : impl->config.forcedFinePressure ? "fine"
                                            : "mixed")
      << '"' << ",\"pressureBrickCapacity\":" << impl->config.pressureBrickCapacity
      << ",\"pressureChangesPerFrame\":" << impl->config.pressureChangesPerFrame
      << ",\"pressureCgBudget\":" << impl->config.cgIterations << ",\"pressureLoop\":\""
      << (!impl->config.mixedPressure                                     ? "none"
          : impl->config.graphs && impl->config.pressureConditionalGraphs ? "conditional"
                                                                          : "unrolled")
      << '"' << ",\"completedFrames\":" << stats.completedFrames
      << ",\"rejectedFrames\":" << stats.rejectedFrames << ",\"pressureSolves\":" << stats.pressureSolves
      << ",\"pressureIterations\":" << stats.pressureIterations << ",\"pressureCaps\":" << stats.pressureCaps
      << ",\"pressureLoopSolves\":" << stats.pressureLoopSolves
      << ",\"pressureLoopIterations\":" << stats.pressureLoopIterations
      << ",\"pressureCoarsePeak\":" << stats.coarsePeak
      << ",\"pressureFallbacks\":" << stats.pressureFallbacks
      << ",\"pressureBacklog\":" << stats.pressureBacklog
      << ",\"pressureResident\":" << stats.pressureResident
      << ",\"pressurePageChanges\":" << stats.pressurePageChanges
      << ",\"pressurePeakIterations\":" << stats.pressurePeakIterations
      << ",\"pressureHierarchyBuilds\":" << stats.pressureHierarchyBuilds
      << ",\"pressureHierarchyReuses\":" << stats.pressureHierarchyReuses
      << ",\"pressureWarmStarts\":" << stats.pressureWarmStarts
      << ",\"pressureDivergence\":" << stats.pressureDivergence << ",\"refinementPolicy\":\""
      << (impl->config.mixedPressure && !impl->config.forcedFinePressure ? "advected-error-v1" : "none")
      << "\",\"refinementWakeCellSteps\":" << stats.refinementWakeCells
      << ",\"refinementTemporalCellSteps\":" << stats.refinementTemporalCells
      << ",\"refinementPaddedCellSteps\":" << stats.refinementPaddedCells
      << ",\"cpuParticleReadbacks\":0,\"multiscale\":false}";
}
} // namespace lab
#else
namespace lab {
struct FluidCuda::Impl {};
FluidCuda::FluidCuda(ID3D12Device *, const cuda_fluid::Config &,
                     const std::array<ID3D12Resource *, cuda_fluid::BufferCount> &, std::span<const float>,
                     ID3D12CommandQueue *, bool,
                     const std::array<ID3D12Resource *, cuda_fluid::OwnershipBufferCount> &,
                     const std::array<ID3D12Resource *, cuda_fluid::GridInventoryBufferCount> &,
                     const std::array<ID3D12Resource *, cuda_fluid::SurfaceGeometryBufferCount> &,
                     ID3D12Resource *) {
    throw std::runtime_error("CUDA fluid backend is not built; configure with -DNVMATRIXENGINE_CUDA_FLUID=ON");
}
FluidCuda::~FluidCuda() = default;
void FluidCuda::run(ID3D12GraphicsCommandList *, ID3D12CommandQueue *, ID3D12CommandAllocator *,
                    const FluidSimulationConstants &, const FluidColliderTimeline &, uint32_t, bool, bool,
                    gpu::SubmissionTimeline *) {}
void FluidCuda::collect() {}
cuda_fluid::State FluidCuda::state() const {
    return {};
}
std::array<double, 5> FluidCuda::telemetry() const {
    return {};
}
void FluidCuda::report(std::ostream &) const {}
} // namespace lab
#endif
