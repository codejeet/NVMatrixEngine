#pragma once
#include "../gpu_resources.h"
#include "../gpu_submission_profile.h"
#include "camera.h"
#include <array>
#include <ostream>
#include "mesh_sdf.h"
#include "fluid_pressure.h"
#include "fluid_bulk.h"
#include "fluid_resampling.h"
#include "fluid_interior.h"
#include "fluid_colliders.h"
#include "fluid_mac.h"
#include "fluid_work.h"
#include "fluid_cut_cells.h"
#include "fluid_bulk_pressure.h"
#include "fluid_particle_grid_exchange.h"
#include "fluid_uniforms.h"
#if PT_FLUID_CUDA
#include "fluid_cuda.h"
#endif

namespace lab {
enum class FluidTransfer { Apic, Flip };
struct FluidSystemDesc {
    bool cudaBackend = false;            // explicit CUDA backend; particles/transfer remain uniform
    bool cudaGraphs = false;             // optional replay; direct launch remains the reference
    bool cudaGraphicsContext = false;    // explicit CUDA-in-Graphics scheduling experiment
    bool cudaConditionalPressure = true; // only affects captured CUDA MGPCG
    bool cudaMixedPressure = false, cudaForcedFinePressure = false;
    uint32_t cudaPressureBricks = 512, cudaPressureChanges = 64, cudaCgIterations = 32;
    DirectX::XMFLOAT3 minimum{1.82f, .03f, -4.88f}, maximum{5.58f, 4.03f, -1.32f};
    float gridCellSize = .08f, particleRadius = .02f;
    uint32_t maxParticles = 100000, initialParticles = 100000;
    float simulationRate = 120, flipRatio = .95f;
    uint32_t maxSubsteps = 8, pressureIterations = 120;
    FluidPressureMode pressureMode = FluidPressureMode::Uniform;
    uint32_t pressureCycles = 3;
    bool bulkInventory = false;
    bool bulkProjected = false;    // passive transport using canonical cut-pressure flux/capacities
    bool bulkCapacity = false;     // bounded admission of initial/inlet volume, pending kept separate
    bool bulkBounded = false;      // conservative receiver limits; closure/support remain separate
    bool bulkPressure = false;     // filled bulk interiors participate in pressure and velocity coverage
    bool bulkImplicit = false;     // backward-Euler coarse transport, no explicit donor cap
    bool bulkAirExtension = false; // capacity-constrained air-only carrier flux
    bool bulkCoupled = false;      // shared capacity/MAC correction before particle transfer
    uint32_t capacityPressureCycles = 1; // inner multigrid budget; convergence gates unchanged
    uint32_t bulkFixture = 0;
    bool cutCells = false;
    bool cutPressure = false;
    bool cutTimeCentered = false; // time-integrated apertures for moving-boundary pressure
    bool cutKernelCache = true;
    uint32_t cutCellFixture = 0;
    bool adaptiveParticles = false;
    bool ownedParticles = false; // FP64 particle authority and fractional mass consumers
    bool narrowBand = false;     // CUDA flowing particle/grid ownership, opt-in
    bool coarseInterior = false;
    bool adaptiveMac = false;
    bool macMultigrid = false;
    bool deterministicBins = false; // optional reproducible GPU gather order for references
    uint32_t densityIterations = 60;
    bool tiledDensity = false; // exact two-sweep 8x4x4 tiles; scalar fallback for validation
    bool sparseWork = false;
    // SI: density kg/m^3, kinematic viscosity m^2/s, surface tension N/m.
    float density = 998.207f, viscosity = .000001f, surfaceTension = .0728f;
    uint32_t materialTest = 0; // validation only: 1 MAC diffusion, 2 sphere curvature
    DirectX::XMFLOAT3 gravity{0, -9.81f, 0};
    FluidTransfer transfer = FluidTransfer::Apic;
    bool ballisticTest = false;
    uint32_t transferTest = 0; // 1 constant, 2 affine, 3 compression, 4 roundtrip; validation only
    bool roomPool = false;
    float initialDepth = .34f;
};
struct FluidEmitterDesc {
    DirectX::XMFLOAT3 position{-5.62f, 1.85f, 2.4f}, velocity{4.5f, -.4f, 0};
    float radius = .20f; // disc perpendicular to velocity; flow = area * speed
    bool enabled = false;
};
struct FluidMaterialDesc {
    float density = 998.207f, viscosity = 0.000001f, surfaceTension = .0728f, ior = 1.333f;
    DirectX::XMFLOAT3 sigmaA{.2644f, .0565f, .00979f}, sigmaS{.005f, .005f, .005f};
    float anisotropyG = 0;
};
struct FluidParticle {
    // apic0.w stores rest-mass multiplicity; XYZ rows are the APIC matrix.
    DirectX::XMFLOAT4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
static_assert(sizeof(FluidParticle) == 80);
struct FluidGpuView {
    ID3D12Resource *particles, *offsets, *indices, *previousPositions;
    DirectX::XMUINT4 grid;
    ID3D12Resource *colliders, *meshPhi;
    uint32_t colliderCount;
    ID3D12Resource *faces, *material;
    ID3D12Resource *cellQuanta;
    ID3D12Resource *interior, *interiorTotals;
    bool interiorEnabled;
    D3D12_GPU_VIRTUAL_ADDRESS colliderAddress;
};
// Renderer owns queue synchronization. Every record method only appends GPU work;
// no waits or particle mapping. Readback is opt-in for bounded validation only.
class FluidSystem {
  public:
    FluidSystem(ID3D12Device *, const std::filesystem::path &, const FluidSystemDesc &);
    ~FluidSystem() = default;
    FluidSystem(const FluidSystem &) = delete;
    FluidSystem &operator=(const FluidSystem &) = delete;
    void record(ID3D12GraphicsCommandList *, float dt, const Camera &, ID3D12CommandQueue * = nullptr,
                ID3D12CommandAllocator * = nullptr, gpu::SubmissionTimeline * = nullptr);
    void drawDebug(ID3D12GraphicsCommandList *, D3D12_GPU_DESCRIPTOR_HANDLE depth);
    void setColliders(const std::vector<FluidCollider> &, uint32_t discontinuities = 0);
    void setMeshSdf(const MeshSdfAsset &); // immutable collision asset, before first record
    FluidGpuView gpuView() const;
    bool changedThisFrame = false;
    bool resetThisFrame = false;
    void requestReset() {
        resetPending = true;
        emitter.enabled = false;
    }
    void requestStep() {
        singleStep = true;
    }
    bool paused = false, debugVisible = true;
    FluidEmitterDesc emitter;
    // Issued base-mass units/high-water ID, not sample count under resampling.
    // Retained name for existing emitter/whitewater/report compatibility.
    uint32_t activeParticles = 0;
    uint64_t emittedParticles = 0;
    float advancedSeconds = 0;
    bool emitterFull = false;
    float particleVolume = 0;
    uint32_t debugMode = 0; // particles, MAC velocity, pressure, divergence, classification
    bool validatePressureThisFrame = false;
    std::unique_ptr<FluidPressure> pressureSolver;
    std::unique_ptr<FluidMac> mac;
    std::unique_ptr<FluidWork> work;
    bool validateWorkThisFrame = false;
    bool validateMacThisFrame = false;
    std::unique_ptr<FluidBulk> bulk;
    std::unique_ptr<FluidBulkPressure> bulkPressure;
    std::unique_ptr<FluidCutCells> cutCells;
    bool validateCutCellsThisFrame = false;
    std::unique_ptr<FluidResampling> resampling;
    std::unique_ptr<FluidInterior> interior;
    bool validateInteriorThisFrame = false;
    bool validateResamplingThisFrame = false;
    bool validateBulkThisFrame = false;
    void recordValidationReadback(ID3D12GraphicsCommandList *);
    void validateAndReport(std::ostream &); // caller must have completed the submission
    void recordTimings(ID3D12GraphicsCommandList *);
    void collectTimings(uint64_t frequency); // existing renderer fence only
    uint64_t stepCount = 0;
    double simulationMs = 0, droppedSeconds = 0;
    std::array<double, 5> cudaTelemetry() const {
#if PT_FLUID_CUDA
        if (cuda)
            return cuda->telemetry();
#endif
        return {};
    }
    uint32_t activeCells = 0, maxCellOccupancy = 0;
    const FluidSystemDesc &description() const {
        return desc;
    }
    ID3D12Resource *gridOwnedResource() const {
        return desc.narrowBand && exchange ? exchange->gridRead() : nullptr;
    }

  private:
    using Uniforms = FluidSimulationConstants;
    FluidSystemDesc desc;
    gpu::Buffer particles, uniforms, readback, timingReadback;
    gpu::Buffer cellCounts, cellOffsets, cellCursor, sortedIndices, scanBlocks, cellQuanta;
    gpu::Buffer narrowCapacity;
    std::unique_ptr<FluidParticleGridExchange> exchange;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gatherMass;
    FluidParticleGridExchange::View exchangeView() const;
    DirectX::XMUINT4 grid{};
    uint32_t scanBlockCount = 0, faceStride = 0;
    gpu::Buffer faces, faceScratch;
    std::array<gpu::Buffer, 2> pressure;
    gpu::Buffer cellData;
    gpu::Buffer densityData;
    gpu::Buffer densityArguments;
    gpu::Buffer materialData;
    uint32_t viscositySubsteps = 1;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> viscosity, surfaceColor, surfaceCurvature, materialFixture;
    gpu::Buffer previousPositions, solidGrid, colliderData;
    FluidColliderTimeline colliderTimeline;
    uint64_t colliderOffset = 0;
    gpu::Buffer meshData, meshUpload;
    bool meshPending = false, hasRecorded = false;
    bool collidersDirty = false;
    MeshSdfAsset meshAsset;
    uint32_t colliderCount = 0;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> snapshot, bakeSolids, collide;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> densityGather, densityJacobi, densityTileJacobi,
        densityDisplace, densityMeasure;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> densityGatherAdaptive, densityClearArguments,
        densityContinueArguments, densityPrepareArguments;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> classify, forces, divergence, jacobi, project, measure;
    uint32_t pressureIndex = 0;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> p2g;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> g2p, extrapolate;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> binDispatch;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> initialize, integrate, debug;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> emit;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clearBins, countBins, scanCells, scanSums, finishScan,
        scatter, sortBins;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries;
    double accumulator = 0;
    double emissionRemainder = 0;
    DirectX::XMUINT4 initialLattice{};
    DirectX::XMFLOAT3 initialMinimum{}, initialMaximum{};
    bool resetPending = true, singleStep = false, readbackReady = false;
    bool bulkSourcesPending = false;
    void bind(ID3D12GraphicsCommandList *);
    void bin(ID3D12GraphicsCommandList *, ID3D12Resource *arguments = nullptr);
    void projectGrid(ID3D12GraphicsCommandList *);
    void transferToGrid(ID3D12GraphicsCommandList *, bool binsCurrent = false);
    void transferToParticles(ID3D12GraphicsCommandList *);
    void correctDensity(ID3D12GraphicsCommandList *);
    void repairDensity(ID3D12GraphicsCommandList *, bool currentBins = false);
    void diffuseVelocity(ID3D12GraphicsCommandList *);
#if PT_FLUID_CUDA
    // Declared last: drain CUDA before releasing the shared DX12 resources.
    std::unique_ptr<FluidCuda> cuda;
    bool cudaFacesSwapped = false;
#endif
};
} // namespace lab
