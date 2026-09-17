#pragma once
#include "fluid_surface.h"

namespace lab {
// Heuristic importance requests. Optional particle resampling consumes physics
// LOD; optional FluidMac consumes the same requests independently. Surface
// reconstruction also consumes optical/visibility and disturbance metrics.
// Ray PDFs are still unchanged here.
struct FluidComplexityDesc {
    float responseSeconds = .20f;
    float vorticityScale = 8, gradientScale = 8, accelerationScale = 15;
    float wakeSeconds = .35f, leadSeconds = .15f, solidMargin = .16f;
    DirectX::XMFLOAT3 promote{.75f, .45f, .15f}, demote{.55f, .30f, .08f};
};
struct ComplexityBrick {
    DirectX::XMFLOAT4 importance; // physics, surface, optical prior, visibility
    DirectX::XMFLOAT4 dynamics;   // temporal change, curl (/s), gradient (/s), acceleration (m/s^2)
    DirectX::XMFLOAT4 velocity;   // mass-weighted mean MAC velocity, rest-mass units
    DirectX::XMUINT4 state;       // requested physics/surface LOD, flags, particle count
};
static_assert(sizeof(ComplexityBrick) == 64);
struct FluidComplexityGpuView {
    ID3D12Resource *state, *lists, *active, *arguments;
    DirectX::XMFLOAT4 minimumSpacing;
    DirectX::XMUINT4 grid;
    // Lists: four physics partitions then four surface partitions, capacity each.
    // Dispatch arguments: eight 12-byte records (one group per brick); draw at 96.
    // Buffers are left in UAV state. Consumers must order/transition them before
    // SRV reads or ExecuteIndirect. minimumSpacing.w is the render-node spacing;
    // a complexity brick spans eight nodes' cell spacing (four MAC cells).
};
class FluidComplexity {
  public:
    FluidComplexity(ID3D12Device *, const std::filesystem::path &, const FluidSurface &,
                    const FluidComplexityDesc & = {});
    void record(ID3D12GraphicsCommandList *, const FluidSystem &, const FluidSurface &, const Camera &,
                float dt);
    void drawDebug(ID3D12GraphicsCommandList *);
    void recordReadback(ID3D12GraphicsCommandList *, bool validate);
    void collect(uint64_t frequency);
    void report(std::ostream &) const;
    FluidComplexityGpuView gpuView() const;
    void setOpticalFeedback(ID3D12Resource *resource, bool ready) {
        opticalFeedback = resource;
        opticalFeedbackReady = ready;
    }
    const char *viewName() const;
    uint32_t debugMode = 0; // off + ten false-color views
    bool frozen = false;
    double classificationMs = 0, schedulingMs = 0;
    std::array<uint32_t, 24> counts{};

  private:
    struct Uniforms {
        DirectX::XMFLOAT4X4 viewProjection;
        DirectX::XMFLOAT4 minimumSpacing, simulationMinimumCell;
        DirectX::XMUINT4 bricks, grid;
        DirectX::XMFLOAT4 cameraPosition, cameraForward;
        DirectX::XMFLOAT4 timing; // dt, response seconds, initialized, reset
        DirectX::XMFLOAT4 scales; // curl, gradient, acceleration, solid margin
        DirectX::XMFLOAT4 promote, demote;
        DirectX::XMFLOAT4 wake;   // history seconds, forward seconds, unused, unused
        DirectX::XMUINT4 control; // colliders, debug view, reserved
    };
    FluidComplexityDesc desc;
    DirectX::XMFLOAT4 minimumSpacing{};
    DirectX::XMUINT4 bricks{};
    gpu::Buffer uniforms, raw, state, lists, active, counters, arguments, readback, validation;
    ID3D12Resource *opticalFeedback = nullptr;
    bool opticalFeedbackReady = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear, classify, schedule, prepare, debug;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> draw;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    bool recorded = false, validatePending = false, validated = false;
    bool particleConsumer = false, macConsumer = false, surfaceConsumer = false;
    uint32_t classifications = 0, frozenFrames = 0;
    double totalClassificationMs = 0, totalSchedulingMs = 0;
    uint64_t measurements = 0, allocatedBytes = 0;
};
} // namespace lab
