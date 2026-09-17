#pragma once
#include "../src/gpu_cuda_interop.h"
#include "../src/fluid/cuda/fluid_cuda_owned_transport.h"
#include <cuda_runtime_api.h>

// Test adapter only. Reuses the production DX12/CUDA handoff and the exact
// exchange grid owners; no CPU particle/quantity staging or duplicate inventory.
class CudaOwnedExchangeFixture {
    std::unique_ptr<lab::gpu::CudaInterop> interop;
    lab::cuda_fluid::OwnedTransport *solver = nullptr;

  public:
    CudaOwnedExchangeFixture(ID3D12Device *device, ID3D12CommandQueue *queue,
                             const std::array<lab::gpu::CudaInterop::Binding, 6> &views) {
        interop = std::make_unique<lab::gpu::CudaInterop>(device, views, queue);
        const auto context = interop->activate();
        solver = lab::cuda_fluid::createOwnedTransport({2, 1, 1});
    }
    ~CudaOwnedExchangeFixture() {
        try {
            const auto context = interop->activate();
            interop->drainForTeardown();
            lab::cuda_fluid::destroyOwnedTransport(solver);
            solver = nullptr;
        } catch (...) {
            lab::cuda_fluid::destroyOwnedTransport(solver);
        }
    }
    void record(ID3D12GraphicsCommandList *cmd, ID3D12CommandQueue *queue,
                ID3D12CommandAllocator *allocator) {
        const auto context = interop->activate();
        interop->execute(cmd, queue, allocator, [&](void *stream) {
            auto p = interop->pointers();
            lab::cuda_fluid::OwnedTransportInputs v{p[0], p[2], p[3],
                                                    p[1], p[4], static_cast<uint32_t *>(p[5]) + 16};
            lab::cuda_fluid::enqueueOwnedTransport(solver, stream, v, .25f);
        });
    }
    void collect() {
        const auto context = interop->activate();
        const auto timing = interop->collect();
        lab::cuda_fluid::OwnedTransportMetrics metrics{};
        const auto result = cudaMemcpy(&metrics, lab::cuda_fluid::ownedTransportMetrics(solver),
                                       sizeof(metrics), cudaMemcpyDeviceToHost);
        if (result != cudaSuccess)
            throw std::runtime_error(cudaGetErrorString(result));
        if (timing.submissions != 1 || !metrics.converged || metrics.invalid || metrics.capped ||
            metrics.residual > 2e-13)
            throw std::runtime_error("Actual exchange grid owners failed CUDA transport");
    }
};
