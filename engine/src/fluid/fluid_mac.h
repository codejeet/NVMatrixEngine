#pragma once
#include "../gpu_resources.h"
#include "fluid_mac_pressure.h"
#include "fluid_cut_cells.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>
#include <memory>

namespace lab {
struct FluidComplexityGpuView;
// Two-level MAC projection. Fine allocation is retained only as transfer cache;
// coarse cells have one pressure DOF and shared coarse normal face velocities.
class FluidMac {
  public:
    FluidMac(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4, uint32_t iterations,
             float cellSize, float density, float simulationRate, float surfaceTension,
             bool multigrid = false, bool cutPressure = false);
    void setCutCells(const FluidCutCellGpuView &view) {
        cut = view;
    }
    void setImportance(const FluidComplexityGpuView &);
    void beginFrame(bool reset, bool validate);
    uint32_t solve(ID3D12GraphicsCommandList *, ID3D12Resource *frame, ID3D12Resource *faces,
                   ID3D12Resource *scratch, ID3D12Resource *cells, ID3D12Resource *solids,
                   ID3D12Resource *material, ID3D12Resource *ping, ID3D12Resource *pong);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    void drawDebug(ID3D12GraphicsCommandList *, ID3D12Resource *frame);
    bool debugVisible = false;
    bool forcedFine = false;
    bool splitCoarse = false;
    uint32_t coarseLeaves() const {
        return metrics[1];
    }
    // Shared-face physical volume flux for future conservative bulk transport.
    // Internal coarse-child faces are only the compatible FP32 transfer cache.
    ID3D12Resource *projectedVolumeFlux() const {
        return cutFlux.resource.Get();
    }
    FluidMacConstraintView constraintView(ID3D12Resource *faces) const {
        return {map.resource.Get(), state.resource.Get(), rows.resource.Get(), cutExact.resource.Get(), faces,
                multigrid.get()};
    }
    void applyCapacity(ID3D12GraphicsCommandList *, ID3D12Resource *frame, ID3D12Resource *faces,
                       ID3D12Resource *cells, ID3D12Resource *correctedFlux);
    bool validated = false;
    double residualMismatch = 0;
    double gpuMs = 0;

  private:
    using Row = FluidMacRow;
    static_assert(sizeof(Row) == 216);
    DirectX::XMUINT4 grid{}, coarse{}, bricks{};
    uint32_t iterations = 0, solves = 0, faceCount = 0;
    float h = 0, rho = 0, rate = 0, sigma = 0;
    ID3D12Resource *importance = nullptr;
    FluidCutCellGpuView cut{};
    bool cutProjection = false;
    std::unique_ptr<FluidMacPressure> multigrid;
    gpu::Buffer map, state, list, rows, counters, arguments, readback, snapshot, cutExact, cutFlux;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 11> pipelines;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> debug;
    ID3D12Resource *lastCells = nullptr;
    ID3D12Resource *lastPressure = nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    std::array<uint32_t, 16> metrics{};
    bool resetPending = true, validateFrame = false;
    uint64_t allocatedBytes = 0, measuredFrames = 0, peakCoarse = 0, peakJunctions = 0;
    double totalMs = 0, matrixError = 0, rhsDivergenceError = 0, fluxError = 0, prolongationError = 0;
    double velocityCacheDivergence = 0, velocityCacheFluxError = 0;
    double preciseMatrixError = 0, canonicalDivergence = 0;
    uint64_t auditedFrames = 0;
    // Independent final-substep snapshots, not counters for every GPU substep.
    uint64_t closingCellSamples = 0, closingLiquidSamples = 0, closingConnectedSamples = 0;
    double closingLiquidVolumeM3 = 0;
    void copySnapshot(ID3D12GraphicsCommandList *, ID3D12Resource *, uint64_t &, uint64_t);
    void validateSnapshot();
};
} // namespace lab
