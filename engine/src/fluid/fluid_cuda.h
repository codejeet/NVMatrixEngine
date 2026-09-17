#pragma once
#include "../gpu_resources.h"
#include "../gpu_submission_profile.h"
#include "fluid_uniforms.h"
#include "fluid_colliders.h"
#include "cuda/fluid_cuda_kernels.h"
#include <memory>
#include <ostream>

namespace lab {
class FluidCuda {
  public:
    // Optional joint inventory and non-owning phase/plane outputs use the same
    // shared-buffer/fence transaction. The caller retains these DX12 resources
    // for subsequent reconstruction; no CPU geometry copies are required.
    FluidCuda(ID3D12Device *, const cuda_fluid::Config &,
              const std::array<ID3D12Resource *, cuda_fluid::BufferCount> &, std::span<const float> mesh,
              ID3D12CommandQueue * = nullptr, bool graphicsContext = false,
              const std::array<ID3D12Resource *, cuda_fluid::OwnershipBufferCount> & = {},
              const std::array<ID3D12Resource *, cuda_fluid::GridInventoryBufferCount> & = {},
              const std::array<ID3D12Resource *, cuda_fluid::SurfaceGeometryBufferCount> & = {},
              ID3D12Resource *narrowPreviousPositions = nullptr);
    ~FluidCuda();
    FluidCuda(const FluidCuda &) = delete;
    FluidCuda &operator=(const FluidCuda &) = delete;
    // Ends/submits the DX12 prefix, enqueues CUDA behind a shared fence, queues
    // the GPU return wait, then reopens the same command list. Never resets an
    // in-flight allocator and never waits for simulation data on the CPU.
    void run(ID3D12GraphicsCommandList *, ID3D12CommandQueue *, ID3D12CommandAllocator *,
             const FluidSimulationConstants &, const FluidColliderTimeline &, uint32_t steps, bool rebuild,
             bool reset = false, gpu::SubmissionTimeline * = nullptr);
    void collect(); // call only after the engine's existing frame fence
    cuda_fluid::State state() const;
    std::array<double, 5> telemetry() const; // work, handoff, span, CPU enqueue, cumulative preparation (ms)
    void report(std::ostream &) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace lab
