#pragma once
#include "../gpu_resources.h"
#include <DirectXMath.h>
#include <array>

namespace lab {
// Admission policy for the precise particle/grid owner, not a second inventory.
// Keep the free-surface/solid band particle-owned. Uniform translation is not
// a reason to reject an interior; unresolved velocity and spatial moments are.
class FluidFlowingBand {
  public:
    struct Desc {
        DirectX::XMUINT4 fine{};
        uint32_t maxParticles = 0;
        float velocityError = .025f;   // RMS unresolved velocity, metres/second
        float centroidError = .025f;   // fine-cell units
        float covarianceError = .035f; // squared fine-cell units
        float densityError = .15f;
        uint32_t bandCells = 3, padding = 1, promotionFrames = 3;
    };
    struct View {
        ID3D12Resource *frame = nullptr; // existing FluidSimulationConstants
        ID3D12Resource *particles = nullptr, *quantities = nullptr;
        ID3D12Resource *offsets = nullptr, *indices = nullptr;
        ID3D12Resource *gridQuantity = nullptr;
        ID3D12Resource *fineVolume = nullptr;   // float2 current/previous physical m^3
        ID3D12Resource *coarseVolume = nullptr; // double2, same cut-cell geometry
        ID3D12Resource *solidGrid = nullptr;
        ID3D12Resource *importance = nullptr; // optional FluidComplexity state
        DirectX::XMUINT4 importanceGrid{};
        D3D12_GPU_VIRTUAL_ADDRESS colliders = 0;
        ID3D12Resource *meshPhi = nullptr;
    };
    struct State {
        DirectX::XMFLOAT4 error;   // unresolved speed, centroid, covariance, angular speed
        DirectX::XMFLOAT4 flow;    // mean velocity, physical volume
        DirectX::XMUINT4 decision; // request, rejection bits, eligible frames, population
    };
    static_assert(sizeof(State) == 48);
    enum Rejection : uint32_t {
        Invalid = 1,
        SurfaceBand = 2,
        SolidBand = 4,
        VelocityDetail = 8,
        SpatialDetail = 16,
        PhysicsImportance = 32,
        Padding = 64,
        PromotionDelay = 128,
        ForcedParticles = 256
    };
    static constexpr uint32_t siteStride = 64;
    FluidFlowingBand(ID3D12Device *, const std::filesystem::path &, const Desc &);
    // Forced restoration uses only current geometry and the previous decisions;
    // it does not read bins that may have been invalidated by a deposit.
    void record(ID3D12GraphicsCommandList *, const View &, bool reset, bool forceParticles = false);
    ID3D12Resource *requests() const {
        return request.resource.Get();
    }
    ID3D12Resource *sites() const {
        return points.resource.Get();
    }
    ID3D12Resource *siteCounts() const {
        return pointCounts.resource.Get();
    }
    ID3D12Resource *state() const {
        return history.resource.Get();
    }

  private:
    struct Constants {
        DirectX::XMUINT4 coarse{}, importanceGrid{}, control{}, policy{};
        DirectX::XMFLOAT4 tolerance{};
    } constants{};
    Desc desc;
    gpu::Buffer fineState, candidate, history, request, points, pointCounts;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 5> pipelines;
    bool initialized = false;
};
} // namespace lab
