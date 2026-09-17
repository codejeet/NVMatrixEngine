#pragma once
#include "../gpu_resources.h"
#include "fluid_colliders.h"
#include "mesh_sdf.h"
#include <ostream>
#include <span>

namespace lab {
struct FluidSystemDesc;
struct FluidGpuView;
// Geometric capacity, not liquid mass and not a second pressure solver. All
// buffers remain UAVs. changed describes this frame's geometry work; swept is
// valid only for the most recent simulated endpoint/projection. Cached geometry,
// initial/reset state and zero-step placement never supply a new swept source.
struct FluidCutCellGpuView {
    ID3D12Resource *fineVolume, *fineArea, *coarseVolume, *coarseArea, *solidKernel;
    DirectX::XMUINT4 fine, coarse;
    DirectX::XMFLOAT4 minimumSpacing, maximum;
    bool changed;
    bool swept;
    ID3D12Resource *pressureArea;
    bool timeCentered;
    bool preciseCapacity = false; // FP64 restriction of the FP32 fine geometry
};
class FluidCutCells {
  public:
    FluidCutCells(ID3D12Device *, const std::filesystem::path &, const FluidSystemDesc &, DirectX::XMUINT4);
    void beginFrame(bool reset, bool validate);
    void record(ID3D12GraphicsCommandList *, const FluidGpuView &, std::span<const FluidCollider>,
                const MeshSdfAsset &, uint64_t step, bool simulated = true);
    void finishFrame(ID3D12GraphicsCommandList *, ID3D12Resource *bulkInventory = nullptr);
    void collect(uint64_t frequency);
    void drawDebug(ID3D12GraphicsCommandList *, const DirectX::XMFLOAT4X4 &);
    void report(std::ostream &) const;
    FluidCutCellGpuView gpuView() const;
    bool debugVisible = false;

  private:
    struct Constants {
        DirectX::XMFLOAT4 minimumCell, maximum;
        DirectX::XMUINT4 fine, coarse, control;
        DirectX::XMFLOAT4 fixture;
    } constants{};
    struct Metrics {
        DirectX::XMFLOAT4 volume, inventory;
        DirectX::XMUINT4 fineCount, coarseCount, kernel;
    } metrics{};
    static_assert(sizeof(Constants) == 96 && sizeof(Metrics) == 80);
    std::array<gpu::Buffer, 11> buffers;
    gpu::Buffer readback, snapshot, cameraUniforms;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 11> pipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> kernelDispatch;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> debug;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    uint32_t fineStride = 0, coarseStride = 0, queriesUsed = 0, frameUpdates = 0;
    uint64_t updates = 0, frames = 0, allocatedBytes = 0, auditedFrames = 0;
    float simulationRate = 0;
    bool preciseBulk = false;
    bool initialized = false, validateFrame = false, validated = false, bulkConnected = false;
    bool projectionSwept = false;
    bool timeCentered = false;
    double gpuMs = 0, totalMs = 0, geometryError = 0, restrictionError = 0, historyError = 0;
    double kernelError = 0;
    std::vector<FluidCollider> lastColliders;
    const MeshSdfAsset *mesh = nullptr;
    std::array<uint64_t, 12> snapshotOffsets{};
    double timeAreaError = 0;
    void bind(ID3D12GraphicsCommandList *);
    void pass(ID3D12GraphicsCommandList *, uint32_t, uint32_t);
    void startTiming(ID3D12GraphicsCommandList *);
    void endTiming(ID3D12GraphicsCommandList *);
    void validateSnapshot();
};
} // namespace lab
