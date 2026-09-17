#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
class FluidImplicitTransport {
  public:
    FluidImplicitTransport(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4,
                           bool precise = false);
    void beginFrame(ID3D12GraphicsCommandList *, bool validate, bool reset);
    void record(ID3D12GraphicsCommandList *, ID3D12Resource *resident, ID3D12Resource *capacity,
                ID3D12Resource *rates, ID3D12Resource *output, ID3D12Resource *transfers,
                ID3D12Resource *limiter, float dt, bool swept);
    void finishFrame(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;

  private:
    struct Constants {
        DirectX::XMUINT4 grid{};
        DirectX::XMFLOAT4 parameters{}; // dt, normalized residual tolerance, scale floor, swept
    } constants{};
    static constexpr uint32_t maxIterations = 256;
    gpu::Buffer solution[2], diagonal, control, arguments, readback, snapshot;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 7> pipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> indirect;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    std::array<uint32_t, 16> stats{};
    std::array<uint64_t, 6> offsets{};
    uint32_t faceCount = 0, calls = 0;
    uint64_t frames = 0, steps = 0, iterations = 0, auditedFrames = 0, bytes = 0;
    uint64_t closingCells = 0, closingWaterCells = 0;
    uint64_t auditedIdleFrames = 0;
    bool precise = false;
    uint32_t stateBytes = 16, capacityBytes = 8;
    bool validateFrame = false, validated = false;
    double gpuMs = 0, totalMs = 0, residualError = 0, balanceError = 0, conservationError = 0;
    double energyIncrease = 0, peakResidual = 0, peakExcess = 0;
    void validateSnapshot(bool failedSolve = false);
};
} // namespace lab
