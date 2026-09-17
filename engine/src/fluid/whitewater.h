#pragma once
#include "fluid_surface.h"

namespace lab {
// Bounded, one-way coupled secondary phase. No neighbor search between secondary
// particles and no CPU particle allocation/readback in normal play.
class Whitewater {
  public:
    static constexpr uint32_t capacity = 8192;
    Whitewater(ID3D12Device5 *, const std::filesystem::path &, const FluidSurface &);
    void record(ID3D12GraphicsCommandList4 *, const FluidSystem &, const FluidSurface &);
    void recordReadback(ID3D12GraphicsCommandList *, bool validate);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    ID3D12Resource *particleResource() const {
        return particles.resource.Get();
    }
    ID3D12Resource *foamResource() const {
        return foamHistory[foamIndex].resource.Get();
    }
    D3D12_GPU_VIRTUAL_ADDRESS accelerationStructure() const {
        return blas.resource->GetGPUVirtualAddress();
    }
    double simulationMs = 0, blasMs = 0;
    std::array<uint32_t, 8> counts{};

  private:
    struct Particle {
        DirectX::XMFLOAT4 positionRadius, velocityLife, previousType, surfaceAge;
    };
    static_assert(sizeof(Particle) == 64);
    struct Uniforms {
        DirectX::XMFLOAT4 fieldMinimum, domainMinimum, domainMaximum;
        DirectX::XMUINT4 bricks, grid, control;
        DirectX::XMFLOAT4 timeGravity, emitter;
    };
    gpu::Buffer particles, aabbs, counters, constants, blas, scratch, readback, validation, foam;
    std::array<gpu::Buffer, 2> foamHistory;
    uint32_t foamNodes = 0, foamIndex = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear, update, foamClear, foamSplat, foamTransport;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    bool readable = false, recorded = false, validatePending = false, validated = false;
};
} // namespace lab
