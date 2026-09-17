#pragma once
#include "fluid_cut_cells.h"
#include "fluid_mac_pressure.h"
#include <array>
#include <ostream>

namespace lab {
// Capacity correction: either air-only extension of fixed liquid flux, or a
// coupled mixed-MAC correction applied before G2P. Neither clips inventory.
class FluidCarrierProjection {
  public:
    FluidCarrierProjection(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4,
                           DirectX::XMFLOAT4 minimumCell, DirectX::XMFLOAT4 maximum, bool coupled = false,
                           uint32_t pressureCycles = 1);
    void beginFrame(ID3D12GraphicsCommandList *, bool validate, bool reset);
    ID3D12Resource *record(ID3D12GraphicsCommandList *, const FluidCutCellGpuView &, ID3D12Resource *resident,
                           ID3D12Resource *cells, ID3D12Resource *canonical, float dt,
                           const FluidMacConstraintView &mac = {});
    void finishFrame(ID3D12GraphicsCommandList *);
    void captureApplied(ID3D12GraphicsCommandList *, ID3D12Resource *faces, ID3D12Resource *canonical);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;

  private:
    struct Constants {
        DirectX::XMUINT4 fine{}, coarse{};
        DirectX::XMFLOAT4 minimumCell{}, maximum{}, parameters{};
    } constants{};
    static constexpr uint32_t maxIterations = 256;
    gpu::Buffer extended, weights, rates, rows, potential, control, arguments, readback, fraction;
    gpu::Buffer lambda[2], pressureRates;
    bool coupled = false;
    uint32_t capacityPressureCycles = 1;
    std::array<gpu::Buffer, 16> snapshots;
    std::array<Constants, 16> snapshotConstants{};
    std::array<bool, 16> snapshotSwept{};
    uint32_t controlBytes = 64;
    uint64_t snapshotDataBytes = 0, auditedSubsteps = 0, totalSubsteps = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 14> pipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> indirect;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    std::array<uint32_t, 16> stats{};
    std::array<uint64_t, 21> offsets{};
    uint32_t appliedMask = 0;
    uint32_t fineFaces = 0, coarseFaces = 0, calls = 0;
    uint64_t steps = 0, frames = 0, iterations = 0, auditedFrames = 0, auditedIdleFrames = 0, bytes = 0;
    uint64_t eligibleFaces = 0, changedFaces = 0;
    bool validateFrame = false, validated = false;
    double auditedPhaseDeficit = 0, auditedPhaseResidual = 0;
    double gpuMs = 0, totalMs = 0, peakResidual = 0, weightError = 0, matrixError = 0, extensionError = 0;
    double mixedDivergence = 0, appliedDivergence = 0, appliedVelocityError = 0;
    double originalPhysicalDivergence = 0, proposedPhysicalDivergence = 0, appliedPhysicalDivergence = 0;
    void validateSnapshot(uint32_t call);
};
} // namespace lab
