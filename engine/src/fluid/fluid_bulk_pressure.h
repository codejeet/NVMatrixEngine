#pragma once
#include "fluid_bulk.h"
#include "fluid_cut_cells.h"
#include <array>

namespace lab {
// Supplies pressure/velocity coverage from filled Eulerian interiors. The
// particle band still owns the free surface; no mass is duplicated or removed.
class FluidBulkPressure {
  public:
    FluidBulkPressure(ID3D12Device *, const std::filesystem::path &, DirectX::XMUINT4, float particleVolume,
                      bool closingSupport = false, bool shrinkingSupport = false, bool precise = false);
    void beginFrame(ID3D12GraphicsCommandList *, bool validate);
    void record(ID3D12GraphicsCommandList *, const FluidBulkGpuView &, const FluidCutCellGpuView &,
                ID3D12Resource *cells, ID3D12Resource *faces);
    void finishFrame(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    ID3D12Resource *supportVolume() const {
        return support.resource.Get();
    }

  private:
    struct Constants {
        DirectX::XMUINT4 fine{}, coarse{}, control{};
        DirectX::XMFLOAT4 parameters{}; // particle volume, full-cell relative tolerance
    } constants{};
    gpu::Buffer counters, readback, snapshot, support;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 3> pipelines;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    std::array<uint32_t, 8> metrics{};
    std::array<uint64_t, 9> offsets{};
    uint32_t faceCount = 0, records = 0;
    uint64_t frames = 0, steps = 0, auditedFrames = 0, auditedIdleFrames = 0;
    uint64_t addedCells = 0, filledFaces = 0, closingCells = 0;
    uint64_t partialClosingCells = 0;
    bool closingSupport = false;
    bool shrinkingSupport = false;
    uint64_t partialShrinkingCells = 0;
    bool precise = false;
    bool validateFrame = false, validated = false;
    double gpuMs = 0, totalMs = 0, velocityError = 0, massWeightError = 0;
    void validateSnapshot();
};
} // namespace lab
