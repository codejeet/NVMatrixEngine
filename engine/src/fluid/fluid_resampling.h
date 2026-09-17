#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
struct FluidSystemDesc;
struct FluidGpuView;
struct FluidComplexityGpuView;
// Conservative sample-count adaptation, not particle-free bulk ownership.
// Consumes previous-frame importance plus current occupancy/collision guards.
class FluidResampling {
  public:
    FluidResampling(ID3D12Device *, const std::filesystem::path &, const FluidSystemDesc &,
                    DirectX::XMUINT4 grid);
    void setImportance(const FluidComplexityGpuView &);
    void record(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *frameConstants,
                ID3D12Resource *solids, uint32_t issued, bool reset, bool advancing, bool validate);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    bool changed = false;
    bool forcedFine = false;
    uint32_t activeSamples() const {
        return counts[6];
    }
    double milliseconds() const {
        return lastMs;
    }
    ID3D12Resource *binArguments() const {
        return arguments.resource.Get();
    }

  private:
    gpu::Buffer uniforms, freeSlots, counters, selected, readback, validation, arguments;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> pipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    ID3D12Resource *importance = nullptr;
    ID3D12Resource *importanceArguments = nullptr;
    DirectX::XMUINT4 bricks{}, grid{};
    uint32_t capacity = 0;
    uint32_t expectedMass = 0;
    float cellSize = 0;
    bool apic = true;
    bool orderedAllocation = false;
    bool validPending = false, validated = false;
    uint64_t frames = 0, merges = 0, splits = 0, rejected = 0, starved = 0;
    uint64_t binRebuilds = 0;
    double totalMs = 0, lastMs = 0, massError = 0, linearError = 0, angularError = 0, energyIncrease = 0;
    std::array<uint32_t, 16> counts{};
};
} // namespace lab
