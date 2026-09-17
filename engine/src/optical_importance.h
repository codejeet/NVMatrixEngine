#pragma once
#include "gpu_resources.h"
#include <DirectXMath.h>
#include <array>

namespace lab {
// GPU scheduling history only. Never feeds a filtered radiance signal to RR.
class OpticalImportance {
  public:
    OpticalImportance(ID3D12Device *, ID3D12RootSignature *, const std::filesystem::path &, uint32_t width,
                      uint32_t height, bool validate);
    void record(ID3D12GraphicsCommandList *, uint32_t frame);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    void setWorldBricks(ID3D12Device *, ID3D12Resource *state, DirectX::XMUINT4 grid,
                        DirectX::XMFLOAT4 minimumSpacing);
    gpu::Buffer pixels, counters, receivers, feedback;
    ID3D12Resource *world = nullptr;
    bool feedbackReady = false;
    double gpuMs = 0;
    uint64_t allocatedBytes = 0, audits = 0;
    std::array<uint32_t, 16> counts{};

  private:
    struct Pixel {
        DirectX::XMFLOAT4 positionDepth, normalRoughness;
        DirectX::XMUINT4 control;
        DirectX::XMFLOAT4 moments, signals;
    };
    static_assert(sizeof(Pixel) == 80);
    uint32_t width, height, currentFrame = 0;
    bool validation;
    gpu::Buffer readback, snapshot, feedbackSnapshot;
    DirectX::XMUINT4 worldGrid{};
    DirectX::XMFLOAT4 worldMinimum{};
    uint64_t feedbackAudits = 0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> finalize;
    std::vector<double> timings;
    std::array<uint64_t, 16> totals{};
    void audit();
};
} // namespace lab
