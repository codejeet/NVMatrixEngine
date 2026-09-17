#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
enum class FluidPressureMode { Uniform, Active, Multigrid };
// Solves the existing fine-grid operator. The coarse level is an algebraic
// correction space, NOT a replacement for fine MAC velocities or liquid mass.
class FluidPressure {
  public:
    FluidPressure(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4 grid, FluidPressureMode,
                  uint32_t iterations, uint32_t cycles);
    void beginFrame(bool validate) {
        solves = 0;
        validateFrame = validate;
    }
    uint32_t solve(ID3D12GraphicsCommandList *, ID3D12Resource *stencil, ID3D12Resource *ping,
                   ID3D12Resource *pong);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    double gpuMs = 0;
    bool validated = false;

  private:
    struct Uniforms {
        DirectX::XMUINT4 fine, coarse, tiles;
        DirectX::XMFLOAT4 settings;
    };
    FluidPressureMode mode;
    uint32_t iterations, cycles, solves = 0;
    DirectX::XMUINT4 grid{}, coarseGrid{}, tiles{};
    gpu::Buffer uniforms, list, counters, arguments, coarseA, coarseRhs, error[2], readback, snapshot,
        tileFlags;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear, active, prepare, assemble, smooth, restrictResidual,
        coarseSmooth, prolongate, residual, tileSmooth;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    bool validateFrame = false;
    std::array<uint32_t, 8> metrics{};
    uint64_t allocatedBytes = 0, measuredFrames = 0;
    double totalMs = 0;
    void validateSnapshot();
};
} // namespace lab
