#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
// A conservative source-admission transaction, not a simulation/advection
// repair. Pending quantities remain owned/accounted for until a cell admits them.
class FluidVolumeAllocator {
  public:
    FluidVolumeAllocator(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4 grid,
                         float particleVolume, bool precise = false);
    ID3D12Resource *pending() const {
        return pendingState.resource.Get();
    }
    void beginFrame();
    void record(ID3D12GraphicsCommandList *, ID3D12Resource *resident, ID3D12Resource *capacity,
                ID3D12Resource *aperture, bool validate, bool advance);
    void collect(double gpuMs);
    void report(std::ostream &) const;
    double pendingVolume() const {
        return metrics.pending.w;
    }

  private:
    static constexpr uint32_t maxIterations = 64;
    DirectX::XMUINT4 grid{};
    uint32_t faceStride = 0, groups = 0;
    float routingTolerance = 0;
    struct Metrics {
        DirectX::XMFLOAT4 pending, activity, bounds;
        DirectX::XMUINT4 counts;
    } metrics{};
    static_assert(sizeof(Metrics) == 64);
    gpu::Buffer pendingState, weights, transfers, partials, totals, control, arguments, readback, snapshot;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> indirect;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 7> pipelines;
    uint64_t gpuBytes = 0, frames = 0;
    double lastMs = 0, totalMs = 0, auditError = 0, auditMassError = 0, auditEnergyIncrease = 0;
    bool precise = false;
    uint32_t stateBytes = 16, capacityBytes = 8;
    bool validateFrame = false, validated = false, advanceFrame = false;
    void validateSnapshot();
};
} // namespace lab
