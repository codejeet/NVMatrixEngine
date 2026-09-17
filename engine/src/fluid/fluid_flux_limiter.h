#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
// Limits shared phase transfers, never the cell inventory. Existing geometric
// excess is reported separately: this is not a moving-cut-cell closure solver.
class FluidFluxLimiter {
  public:
    FluidFluxLimiter(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4);
    void beginFrame(ID3D12GraphicsCommandList *, bool validate, bool reset);
    void record(ID3D12GraphicsCommandList *, ID3D12Resource *resident, ID3D12Resource *capacity,
                ID3D12Resource *transfers);
    void finishFrame(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;

  private:
    static constexpr uint32_t maxIterations = 128;
    DirectX::XMUINT4 grid;
    uint32_t faceCount = 0, calls = 0;
    gpu::Buffer scales, control, arguments, readback, snapshot;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> indirect;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 7> pipelines;
    std::array<uint32_t, 16> stats{};
    bool validateFrame = false, validated = false;
    uint64_t bytes = 0, frames = 0, steps = 0, iterations = 0;
    double lastMs = 0, totalMs = 0, auditError = 0;
    double peakNewExcess = 0, peakOldExcess = 0;
    double conservationError = 0, energyIncrease = 0;
    void validateSnapshot();
};
} // namespace lab
