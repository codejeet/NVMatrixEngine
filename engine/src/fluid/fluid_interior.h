#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>
namespace lab {
struct FluidGpuView;
struct FluidComplexityGpuView;
struct FluidSystemDesc;
class FluidInterior {
  public:
    FluidInterior(ID3D12Device *, const std::filesystem::path &, const FluidSystemDesc &, DirectX::XMUINT4);
    void setImportance(const FluidComplexityGpuView &);
    void begin(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *frame,
               ID3D12Resource *solids, bool reset, uint32_t issued, bool validate, bool advancing,
               bool solidsChanged);
    void restore(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *, ID3D12Resource *,
                 bool finalExchange = false);
    void advect(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *, ID3D12Resource *);
    void deposit(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *, ID3D12Resource *);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    ID3D12Resource *state() const {
        return cells.resource.Get();
    }
    ID3D12Resource *totals() const {
        return counters.resource.Get();
    }
    ID3D12Resource *binArguments() const {
        return arguments.resource.Get();
    }
    double massUnits() const {
        return counts[1] / 16.0;
    }
    bool forcedFine = false;
    bool changed = false;

  private:
    struct Constants {
        DirectX::XMUINT4 coarse, bricks, control;
        DirectX::XMFLOAT4 policy;
    } constants{};
    gpu::Buffer cells, counters, freeSlots, arguments, active, readback, validation;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> pipelines;
    ID3D12Resource *importance = nullptr;
    uint32_t capacity = 0, frameNumber = 0, queryCount = 0;
    float cellSize = 0;
    uint64_t snapshotStride = 0, demotions = 0, promotions = 0;
    bool validatePending = false, validated = false, previousForcedFine = false;
    bool orderedAllocation = false;
    double gpuMs = 0, massError = 0, linearError = 0, angularError = 0, energyIncrease = 0;
    std::array<uint32_t, 16> counts{};
    void bind(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *, ID3D12Resource *);
    void pass(ID3D12GraphicsCommandList *, uint32_t, uint32_t);
    void snapshot(ID3D12GraphicsCommandList *, const FluidGpuView &, uint32_t);
};
} // namespace lab
