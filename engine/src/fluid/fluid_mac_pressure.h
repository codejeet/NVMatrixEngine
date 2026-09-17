#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
struct FluidMacRow {
    uint32_t count;
    float diagonal, rhs, step;
    uint32_t neighbor[24];
    float coefficient[24];
    float boundaryDiagonal, volumeUnits;
};
static_assert(sizeof(FluidMacRow) == 216);
struct FluidCutPressureRow {
    double rhs, diagonal;
    double conductance[6];
};
static_assert(sizeof(FluidCutPressureRow) == 64);

// Borrowed UAVs for a divergence-preserving capacity correction. Valid after
// FluidMac::solve until the next substep; ownership stays with the MAC solver.
class FluidMacPressure;
struct FluidMacConstraintView {
    ID3D12Resource *map = nullptr, *state = nullptr, *rows = nullptr, *preciseRows = nullptr;
    ID3D12Resource *faces = nullptr;
    FluidMacPressure *solver = nullptr;
};

// A solver for the actual mixed MAC operator, not a replacement fine-grid
// projection. Constant Galerkin aggregation only preconditions the CG solve.
class FluidMacPressure {
  public:
    FluidMacPressure(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4, float cellSize,
                     float density, float simulationRate, bool cutPressure = false);
    void beginFrame(bool audit);
    void solve(ID3D12GraphicsCommandList *, ID3D12Resource *rows, ID3D12Resource *map, ID3D12Resource *cells,
               ID3D12Resource *list, ID3D12Resource *counts, ID3D12Resource *solution,
               ID3D12Resource *cutExact = nullptr);
    void collect();
    // One V cycle on the latest assembled operator. Imports a double RHS and
    // replaces it with a double correction; does not overwrite physical pressure,
    // rebuild the hierarchy, run forces or change the caller's GPU predicate.
    void precondition(ID3D12GraphicsCommandList *, ID3D12Resource *rhsCorrection);
    void report(std::ostream &) const;
    bool splitCoarse = false; // validation/profile comparison with distributed levels
    ID3D12Resource *pressureResource() const {
        return precisePressure.resource.Get();
    }

  private:
    struct Constants {
        DirectX::XMUINT4 grid, levels[16], control;
        DirectX::XMFLOAT4 parameters;
    } constants{};
    struct CoarseRow {
        DirectX::XMFLOAT4 xy, z;
    };
    static_assert(sizeof(Constants) == 304 && sizeof(CoarseRow) == 32);
    enum Pass {
        Clear,
        Assemble,
        Canonical,
        Prepare,
        Factor,
        Initialize,
        ResetCorrection,
        FineSmooth,
        RestrictBase,
        CoarseSmooth,
        RestrictCoarse,
        Bottom,
        ProlongCoarse,
        ProlongFine,
        Dot,
        Direction,
        Apply,
        Update,
        Residual,
        Reduce,
        CoarseCycle,
        ImportCorrection,
        ExportCorrection,
        PassCount
    };
    static constexpr uint32_t maxIterations = 32;
    uint32_t capacity = 0, solves = 0;
    bool audit = false, validated = false;
    bool cutProjection = false;
    std::array<ID3D12Resource *, 6> cachedBase{};
    uint64_t bytes = 0, totalSolves = 0, totalIterations = 0, exhausted = 0;
    uint32_t lastIterations = 0, peakIterations = 0;
    std::array<uint32_t, 16> activePerLevel{};
    double peakFinalDivergence = 0, cappedMaxDivergence = 0;
    double relativeResidual = 0, maxDivergence = 0, hierarchyError = 0, factorError = 0, residualError = 0;
    gpu::Buffer uniforms, fine[2], vectors, rows, rhs, error[2], work, counts, args, partials, scalars,
        factor;
    gpu::Buffer readback, snapshot;
    gpu::Buffer precisePressure;
    gpu::Buffer failureSnapshot;
    std::filesystem::path outputFolder;
    uint64_t lastCappedSolve = 0;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, PassCount> pipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    void validateSnapshot();
};
} // namespace lab
