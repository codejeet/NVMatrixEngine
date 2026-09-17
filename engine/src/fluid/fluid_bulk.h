#pragma once
#include "../gpu_resources.h"
#include "camera.h"
#include "fluid_volume_allocator.h"
#include "fluid_flux_limiter.h"
#include "fluid_implicit_transport.h"
#include "fluid_carrier_projection.h"
#include <memory>
#include <array>
#include <ostream>

namespace lab {
struct FluidSystemDesc;
struct FluidGpuView;
struct FluidCutCellGpuView;
struct FluidBulkGpuView {
    ID3D12Resource *inventory, *sourceLedger, *faceRates;
    DirectX::XMUINT4 grid;
    DirectX::XMFLOAT4 minimumSpacing, maximumDensity;
    bool preciseFaceRates;          // double instead of float when consuming canonical projected flux
    ID3D12Resource *pendingSources; // optional source requests, never additional resident liquid
    bool preciseInventory = false;  // FP64 resident, ledger, pending and shared transfers
    // UAV state. inventory.xyz = volume-weighted velocity, .w = rest volume.
    // Not additional physical mass or particle-free ownership. Optional bulk
    // pressure coupling reads this state; default use remains a passive replica.
    // Multiplying xyz/w by density gives momentum/mass.
};
class FluidBulk {
  public:
    FluidBulk(ID3D12Device *, const std::filesystem::path &, const FluidSystemDesc &,
              DirectX::XMUINT4 fineGrid, float particleVolume);
    void beginFrame(ID3D12GraphicsCommandList *, const Camera &, uint32_t first, uint32_t count, bool reset,
                    bool validate, uint32_t activeParticles);
    void sources(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *solids);
    void allocateSources(ID3D12GraphicsCommandList *, bool advance);
    void advance(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *solids,
                 ID3D12Resource *cells);
    void setProjection(const FluidCutCellGpuView &, ID3D12Resource *canonicalFlux);
    ID3D12Resource *projectCapacity(ID3D12GraphicsCommandList *, ID3D12Resource *cells,
                                    const FluidMacConstraintView &);
    void auditCapacityApplication(ID3D12GraphicsCommandList *, ID3D12Resource *faces,
                                  ID3D12Resource *canonical);
    void finishFrame(ID3D12GraphicsCommandList *);
    void collect(uint64_t frequency);
    void drawDebug(ID3D12GraphicsCommandList *);
    void report(std::ostream &) const;
    FluidBulkGpuView gpuView() const;
    uint32_t debugMode = 0; // off, rest-volume fraction, bulk speed (x-ray wires)
    double gpuMs = 0;
    bool validated = false;
    bool validateImplicitThisFrame = false;

  private:
    struct Uniforms {
        DirectX::XMFLOAT4 minimumCell, maximumDensity;
        DirectX::XMUINT4 fine, coarse;
        DirectX::XMFLOAT4 physical; // dt, particle volume, fixture, debug mode
        DirectX::XMUINT4 source;
        DirectX::XMFLOAT4X4 viewProjection;
    } constants{};
    struct Metrics {
        DirectX::XMFLOAT4 inventory, ledger, detail;
        DirectX::XMUINT4 counts;
    } metrics{};
    static_assert(sizeof(Uniforms) == 160 && sizeof(Metrics) == 64);
    gpu::Buffer uniforms, state[2], ledger, rates, flux, limiter, partials, statistics, counters;
    gpu::Buffer readback, snapshot;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear, seed, restrictFaces, force, limit, transfer, update,
        reduce, totals, debug;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    uint32_t current = 0, faceStride = 0, groups = 0, queryPairs = 0, advances = 0;
    uint64_t steps = 0, frames = 0, sourceCalls = 0, resets = 0, allocatedBytes = 0;
    double totalMs = 0, expectedVolume = 0, volumeError = 0, momentumError = 0;
    bool validateFrame = false;
    bool projected = false, swept = false, pressureSupport = false;
    bool coupledPressure = false;
    std::unique_ptr<FluidVolumeAllocator> allocator;
    std::unique_ptr<FluidFluxLimiter> phaseLimiter;
    std::unique_ptr<FluidImplicitTransport> implicitTransport;
    std::unique_ptr<FluidCarrierProjection> carrierProjection;
    FluidCutCellGpuView cutGeometry{};
    uint32_t allocationPair = UINT_MAX;
    uint32_t rateBytes = 4, stateBytes = 16, capacityBytes = 8;
    ID3D12Resource *projectedFlux = nullptr, *fineVolume = nullptr, *coarseVolume = nullptr;
    ID3D12Resource *coarseArea = nullptr;
    double restrictionError = 0, capacityRestrictionError = 0;
    double forceError = 0;
    uint64_t forceSnapshotOffset = 0;
    void bindProjection(ID3D12GraphicsCommandList *, bool graphics = false);
    void bind(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *);
    void pass(ID3D12GraphicsCommandList *, ID3D12PipelineState *, uint32_t);
    void startTiming(ID3D12GraphicsCommandList *);
    void endTiming(ID3D12GraphicsCommandList *);
    void validateSnapshot();
};
} // namespace lab
