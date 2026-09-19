#pragma once
#include "../gpu_resources.h"
#include "hamiltonian_config.h"
#include <DirectXMath.h>
#include <array>
#include <bit>
#include <ostream>

namespace lab {
struct FluidSystemDesc;
struct FluidGpuView;
// GPU-resident canonical wave state. No Python, CUDA, host FFT or per-step readback.
// All record methods change compute bindings; callers rebind their own root.
class HamiltonianWave {
  public:
    static constexpr uint32_t defaultResolution = 32, defaultFftSize = 2 * defaultResolution, layers = 8;
    const uint32_t resolution, fftSize;
    HamiltonianWave(ID3D12Device *, const std::filesystem::path &, const FluidSystemDesc &,
                    DirectX::XMUINT4 grid);
    void reset(ID3D12GraphicsCommandList *);
    // Called once after the previous frame's fence, before recording any work.
    void prepareTransfers(float seconds, bool resetting);
    void rebase(ID3D12GraphicsCommandList *);
    void advanceFreeWater(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *fluidConstants,
                          uint32_t emissionStart, uint32_t emissionCount, uint32_t emissionSerial);
    uint32_t sourceCapacity() const { return maximumSourceParticles; }
    uint32_t freeWaterSamples() const { return diagnostics[11]; }
    uint32_t emittedSamples() const { return diagnostics[12]; }
    uint32_t rejectedSamples() const { return diagnostics[17]; }
    uint32_t activeColumns() const { return diagnostics[18]; }
    uint32_t regionChanges() const { return diagnostics[20]; }
    int32_t receivedSamples() const { return std::bit_cast<int32_t>(diagnostics[13]); }
    double addedVolume() const { return double(receivedSamples()) * particleVolume; }
    float depth() const { return initialDepth + float(addedVolume() / (double(settings.domain.z) * settings.domain.w)); }
    float sampleVolume() const { return particleVolume; }
    float fillLimit() const { return maximumFillDepth; }
    bool full() const { return emittedSamples() >= maximumSourceParticles || rejectedSamples() != 0; }
    float publishedMeanHeight() const { return std::bit_cast<float>(diagnostics[10]); }
    void snapshot(ID3D12GraphicsCommandList *);
    void advance(ID3D12GraphicsCommandList *);
    void couple(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *fluidConstants);
    void reseed(ID3D12GraphicsCommandList *, const FluidGpuView &, ID3D12Resource *fluidConstants,
                bool initialize = false);
    void recordReadback(ID3D12GraphicsCommandList *);
    void collect(); // only after the renderer's existing fence
    void report(std::ostream &) const;
    uint32_t activeSamples() const { return diagnostics[9] + diagnostics[11] + std::min(diagnostics[0], diagnostics[1] - diagnostics[16]); }
    ID3D12Resource *surface() const { return published.resource.Get(); }
    D3D12_GPU_VIRTUAL_ADDRESS constants() const { return uniforms.resource->GetGPUVirtualAddress(); }
    DirectX::XMFLOAT3 minimum, maximum;
    // Used by the independent GPU numerical fixture, read only after completion.
    ID3D12Resource *canonicalState() const { return pool.resource.Get(); }
    uint64_t steps = 0;

  private:
    struct Constants {
        DirectX::XMFLOAT4 domain, physics, coupling, local;
        DirectX::XMUINT4 grid;
        DirectX::XMFLOAT4 fill; // horizontal/vertical transfer rates, pending mean rise/local volume
        DirectX::XMFLOAT4 mass; // particle volume, GPU mass-ledger snapshot, patch snapshot, reserved
        DirectX::XMFLOAT4 spectrum; // wind speed, optical horizon continuation, minimum wavelength, reserved
    };
    HamiltonianConfig config;
    Constants settings{};
    DirectX::XMUINT4 simulationGrid{};
    uint32_t capacity = 0, seedCandidates = 0;
    bool history = false, readable = false;
    double inletVolume = 0;
    float initialDepth = 0, bottom = 0, maximumFillDepth = 0, frameRise = 0;
    float particleVolume = 0;
    uint32_t maximumSourceParticles = 0;
    enum Slot : uint32_t {
        Eta,
        Psi,
        OldH,
        OldP,
        V,
        Px,
        Pz,
        Hx,
        Hz,
        EtaV,
        G1,
        G2,
        Dno,
        Bpsi,
        T0,
        T1,
        T2,
        T3,
        NLH,
        NLP,
        SavedV,
        Lift,
        RealH,
        RealP,
        RealPx,
        RealPz,
        RealHx,
        RealHz,
        RealV,
        ProductA,
        ProductB,
        ProductOut,
        FftScratch,
        Target,
        SmoothTarget,
        VelocityScratch,
        SlotCount = VelocityScratch + layers * 3
    };
    enum Pass : uint32_t {
        Init,
        FFT,
        Operator,
        Product,
        Bernoulli,
        Integrate,
        Publish,
        Snapshot,
        TargetHeight,
        Smooth,
        Relax,
        ClearFree,
        Cull,
        Seed,
        Validate,
        Emit,
        FreeWater,
        Transfers,
        Activity,
        Regions,
        PassCount
    };
    gpu::Buffer uniforms, pool, published, freeIds, counters, readback;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root, couplingRoot;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, PassCount> pipelines;
    std::array<uint32_t, 24> diagnostics{};
    void bind(ID3D12GraphicsCommandList *, const FluidGpuView * = nullptr, ID3D12Resource * = nullptr);
    void run(ID3D12GraphicsCommandList *, Pass, uint32_t dst = 0, uint32_t a = 0, uint32_t b = 0,
             uint32_t op = 0, float x = 1, float y = 0, uint32_t groups = 0);
    void fft(ID3D12GraphicsCommandList *, Slot dst, Slot src, bool inverse = false);
    void symbol(ID3D12GraphicsCommandList *, Slot dst, Slot src, uint32_t op, float scale = 1);
    void sum(ID3D12GraphicsCommandList *, Slot dst, Slot a, Slot b, float x = 1, float y = 1);
    void product(ID3D12GraphicsCommandList *, Slot dst, Slot a, Slot b);
    void dno(ID3D12GraphicsCommandList *);
    void publish(ID3D12GraphicsCommandList *);
};
} // namespace lab
