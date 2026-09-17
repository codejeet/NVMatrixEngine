#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>

namespace lab {
// ST-FLIP numerical transfer component. Borrows the existing particles, bins and
// projected MAC grid. Pressure integration and swept solids are separate gates;
// this is deliberately not enabled in the playable solver yet.
class FluidSpacetime {
  public:
    struct View {
        ID3D12Resource *frame = nullptr, *particles = nullptr;
        ID3D12Resource *offsets = nullptr, *indices = nullptr, *faces = nullptr;
    };
    FluidSpacetime(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4 grid, uint32_t capacity);
    void reset(ID3D12GraphicsCommandList *, const View &);
    void seed(ID3D12GraphicsCommandList *, const View &, uint32_t first, uint32_t count);
    void deposit(ID3D12GraphicsCommandList *, const View &, float previousDt, float phaseEta = .5f);
    // Call after G2P updated velocities, INSTEAD of baseline position advection.
    // Caller supplies dt/serial from simulation, never display frame or wall time.
    void advect(ID3D12GraphicsCommandList *, const View &, float dt, uint32_t step, uint32_t seed,
                float jitterStrength = 1);
    void synchronize(ID3D12GraphicsCommandList *, const View &);
    ID3D12Resource *timeOffsets() const {
        return times.resource.Get();
    }
    ID3D12Resource *renderParticles() const {
        return render.resource.Get();
    }
    ID3D12Resource *phaseFaces() const {
        return phase.resource.Get();
    }
    ID3D12Resource *diagnostics() const {
        return counters.resource.Get();
    }

  private:
    struct Constants {
        DirectX::XMFLOAT4 time{1, 0, 1, .5f};
        DirectX::XMUINT4 control{};
    } constants;
    DirectX::XMUINT4 grid;
    uint32_t capacity, faceCount;
    bool initialized = false;
    gpu::Buffer times, render, phase, counters;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 6> pipelines;
    void bind(ID3D12GraphicsCommandList *, const View &, bool requireBins = false);
    void pass(ID3D12GraphicsCommandList *, uint32_t pipeline, uint32_t count);
};
} // namespace lab
