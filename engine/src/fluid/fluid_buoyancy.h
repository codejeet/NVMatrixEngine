#pragma once
#include "fluid_system.h"
#include "fluid_surface.h"
namespace lab {
// A fixed small body-query channel. Simulation particles/fields never leave GPU.
// Readback is consumed at the renderer's existing completion fence (one-frame
// force latency), with no additional queue wait.
class FluidBuoyancy {
  public:
    FluidBuoyancy(ID3D12Device *, const std::filesystem::path &, bool hamiltonian = false);
    void record(ID3D12GraphicsCommandList *, const FluidSystem &, const FluidSurface &,
                const std::vector<DirectX::XMFLOAT4> &queries);
    std::vector<DirectX::XMFLOAT4> collect();

  private:
    static constexpr uint32_t capacity = 64;
    uint32_t count = 0;
    gpu::Buffer constants, queries, samples, readback;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> sample;
};
} // namespace lab
