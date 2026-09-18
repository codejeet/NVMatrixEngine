#pragma once
#include "fluid_system.h"

namespace lab {
struct FluidComplexityGpuView;
// Non-owning, GPU-only reconstruction inputs. The optional phase/plane pair is
// published by the joint CUDA Solver transaction; it describes TOTAL liquid,
// not an additional particle or grid inventory. Caller owns fence/lifetimes.
struct FluidSurfaceInput {
    FluidGpuView view;
    const FluidSystemDesc &description;
    uint64_t stepCount;
    bool changedThisFrame, resetThisFrame;
    float advancedSeconds;
    ID3D12Resource *phase = nullptr, *planes = nullptr;
    ID3D12Resource *gridOwned = nullptr;
    float particleVolume = 0;
    HamiltonianWave *hamiltonian = nullptr;
};
// Sparse active work/page mapping, fixed-capacity backing pool and page table. The canonical render
// surface is trilinear phi (with exact Cartesian-domain clipping for joint phase
// geometry), NOT a distance bound and NOT simulation particles. Domain metadata
// travels with the existing page map to every canonical-field consumer.
class FluidSurface {
  public:
    FluidSurface(ID3D12Device5 *, const std::filesystem::path &, const FluidSystemDesc &);
    void record(ID3D12GraphicsCommandList4 *, const FluidSurfaceInput &, const Camera &, float dt);
    void record(ID3D12GraphicsCommandList4 *cmd, const FluidSystem &system, const Camera &camera, float dt) {
        record(cmd,
               {system.gpuView(), system.description(), system.stepCount, system.changedThisFrame,
                system.resetThisFrame, system.advancedSeconds, nullptr, nullptr, system.gridOwnedResource(),
                system.particleVolume, system.hamiltonian.get()},
               camera, dt);
    }
    void setImportance(const FluidComplexityGpuView &);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    ID3D12Resource *fieldResource() const {
        return field.resource.Get();
    }
    ID3D12Resource *mapResource() const {
        return map.resource.Get();
    }
    D3D12_GPU_VIRTUAL_ADDRESS accelerationStructure() const {
        return blas.resource->GetGPUVirtualAddress();
    }
    DirectX::XMFLOAT4 minimumSpacing{};
    DirectX::XMUINT4 brickGrid{};
    double reconstructionMs = 0, blasMs = 0;
    uint32_t activeBricks = 0, surfaceBricks = 0;
    uint32_t phaseHeightColumns = 0, phaseHeightNodes = 0, phasePlaneNodes = 0;
    uint32_t fixture = 0; // bounded validation: sphere, thin slab, flat box, gently curved box
    bool anisotropic = true;
    bool adaptive = false, forceFine = false, validateLodThisFrame = false;
    bool changedThisFrame = false;
    float lodPhiTolerance = .004f, lodNormalTolerance = .015f;
    uint32_t lodCoarseAxes = 7; // immutable per field: x=1, y=2, z=4
    uint32_t debugMode = 0;     // shaded, normals, brick IDs, material motion, actual sampling LOD
    uint64_t allocatedBytes = 0;
    double renderedVolume = 0;
    void report(std::ostream &) const;

  private:
    struct Uniforms {
        DirectX::XMFLOAT4 minimumSpacing, simulationMinimumCell;
        DirectX::XMUINT4 brickGrid, simulationGrid;
        DirectX::XMFLOAT4 reconstruction; // support, radius, capacity, reserved
        DirectX::XMUINT4 collision;
        DirectX::XMFLOAT4 lodTolerance;
        DirectX::XMUINT4 lodControl;
        DirectX::XMFLOAT4 lodCamera;
        DirectX::XMFLOAT4 phaseControl;
    };
    gpu::Buffer uniforms, field, map, list, aabbs, counts, arguments, blas, scratch, readback;
    gpu::Buffer shapes;
    gpu::Buffer freeHeads, freeNext;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> freeClear, freeBin;
    gpu::Buffer phaseHeights;
    gpu::Buffer lodState, lodNext, lodLists, lodReference, lodSnapshot;
    ID3D12Resource *importance = nullptr;
    ID3D12Resource *previousPhase = nullptr, *previousPlanes = nullptr;
    bool phaseMotion = false;
    bool narrowBand = false;
    std::filesystem::path shaderFolder;
    enum LodPass {
        LodBegin,
        LodFingerprint,
        LodPlan,
        LodFine,
        LodCoarse,
        LodMasks,
        LodAnalyze,
        LodGuard,
        LodRepair,
        LodFinalize,
        LodCommit,
        LodReference,
        LodPassCount
    };
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, LodPassCount> lodPipelines;
    std::array<uint32_t, 20> lodCounts{};
    // Independent same-state audit diagnostics; overlapping rejection reasons.
    std::array<uint32_t, 8> lodAdmission{};
    uint32_t updateNumber = 0;
    uint64_t previousSteps = 0, lodAudits = 0;
    bool lodNeedsUpdate = true, lodSnapshotReady = false;
    DirectX::XMFLOAT3 previousCamera{};
    DirectX::XMUINT3 expectedSimulationGrid{};
    double lodMaxPhiError = 0, lodMaxNormalError = 0, lodBoundaryError = 0, lodMotionBoundaryError = 0;
    void validateLod();
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear, mark, prepare, reconstruct, bounds;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shape;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> phaseMark, phaseReconstruct, phaseHeight;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
    bool readable = false, recorded = false;
};
} // namespace lab
