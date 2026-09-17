#pragma once
#include "deep_pool.h"
#include "scene.h"
#include "streamline.h"
#include "gameplay.h"
#include "gpu_resources.h"
#include "fluid/fluid_system.h"
#include "fluid/fluid_surface.h"
#include "fluid/fluid_complexity.h"
#include "fluid/whitewater.h"
#include "optical_importance.h"
#include "experience.h"
#include "fluid/fluid_buoyancy.h"
#include "latency_profile.h"
#include <wrl/client.h>
#include <d3d12sdklayers.h>
#include <chrono>
#include <cmath>
namespace lab {
class Hud;
using Microsoft::WRL::ComPtr;
struct Options {
    uint32_t width = 1280, height = 720, photons = 65536, frames = 0, history = 32;
    int quality = 0;
    std::string frameGeneration = "auto";
    bool frameGenerationTest = false;
    bool dred = false;
    std::string ser = "auto", atomics = "auto", pose = "initial";
    bool debug = false, animate = false, capture = false, fixture = false, gameplayTest = false;
    bool temporalTest = false, orbitTest = false, rollingTest = false;
    bool fluidTemporalTest = false;
    bool water = true, flatWater = false, lasers = true, haze = true;
    bool restirPt = false;
    bool opticalImportance = false, adaptiveRays = false, opticalValidate = false;
    bool opticalUniform = false, opticalFreeze = false, retracePrimary = false;
    bool opticalControlsTest = false;
    bool opticalEstimatorTest = false;
    uint32_t opticalSamples = 4, opticalView = 0;
    bool fluidFullBrickTraversal = false;
    bool fluidAdaptive = false, fluidComplexityValidate = false, fluidComplexityFreeze = false;
    bool fluidResample = false, fluidResampleValidate = false;
    bool fluidOwnedParticles = false;
    bool fluidNarrowBand = false;
    bool fluidInterior = false, fluidInteriorValidate = false, fluidInteriorCycle = false;
    bool fluidInteriorWake = false;
    bool fluidMac = false, fluidMacValidate = false, fluidMacView = false;
    bool fluidMacCycle = false;
    bool fluidMacMultigrid = false;
    bool fluidMacSplitCoarse = false;
    bool fluidDeterministicBins = false;
    float fluidDepth = .34f, fluidGravity = 9.81f;
    bool fluidComplexityControlsTest = false;
    uint32_t fluidComplexityView = 0;
    uint32_t ptSpatial = 1, ptHistory = 8;
    float laserNm = 532;
    bool fluid = false, fluidValidate = false, gpuValidation = false;
    bool fluidRoom = false, fluidEmitter = false, fluidRoomTest = false, whitewater = true;
    bool fluidDeepPool = false;
    uint32_t fluidCapacity = 500000;
    float fluidCellSize = .16f, fluidSimulationHz = 120;
    bool fisheye = false;
    bool boat = false, experienceTest = false;
    uint32_t fluidParticles = 100000;
    uint32_t fluidTransferTest = 0;
    uint32_t fluidPressureIterations = 120;
    bool fluidSparseWork = false, fluidWorkValidate = false, fluidWorkView = false;
    bool fluidSurfaceLod = false, fluidSurfaceLodValidate = false, fluidSurfaceLodView = false;
    bool fluidSurfaceLodFine = false;
    uint32_t fluidSurfaceLodAxes = 7;
    int32_t fluidDensityIterations = -1; // auto: 120 for precise mixed pressure, otherwise 60
    bool fluidDensityScalar = false;     // reference two-dispatch Jacobi schedule
    std::string fluidPressure = "uniform";
    std::string fluidBackend = "dx12";
    bool fluidCudaGraphs = false;
    bool fluidCudaGraphicsContext = false;
    bool fluidCudaConditionalPressure = true;
    std::string fluidCudaPressure = "uniform"; // uniform baseline, fine or mixed MGPCG
    uint32_t fluidCudaPressureBricks = 512, fluidCudaPressureChanges = 64, fluidCudaCgIterations = 32;
    bool profileLatency = false;
    bool profileFluidBursts = false;
    uint32_t fluidPressureCycles = 3;
    bool fluidPressureValidate = false;
    bool fluidBulk = false, fluidBulkValidate = false;
    bool fluidBulkProjected = false;
    bool fluidBulkCapacity = false;
    bool fluidBulkBounded = false;
    bool fluidCutTimeCentered = false;
    bool fluidBulkPressure = false;
    bool fluidBulkImplicit = false;
    bool fluidBulkAirExtension = false;
    bool fluidBulkCoupled = false;
    uint32_t fluidCapacityCycles = 1;
    uint32_t fluidBulkFixture = 0, fluidBulkView = 0;
    bool fluidCutCells = false, fluidCutValidate = false, fluidCutView = false;
    bool fluidCutPressure = false;
    bool fluidCutKernelCache = true;
    uint32_t fluidCutFixture = 0;
    float fluidViscosity = .000001f, fluidSurfaceTension = .0728f;
    uint32_t fluidMaterialTest = 0;
    bool fluidBallistic = false, fluidFlip = false;
    bool fluidControlTest = false;
    bool fluidSolverOnly = false;
    bool fluidView = false;
    bool fluidIsotropic = false;
    bool fluidColliderTest = false;
    bool fluidSurfaceControlsTest = false;
    uint32_t fluidSurfaceFixture = 0;
};
class Renderer {
  public:
    Renderer(HWND window, const std::filesystem::path &folder, const Options &options);
    ~Renderer();
    void render(float angle, float azimuth, float elevation, Game *game = nullptr, Hud *hud = nullptr,
                float delta = 1.f / 60);
    const Camera &viewCamera() const {
        return previousCamera;
    }
    ExperienceSettings experience;
    XMFLOAT3 displayRay(float x, float y) const;
    void applyWaterSettings();
    ID3D12Device *uiDevice() const {
        return device.Get();
    }
    float receiverWatts = 0;
    float laserWavelength = 532;
    std::unique_ptr<FluidSystem> fluid;
    std::unique_ptr<FluidSurface> fluidSurface;
    std::unique_ptr<FluidComplexity> fluidComplexity;
    std::unique_ptr<Whitewater> whitewater;
    std::unique_ptr<FluidBuoyancy> buoyancy;
    bool roomLiquid() const {
        return options.fluidRoom;
    }
    void toggleWallWater() {
        if (fluid && options.fluidRoom && !fluid->emitterFull)
            fluid->emitter.enabled = !fluid->emitter.enabled;
    }
    bool nearWallValve(const Game &game) const {
        auto p = game.playerPosition();
        if (options.fluidDeepPool)
            return std::hypot(p.x - deepPool::inlet.x, p.z - deepPool::inlet.z) < 2.2f;
        return options.fluidRoom && std::hypot(p.x + 5.6f, p.z - 2.4f) < 2.2f;
    }
    void toggleFluidView() {
        if (fluidSurface) {
            options.fluidView = !options.fluidView;
            reset = true;
        }
    }
    bool inspectingFluid() const {
        return options.fluidView;
    }
    void cycleFluidSurfaceDebug() {
        if (fluidSurface) {
            fluidSurface->debugMode = (fluidSurface->debugMode + 1) % 4;
            reset = true;
        }
    }
    bool hasOpticalImportance() const {
        return optical != nullptr;
    }
    uint32_t opticalDebugMode() const {
        return options.opticalView;
    }
    bool opticalBudgetsFrozen() const {
        return options.opticalFreeze;
    }
    void cycleOpticalDebug() {
        if (optical) {
            options.opticalView = (options.opticalView + 1) % 7;
            reset = true;
        }
    }
    void toggleOpticalFreeze() {
        if (optical)
            options.opticalFreeze = !options.opticalFreeze;
    }
    void resize(uint32_t width, uint32_t height);
    bool waitForFrame();
    void beginSimulation() {
        dlss.beginFrame(frame);
    }
    void endSimulation() {
        dlss.marker(sl::PCLMarker::eSimulationEnd);
    }
    void notifyInput(UINT msg, bool click = false) {
        dlss.inputMessage(msg, click);
    }
    void suspendFrameGeneration() {
        dlss.suspend();
    }
    void setFrameGeneration(uint32_t multiplier);
    const Streamline &streamlineState() const {
        return dlss;
    }
    void report(const std::filesystem::path &path);
    void finishFrameProfile(double wholeFrameMs, double applicationElapsedMs) {
        if (latency)
            latency->finish(wholeFrameMs, applicationElapsedMs);
    }
    void capture(const std::filesystem::path &prefix);
    uint32_t frame = 0, historyResets = 0, rrHistoryResets = 0, fluidHistoryResets = 0;
    double photonMs = 0, accumulationMs = 0, cameraMs = 0, compositeMs = 0, rrMs = 0, frameMs = 0;

  private:
    void createFluid();
    Lens displayedLens;
    bool previousFirstPerson = false;
    using Buffer = gpu::Buffer;
    struct Pipeline {
        ComPtr<ID3D12StateObject> state;
        Buffer table;
    };
    Options options;
    std::filesystem::path folder;
    // Interposer must outlive every proxy. Explicit shutdown happens in destructor body.
    Streamline dlss;
    HWND window = nullptr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12Device> proxyDevice;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList4> commands;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> infoQueue;
    HANDLE event = nullptr;
    HANDLE frameLatencyEvent = nullptr;
    UINT swapchainFlags = 0;
    uint64_t fenceValue = 0, frequency = 0;
    ComPtr<ID3D12DescriptorHeap> heap, rtvHeap;
    uint32_t descriptorSize = 0, rtvSize = 0;
    std::array<ComPtr<ID3D12Resource>, 2> backbuffers;
    std::array<ComPtr<ID3D12Resource>, 8> guides;
    ComPtr<ID3D12Resource> caustics, fluidCaustics, surfaceMap;
    ComPtr<ID3D12Resource> fgHudless, fgUi, fgDepth;
    Buffer fluidPhotonSum;
    Buffer ptReservoirs, ptSurfaces, ptNeighborOffsets;
    std::unique_ptr<OpticalImportance> optical;
    uint64_t ptAllocatedBytes = 0;
    uint32_t ptTemporalFrames = 0, ptStableFrames = 0;
    uint64_t lastPtTransportHash = 0;
    std::vector<std::array<double, 2>> ptTimings;
    MeshSdfAsset fluidPrismSdf;
    Buffer uniforms, vertices, objects, cieData, instances, photonSum, beams, stats, statsReadback, tlas,
        tlasScratch;
    std::vector<Buffer> blas, blasScratch;
    std::vector<Mesh> meshes;
    std::vector<Object> sceneObjects;
    std::vector<XMFLOAT4X4> transportAnchors;
    std::vector<float> transportRadii;
    ComPtr<ID3D12RootSignature> root, presentRoot;
    Pipeline transport;
    ComPtr<ID3D12PipelineState> clear, accumulate, composite, present, fgComposite, fgPrepareDepth;
    ComPtr<ID3D12QueryHeap> queries;
    Buffer timingReadback;
    Camera previousCamera{};
    CameraFollow cameraFollow;
    uint32_t renderWidth = 0, renderHeight = 0, lastBuffer = 0, hitMode = 0;
    bool nvInitialized = false, floatAtomics = false, reorder = false, reset = true;
    std::string adapterName;
    uint32_t shaderModel = 0, raytracingTier = 0;
    bool standardReorders = false, nvReorders = false;
    uint64_t lastHash = 0, lastLightHash = 0, lastFluidLightHash = 0, stackBytes = 0;
    float lastPrismAngle = 0;
    std::array<uint32_t, 43> counters{};
    uint64_t cameraGlassPixels = 0, cameraTransmissions = 0, cameraTir = 0, cameraReflections = 0;
    // Preserve the first six timing columns for existing capture consumers.
    std::vector<std::array<double, 11>> samples;
    std::unique_ptr<LatencyProfile> latency;
    std::array<uint64_t, 4> uiGeometryUploads{};
    Buffer buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_GENERIC_READ);
    ComPtr<ID3D12Resource> texture(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                   D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(uint32_t index) const;
    void wait(gpu::SubmissionTimeline * = nullptr);
    void begin();
    void submit(bool presentFrame, gpu::SubmissionTimeline * = nullptr);
    void checkDebug();
    void capabilities();
    void pipelines();
    void loadScene();
    void targets();
    void createSwapchain();
    void buildTlas();
    void bind();
    void dispatch(uint32_t raygen, uint32_t width, uint32_t height);
};
} // namespace lab
