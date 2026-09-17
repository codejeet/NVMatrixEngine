#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>

namespace lab {
// Transaction layer, not a band policy. Inputs and outputs remain GPU resident.
// Quantity.xyz is volume-weighted velocity, .w is volume (multiply by density
// for SI momentum/mass). The FP32 particle is a cache of its FP64 quantity.
class FluidParticleGridExchange {
  public:
    struct View {
        ID3D12Resource *particles = nullptr;                    // existing 80-byte APIC particles
        ID3D12Resource *offsets = nullptr, *indices = nullptr;  // current fine bins
        ID3D12Resource *requests = nullptr;                     // coarse cells: 1 grid, 0 particles
        ID3D12Resource *capacity = nullptr;                     // double2 endpoint/previous open volume
        ID3D12Resource *sites = nullptr, *siteCounts = nullptr; // geometry-validated GPU sites
        uint32_t siteStride = 0;
        // Restrict recycling to issued IDs when emitters reserve a growing
        // prefix. UINT_MAX lends the entire pool to a shared GPU allocator.
        uint32_t reusableParticleLimit = UINT_MAX;
        ID3D12Resource *previousPositions = nullptr; // reset motion history for restored IDs
    };
    FluidParticleGridExchange(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4 fine,
                              uint32_t maxParticles, float particleVolume, float radius,
                              bool cudaShared = false);
    struct CudaSharing {
        bool particles = false, grid = false;
    };
    FluidParticleGridExchange(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4 fine,
                              uint32_t maxParticles, float particleVolume, float radius, CudaSharing);
    void begin(ID3D12GraphicsCommandList *, const View &, bool reset);
    void seed(ID3D12GraphicsCommandList *, const View &, uint32_t first, uint32_t count);
    void velocityDelta(ID3D12GraphicsCommandList *, const View &);
    void deposit(ID3D12GraphicsCommandList *, const View &);
    // Rebuilds the free list after retirement. One call per exchange phase.
    // Re-bin after successful exchanges before any consumer uses old ranges.
    void restore(ID3D12GraphicsCommandList *, const View &);
    ID3D12Resource *particleQuantities() const {
        return quantities.resource.Get();
    }
    ID3D12Resource *referenceVelocities() const {
        return references.resource.Get();
    }
    ID3D12Resource *gridRead() const {
        return grid[current].resource.Get();
    }
    ID3D12Resource *gridWrite() const {
        return grid[1 - current].resource.Get();
    }
    // Call only after an existing conservative GPU transport wrote gridWrite.
    void commitGridTransport() {
        current = 1 - current;
    }
    ID3D12Resource *counters() const {
        return control.resource.Get();
    }
    DirectX::XMUINT4 dimensions() const {
        return constants.coarse;
    }

  private:
    struct Constants {
        DirectX::XMUINT4 fine{}, coarse{}, work{};
        DirectX::XMFLOAT4 physical{};
        DirectX::XMUINT4 allocation{};
    } constants{};
    gpu::Buffer quantities, references, grid[2], freeIds, control;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 7> pipelines;
    uint32_t current = 0;
    bool restored = false, deposited = false, initialized = false;
    void bind(ID3D12GraphicsCommandList *, const View &, uint32_t transaction = 0);
    void pass(ID3D12GraphicsCommandList *, uint32_t stage, uint32_t groups);
};
} // namespace lab
