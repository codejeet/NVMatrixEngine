#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>
#include <ostream>

namespace lab {
// Compact execution over the existing transfer grid. Storage stays dense;
// occupied-cell and halo coverage do not depend on camera visibility or LOD.
class FluidWork {
  public:
    enum Phase { Classification, P2G, G2P, DensityClassification, DensitySolve, PhaseCount };
    FluidWork(ID3D12Device *, const std::filesystem::path &, ID3D12RootSignature *, DirectX::XMUINT4,
              bool interior, bool ownedParticles = false);
    ID3D12Resource *data() const {
        return work.resource.Get();
    }
    ID3D12Resource *arguments() const {
        return args.resource.Get();
    }
    void beginFrame(ID3D12GraphicsCommandList *, bool reset, bool validate);
    void prepareFaces(ID3D12GraphicsCommandList *);
    void prepareDensity(ID3D12GraphicsCommandList *, bool repair);
    void execute(ID3D12GraphicsCommandList *, ID3D12PipelineState *, bool density);
    void finish(ID3D12GraphicsCommandList *);
    uint32_t begin(ID3D12GraphicsCommandList *, Phase);
    void end(ID3D12GraphicsCommandList *, uint32_t);
    void auditFaces(ID3D12GraphicsCommandList *, ID3D12Resource *counts, ID3D12Resource *quanta,
                    ID3D12Resource *faces);
    void auditDensity(ID3D12GraphicsCommandList *, ID3D12Resource *stencil, ID3D12Resource *repairArguments,
                      bool repair);
    void auditDensityResult(ID3D12GraphicsCommandList *, ID3D12Resource *result, uint32_t iterations,
                            ID3D12Resource *repairArguments = nullptr);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;

  private:
    enum Pass {
        Reset,
        Begin,
        ClearFaces,
        MarkFaces,
        CompactFaces,
        PrepareFaces,
        ClearDensity,
        Density,
        DensityRepair,
        PrepareDensity,
        ZeroPressure,
        ReferenceP2G,
        ReferenceDensity,
        CompareFaces,
        CompareDensity,
        PassCount
    };
    DirectX::XMUINT4 grid{};
    DirectX::XMUINT3 faceGrid{}, densityGrid{};
    uint32_t faceTiles = 0, densityTiles = 0, faceCount = 0, wordCount = 0;
    bool hasInterior = false, initialized = false, indirect = false, audit = false;
    bool faceCaptured = false, densityCaptured = false, densityRepair = false;
    bool faceValidated = false, densityValidated = false, densitySeen = false;
    gpu::Buffer work, args, readback, faceSnapshot, densitySnapshot;
    gpu::Buffer referenceFaces;
    std::array<gpu::Buffer, 2> referencePressure;
    double maxFaceDifference = 0, maxDensityDifference = 0;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, PassCount> pipelines;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> dispatch;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    static constexpr uint32_t maxIntervals = 256;
    std::array<Phase, maxIntervals> intervals{};
    uint32_t intervalCount = 0;
    std::array<uint32_t, 16> metrics{};
    std::array<double, PhaseCount> times{}, totals{};
    uint64_t frames = 0;
    uint64_t faceComparisons = 0, densityComparisons = 0;
    void pass(ID3D12GraphicsCommandList *, Pass, uint32_t groups);
    void copy(ID3D12GraphicsCommandList *, ID3D12Resource *dst, uint64_t &offset, ID3D12Resource *src,
              uint64_t bytes);
    void validateFaces();
    void validateDensity();
};
} // namespace lab
