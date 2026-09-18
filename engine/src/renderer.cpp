#include "renderer.h"
#include "ocean_environment.h"
#include "gpu_diagnostics.h"
#include "hud.h"
#include "water.h"
#include "watercraft.h"
#include <nvapi.h>
#include <nvShaderExtnEnums.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
// Retail SM 6.9: app-local runtime, no Developer Mode or experimental OS flags.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
namespace lab {
namespace {
void check(HRESULT hr, const char *op) {
    if (FAILED(hr)) {
        char code[32];
        sprintf_s(code, "0x%08X", unsigned(hr));
        throw std::runtime_error(std::string(op) + ": " + code);
    }
}
void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    cmd->ResourceBarrier(1, &barrier);
}
void uav(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r = nullptr) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    cmd->ResourceBarrier(1, &b);
}
std::vector<char> bytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Missing shader: " + path.string());
    std::vector<char> data(size_t(in.tellg()));
    in.seekg(0);
    in.read(data.data(), data.size());
    if (!in)
        throw std::runtime_error("Cannot read shader");
    return data;
}
float halton(uint32_t n, uint32_t base) {
    float x = 0, f = 1;
    while (n) {
        f /= base;
        x += f * (n % base);
        n /= base;
    }
    return x;
}
Camera camera(float aspect, float azimuth, float elevation, bool liquid = false, bool room = false,
              bool deep = false, bool large = false, bool ocean = false) {
    Camera c{};
    XMVECTOR target = liquid ? XMVectorSet(3.7f, 1.1f, -3.1f, 1) : XMVectorSet(0, 1.2f, 1, 1);
    if (room)
        target = XMVectorSet(-3.f, 1.1f, 2.4f, 1);
    if (deep)
        target = XMVectorSet(-4, 6, 4, 1);
    if (large)
        target = XMVectorSet(0, largeWater::depth, 2, 1);
    if (ocean) target = XMVectorSet(-20, 7, -16, 1);
    const float radius = ocean ? 68.f : deep ? 43.f : (large ? 12.5f : (room ? 5.5f : (liquid ? 6.f : 10.f)));
    XMVECTOR pos = XMVectorAdd(target, XMVectorSet(radius * std::sin(azimuth) * std::cos(elevation),
                                                   radius * std::sin(elevation),
                                                   -radius * std::cos(azimuth) * std::cos(elevation), 0));
    XMVECTOR forward = XMVector3Normalize(XMVectorSubtract(target, pos)),
             right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward)),
             up = XMVector3Cross(forward, right);
    XMStoreFloat3(&c.position, pos);
    XMStoreFloat3(&c.forward, forward);
    XMStoreFloat3(&c.right, right);
    XMStoreFloat3(&c.up, up);
    XMMATRIX view = XMMatrixLookAtLH(pos, target, up),
             proj = XMMatrixPerspectiveFovLH(XM_PI / 3, aspect, .05f, 200);
    XMStoreFloat4x4(&c.view, view);
    XMStoreFloat4x4(&c.projection, proj);
    XMStoreFloat4x4(&c.viewProjection, view * proj);
    return c;
}
uint64_t hashBytes(const void *data, size_t size, uint64_t value = 14695981039346656037ull) {
    auto p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i)
        value = (value ^ p[i]) * 1099511628211ull;
    return value;
}
} // namespace
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::cpu(uint32_t i) const {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(i) * descriptorSize;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::gpu(uint32_t i) const {
    auto h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(i) * descriptorSize;
    return h;
}
Renderer::Buffer Renderer::buffer(uint64_t size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags,
                                  D3D12_RESOURCE_STATES state) {
    return gpu::buffer(device.Get(), size, type, flags, state);
}
ComPtr<ID3D12Resource> Renderer::texture(uint32_t w, uint32_t h, DXGI_FORMAT format,
                                         D3D12_RESOURCE_FLAGS flags) {
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                          flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                              ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                              : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          nullptr, IID_PPV_ARGS(&r)),
          "Create lab texture");
    return r;
}
Renderer::Renderer(HWND window, const std::filesystem::path &dir, const Options &opts)
    : options(opts), folder(dir), window(window) {
    try {
        if (options.frames > 32)
            samples.reserve(options.frames - 32);
        dlss.init(folder);
        if (options.dred)
            gpu::enableDred();
        if (options.debug) {
            ComPtr<ID3D12Debug> debug;
            check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "Debug layer required for this run");
            debug->EnableDebugLayer();
            if (options.gpuValidation) {
                ComPtr<ID3D12Debug1> validation;
                check(debug.As(&validation), "GPU validation interface");
                validation->SetEnableGPUBasedValidation(TRUE);
            }
        }
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Create factory");
        auto f = factory.Detach();
        dlss.upgrade(reinterpret_cast<void **>(&f));
        factory.Attach(f);
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            check(adapter->GetDesc1(&desc), "Adapter description");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device))))
                break;
            adapter.Reset();
        }
        if (!device)
            throw std::runtime_error("The raygen lab requires an NVIDIA RTX adapter.");
        if (options.profileLatency)
            latency = std::make_unique<LatencyProfile>(adapter.Get(), options.frames);
        for (wchar_t c : desc.Description) {
            if (!c)
                break;
            adapterName += c < 128 ? char(c) : '?';
        }
        if (options.debug)
            check(device.As(&infoQueue), "Debug info queue");
        capabilities();
        dlss.setDevice(device.Get(), desc.AdapterLuid);
        const uint32_t multiplier =
            options.frameGeneration == "auto"
                ? (!options.frames && dlss.fgSupported ? 2u : 1u)
                : (options.frameGeneration == "off" ? 1u : uint32_t(std::stoi(options.frameGeneration)));
        dlss.setMultiplier(multiplier);
        dlss.loadFrameGeneration(multiplier > 1);
        check(device.As(&proxyDevice), "Base device");
        auto d = proxyDevice.Detach();
        dlss.upgrade(reinterpret_cast<void **>(&d));
        proxyDevice.Attach(d);
        D3D12_COMMAND_QUEUE_DESC q{};
        q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(proxyDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Direct queue");
        queue->SetName(L"Lab render and present queue");
        createSwapchain();
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
              "Allocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&commands)),
              "Command list");
        commands->SetName(L"Lab frame / DXR RR FG inputs UI");
        commands->Close();
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            throw std::runtime_error("Fence event failed");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 32;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "Descriptor heap");
        descriptorSize = device->GetDescriptorHandleIncrementSize(hd.Type);
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 4;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "RTV heap");
        rtvSize = device->GetDescriptorHandleIncrementSize(hd.Type);
        uniforms = buffer(768, D3D12_HEAP_TYPE_UPLOAD);
        laserWavelength = options.laserNm;
        beams = buffer(32 * 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        photonSum = buffer(uint64_t(atlasWidth) * chartSize * 12, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        fluidPhotonSum =
            buffer(uint64_t(atlasWidth) * chartSize * 12, D3D12_HEAP_TYPE_DEFAULT,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        fluidPhotonSum.resource->SetName(L"Fluid-owned spectral photon power");
        stats = buffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        statsReadback =
            buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC query{};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = 8;
        check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries)), "Timing queries");
        check(queue->GetTimestampFrequency(&frequency), "Timing frequency");
        timingReadback =
            buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        pipelines();
        loadScene();
        transportAnchors.resize(sceneObjects.size());
        for (const auto &mesh : meshes) {
            float radius2 = 0;
            for (const auto &v : mesh.vertices)
                radius2 = std::max(radius2, v.position.x * v.position.x + v.position.y * v.position.y +
                                                v.position.z * v.position.z);
            transportRadii.push_back(std::sqrt(radius2));
        }
        transportRadii.resize(sceneObjects.size(), 0);
        targets();
        experience.lens.fisheye = options.fisheye;
        experience.particleCapacity = options.fluidCapacity;
        experience.cellSize = options.fluidCellSize;
        experience.deepPool = options.fluidDeepPool;
        experience.largeWaterLab = options.largeWaterLab;
        experience.oceanLab = options.oceanLab;
        if (options.oceanLab) {
            experience.environment = options.oceanNight ? 1 : 0;
            experience.ballFloats = true;
        }
        experience.simulationHz = options.fluidSimulationHz;
        if (options.fluid)
            createFluid();
        checkDebug();
    } catch (...) {
        // Constructor failures still shut Streamline down before member devices/proxies.
        try {
            wait();
            dlss.free();
            dlss.shutdown();
        } catch (...) {
        }
        transport = {};
        if (frameLatencyEvent) {
            CloseHandle(frameLatencyEvent);
            frameLatencyEvent = nullptr;
        }
        if (nvInitialized) {
            NvAPI_Unload();
            nvInitialized = false;
        }
        if (event) {
            CloseHandle(event);
            event = nullptr;
        }
        throw;
    }
}
void Renderer::capabilities() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 rt{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &rt, sizeof(rt)), "DXR query");
    if (rt.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
        throw std::runtime_error("DXR 1.1 required");
    raytracingTier = uint32_t(rt.RaytracingTier);
    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_9};
    check(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)), "Shader model query");
    shaderModel = uint32_t(sm.HighestShaderModel);
    if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_6)
        throw std::runtime_error("Shader Model 6.6 required for the fallback library");
    D3D12_FEATURE_DATA_D3D12_OPTIONS22 caps{};
    standardReorders =
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS22, &caps, sizeof(caps))) &&
        caps.ShaderExecutionReorderingActuallyReorders;
    nvInitialized = NvAPI_Initialize() == NVAPI_OK;
    bool fp32 = false, hit = false;
    if (nvInitialized) {
        NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(device.Get(), NV_EXTN_OP_FP32_ATOMIC, &fp32);
        NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(device.Get(), NV_EXTN_OP_HIT_OBJECT_TRACE_RAY, &hit);
        NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS c{};
        nvReorders =
            hit &&
            NvAPI_D3D12_GetRaytracingCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING,
                                          &c, sizeof(c)) == NVAPI_OK &&
            (c & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD);
    }
    if (options.ser == "auto") {
        // The bounded, split dielectric continuation has substantial live state.
        // Reordering every short visibility ray costs more than it saves here.
        // Keep explicit HitObject/SER modes for the future reservoir pipeline;
        // capability alone is not evidence of a profitable scheduling policy.
        hitMode = 0;
        reorder = false;
    } else if (options.ser == "dxr") {
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_9)
            throw std::runtime_error("Standard HitObject requires SM 6.9");
        hitMode = 2;
        reorder = standardReorders;
    } else if (options.ser == "nvapi") {
        if (!hit)
            throw std::runtime_error("NVAPI HitObject unsupported");
        hitMode = 1;
        reorder = nvReorders;
    } else if (options.ser == "off") {
        hitMode = 0;
        reorder = false;
    } else if (options.ser == "dxr-off") {
        if (sm.HighestShaderModel < D3D_SHADER_MODEL_6_9)
            throw std::runtime_error("SM 6.9 unavailable");
        hitMode = 2;
        reorder = false;
    } else
        throw std::runtime_error("Invalid SER mode");
    floatAtomics = options.atomics == "float" || (options.atomics == "auto" && fp32);
    if (floatAtomics && !fp32)
        throw std::runtime_error("NVAPI fp32 atomics unavailable; use --atomics=fixed");
    logLine("Lab: " + adapterName + "; SM=" + std::to_string(shaderModel) +
            "; DXR=" + std::to_string(raytracingTier) + "; HitObject=" + std::to_string(hitMode) +
            "; actually reorders=" + std::to_string(reorder) + "; fp32=" + std::to_string(floatAtomics));
}
Renderer::~Renderer() {
    try {
        wait();
        dlss.free();
        dlss.shutdown();
    } catch (const std::exception &e) {
        logLine(e.what());
        gpu::reportDred(device.Get());
    }
    transport = {};
    if (frameLatencyEvent)
        CloseHandle(frameLatencyEvent);
    if (nvInitialized)
        NvAPI_Unload();
    if (event)
        CloseHandle(event);
}
void Renderer::wait(gpu::SubmissionTimeline *timeline) {
    if (!queue || !fence)
        return;
    check(queue->Signal(fence.Get(), ++fenceValue), "Signal");
    gpu::stamp(timeline, gpu::SubmissionStage::FrameFenceSignaled);
    if (fence->GetCompletedValue() < fenceValue) {
        check(fence->SetEventOnCompletion(fenceValue, event), "Fence completion");
        gpu::stamp(timeline, gpu::SubmissionStage::FrameEventQueued);
        if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
            throw std::runtime_error("GPU timeout after 15 seconds");
    }
    check(device->GetDeviceRemovedReason(), "GPU status");
    gpu::stamp(timeline, gpu::SubmissionStage::FrameComplete);
}
void Renderer::begin() {
    check(allocator->Reset(), "Reset allocator");
    check(commands->Reset(allocator.Get(), nullptr), "Reset command list");
}
bool Renderer::waitForFrame() {
    if (!frameLatencyEvent)
        return true; // Bounded validation is unpaced.
    // Wait BEFORE simulation, not after sampling input. A message wakes the main
    // loop so resize, focus and close remain responsive, even when minimized.
    DWORD result = MsgWaitForMultipleObjectsEx(1, &frameLatencyEvent, 1000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_FAILED)
        throw std::runtime_error("Frame latency wait failed");
    return result == WAIT_OBJECT_0;
}
void Renderer::submit(bool presentFrame, gpu::SubmissionTimeline *timeline) {
    check(commands->Close(), "Close commands");
    gpu::stamp(timeline, gpu::SubmissionStage::FrameClosed);
    ID3D12CommandList *lists[] = {commands.Get()};
    queue->ExecuteCommandLists(1, lists);
    gpu::stamp(timeline, gpu::SubmissionStage::FrameSubmitted);
    if (presentFrame) {
        dlss.marker(sl::PCLMarker::eRenderSubmitEnd);
        dlss.marker(sl::PCLMarker::ePresentStart);
        // The interactive lifecycle fixture uses the same VSync behavior as
        // actual play; throughput fixtures explicitly remain unpaced.
        const UINT sync =
            (options.frames && !options.frameGenerationTest) || (dlss.fgEnabled && !dlss.vsyncSupported) ? 0
                                                                                                         : 1;
        const UINT flags =
            !sync && (swapchainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        const HRESULT presented = swapchain->Present(sync, flags);
        dlss.marker(sl::PCLMarker::ePresentEnd);
        if (FAILED(presented)) {
            // Do not unwind and destroy HUD uploads while submitted GPU work
            // is still using them, even when presentation itself was rejected.
            try {
                wait();
            } catch (const std::exception &e) {
                logLine(e.what());
                gpu::reportDred(device.Get());
            }
            check(presented, "Present");
        }
        dlss.endPresent(queue.Get());
    }
    gpu::stamp(timeline, gpu::SubmissionStage::Presented);
    wait(timeline);
    checkDebug();
}
void Renderer::checkDebug() {
    if (!infoQueue)
        return;
    std::string error;
    for (UINT64 i = 0; i < infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T n = 0;
        infoQueue->GetMessage(i, nullptr, &n);
        std::vector<char> data(n);
        auto m = reinterpret_cast<D3D12_MESSAGE *>(data.data());
        if (SUCCEEDED(infoQueue->GetMessage(i, m, &n)) && m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            error += m->pDescription;
    }
    infoQueue->ClearStoredMessages();
    if (!error.empty())
        throw std::runtime_error("D3D12 validation: " + error);
}
void Renderer::pipelines() {
    D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 12, 0, 0, 0},
                                       {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 31, 1, 0},
                                       {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 12, 0, 0}};
    D3D12_ROOT_PARAMETER params[21]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 1; i < 5; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = i - 1;
    }
    for (int i = 5; i < 7; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable = {1, &ranges[i - 5]};
    }
    for (int i = 7; i < 9; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = i - 3;
    }
    params[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[9].DescriptorTable = {1, &ranges[2]};
    params[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[10].Descriptor.ShaderRegister = 6;
    for (int i = 11; i < 13; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i + 3;
    }
    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[13].Descriptor.ShaderRegister = 7;
    for (uint32_t i = 14; i < 19; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i + 2;
    }
    params[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[19].Descriptor.ShaderRegister = 8;
    params[20].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[20].Descriptor.ShaderRegister = 9;
    D3D12_ROOT_SIGNATURE_DESC rd{21, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
          "Serialize transport root");
    check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Create transport root");
    auto code = bytes(
        folder / "shaders" /
        ("Transport-" + std::to_string(hitMode) + "-" + std::to_string(floatAtomics ? 1 : 0) + ".dxil"));
    D3D12_DXIL_LIBRARY_DESC lib{{code.data(), code.size()}, 0, nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE gr{root.Get()};
    D3D12_RAYTRACING_SHADER_CONFIG shader{16, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG recursion{1};
    D3D12_HIT_GROUP_DESC hg{L"SurfaceHit", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"Closest", nullptr};
    D3D12_HIT_GROUP_DESC fluidHg{L"FluidHit", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE, nullptr,
                                 L"FluidClosest", L"FluidIntersection"};
    D3D12_HIT_GROUP_DESC secondaryHg{L"WhitewaterHit", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE, nullptr,
                                     L"FluidClosest", L"WhitewaterIntersection"};
    D3D12_STATE_SUBOBJECT sub[] = {{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib},
                                   {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &gr},
                                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader},
                                   {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &recursion},
                                   {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg},
                                   {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &fluidHg},
                                   {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &secondaryHg}};
    D3D12_STATE_OBJECT_DESC state{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 7, sub};
    bool extension = hitMode == 1 || floatAtomics;
    if (extension && NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(device.Get(), 31, 1) != NVAPI_OK)
        throw std::runtime_error("Enable NVAPI shader extension failed");
    HRESULT result = device->CreateStateObject(&state, IID_PPV_ARGS(&transport.state));
    if (extension)
        NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(device.Get(), 0xffffffff, 0);
    check(result, "Create transport raygen pipeline");
    ComPtr<ID3D12StateObjectProperties> props;
    check(transport.state.As(&props), "Raygen properties");
    stackBytes = props->GetPipelineStackSize();
    logLine("Lab pipeline stack bytes: " + std::to_string(stackBytes));
    transport.table = buffer(640, D3D12_HEAP_TYPE_UPLOAD);
    memset(transport.table.mapped, 0, 640);
    const wchar_t *exports[] = {L"PhotonRaygen",     L"CameraRaygen",   L"BeamRaygen",    L"Miss",
                                L"SurfaceHit",       L"FluidHit",       L"WhitewaterHit", L"FluidProbeRaygen",
                                L"PTTemporalRaygen", L"PTSpatialRaygen"};
    for (int i = 0; i < 10; ++i) {
        auto id = props->GetShaderIdentifier(exports[i]);
        if (!id)
            throw std::runtime_error("Missing SBT export");
        memcpy(static_cast<char *>(transport.table.mapped) + i * 64, id, 32);
    }
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &p) {
        auto b = bytes(folder / "shaders" / (std::string(name) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {b.data(), b.size()};
        check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&p)), "Resolve PSO");
    };
    compute("Clear", clear);
    compute("Accumulate", accumulate);
    compute("Composite", composite);
    D3D12_DESCRIPTOR_RANGE sr[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
                                   {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0},
                                   {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
    D3D12_ROOT_PARAMETER pr[4]{};
    for (int i = 0; i < 3; ++i) {
        pr[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        pr[i].DescriptorTable = {1, &sr[i]};
    }
    pr[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    pr[3].Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC pd{4, pr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    blob.Reset();
    error.Reset();
    check(D3D12SerializeRootSignature(&pd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
          "Serialize present root");
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                      IID_PPV_ARGS(&presentRoot)),
          "Present root");
    auto vs = bytes(folder / "shaders/VS.dxil"), ps = bytes(folder / "shaders/PS.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC gp{};
    gp.pRootSignature = presentRoot.Get();
    gp.VS = {vs.data(), vs.size()};
    gp.PS = {ps.data(), ps.size()};
    gp.SampleMask = UINT_MAX;
    gp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gp.RasterizerState.DepthClipEnable = TRUE;
    gp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gp.SampleDesc.Count = 1;
    check(device->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&present)), "Present PSO");
    auto uiPs = bytes(folder / "shaders/FGComposite.dxil");
    gp.PS = {uiPs.data(), uiPs.size()};
    check(device->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&fgComposite)), "FG UI composite PSO");
    auto depthCs = bytes(folder / "shaders/FGDepth.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC fg{};
    fg.pRootSignature = presentRoot.Get();
    fg.CS = {depthCs.data(), depthCs.size()};
    check(device->CreateComputePipelineState(&fg, IID_PPV_ARGS(&fgPrepareDepth)), "FG clip-depth PSO");
    auto distortionCs = bytes(folder / "shaders/FGDistortion.dxil");
    fg.CS = {distortionCs.data(), distortionCs.size()};
    check(device->CreateComputePipelineState(&fg, IID_PPV_ARGS(&fgPrepareDistortion)), "FG lens distortion PSO");
}
void Renderer::createSwapchain() {
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = options.width;
    sc.Height = options.height;
    sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // Streamline's pacer adjusts IDXGISwapChain2::SetMaximumFrameLatency,
    // including in bounded tests. That call requires the waitable flag.
    sc.Flags = (!options.frames || dlss.fgLoaded) ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;
    BOOL tearing = FALSE;
    ComPtr<IDXGIFactory5> tearingFactory;
    if (SUCCEEDED(factory.As(&tearingFactory)) &&
        SUCCEEDED(tearingFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                      sizeof(tearing))) &&
        tearing)
        sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    swapchainFlags = sc.Flags;
    ComPtr<IDXGISwapChain1> swap;
    check(factory->CreateSwapChainForHwnd(queue.Get(), window, &sc, nullptr, nullptr, &swap),
          "Lab swapchain");
    check(swap.As(&swapchain), "Swapchain 3");
    if (sc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        check(swapchain->SetMaximumFrameLatency(1), "Frame latency");
    }
    if (!options.frames) {
        frameLatencyEvent = swapchain->GetFrameLatencyWaitableObject();
        if (!frameLatencyEvent)
            throw std::runtime_error("Frame latency event unavailable");
    }
}
void Renderer::setFrameGeneration(uint32_t multiplier) {
    if (multiplier == dlss.multiplier)
        return;
    dlss.setMultiplier(multiplier); // validate before changing presentation
    dlss.suspend();
    wait();
    dlss.free();
    backbuffers = {};
    swapchain.Reset();
    if (frameLatencyEvent) {
        CloseHandle(frameLatencyEvent);
        frameLatencyEvent = nullptr;
    }
    dlss.loadFrameGeneration(multiplier > 1);
    createSwapchain();
    targets();
}
void Renderer::createFluid() {
    FluidSystemDesc fluidDesc;
    fluidDesc.hamiltonian = options.hamiltonian;
    fluidDesc.cudaBackend = options.fluidBackend == "cuda";
    fluidDesc.cudaGraphs = options.fluidCudaGraphs;
    fluidDesc.cudaGraphicsContext = options.fluidCudaGraphicsContext;
    fluidDesc.cudaConditionalPressure = options.fluidCudaConditionalPressure;
    fluidDesc.cudaMixedPressure = options.fluidCudaPressure != "uniform";
    fluidDesc.cudaForcedFinePressure = options.fluidCudaPressure == "fine";
    fluidDesc.cudaPressureBricks = options.fluidCudaPressureBricks;
    fluidDesc.cudaPressureChanges = options.fluidCudaPressureChanges;
    fluidDesc.cudaCgIterations = options.fluidCudaCgIterations;
    fluidDesc.maxParticles = std::max(1u, options.fluidParticles);
    fluidDesc.initialParticles = options.fluidParticles;
    if (options.fluidRoom) {
        if (options.fluidCapacity < options.fluidParticles)
            throw std::runtime_error("Room liquid capacity is below its initial particle count");
        fluidDesc.roomPool = true;
        fluidDesc.minimum = {-5.95f, .02f, -5.95f};
        fluidDesc.maximum = {5.95f, 3.06f, 7.95f};
        fluidDesc.gridCellSize = options.fluidCellSize;
        fluidDesc.simulationRate = options.fluidSimulationHz;
        fluidDesc.particleRadius = .035f;
        fluidDesc.maxParticles = options.fluidCapacity;
        if (options.largeWaterLab) {
            fluidDesc.minimum = largeWater::minimum;
            fluidDesc.maximum = largeWater::maximum;
        }
        if (options.oceanLab) {
            fluidDesc.minimum = ocean::minimum;
            fluidDesc.maximum = ocean::maximum;
            fluidDesc.particleRadius = .035f * options.fluidCellSize / .16f;
            fluidDesc.density = 1025.f;
            fluidDesc.surfaceCellScale = 1.f;
        }
        if (options.fluidDeepPool) {
            fluidDesc.minimum = deepPool::minimum;
            fluidDesc.maximum = deepPool::maximum;
            fluidDesc.particleRadius = .035f * options.fluidCellSize / .16f;
            // Preallocate the bounded pressure pool for this domain, not the small-room pool.
            if (fluidDesc.cudaMixedPressure && options.fluidCudaPressureBricks == 512) {
                uint32_t bricks = 1;
                for (int a = 0; a < 3; ++a)
                    bricks *= (uint32_t(std::ceil(((&fluidDesc.maximum.x)[a] - (&fluidDesc.minimum.x)[a]) /
                                                  fluidDesc.gridCellSize)) +
                               3) /
                              4;
                fluidDesc.cudaPressureBricks = bricks;
            }
        }
    }
    fluidDesc.transferTest = options.fluidTransferTest;
    if (options.hamiltonian.enabled) {
        fluidDesc.waveMinimum = fluidDesc.minimum;
        fluidDesc.waveMaximum = fluidDesc.maximum;
        // Address the entire basin so separate bodies and disturbances can own
        // independent 3D regions. GPU activity selects actual particle/pressure
        // work; initialParticles remains the equivalent full-room rest density.
    }
    fluidDesc.pressureIterations = options.fluidPressureIterations;
    fluidDesc.densityIterations = options.fluidDensityIterations < 0
                                      ? (options.fluidMacMultigrid ? 120 : 60)
                                      : uint32_t(options.fluidDensityIterations);
    fluidDesc.tiledDensity = options.fluidMacMultigrid && !options.fluidDensityScalar;
    fluidDesc.sparseWork = options.fluidSparseWork;
    fluidDesc.pressureMode = options.fluidPressure == "multigrid" ? FluidPressureMode::Multigrid
                             : options.fluidPressure == "active"  ? FluidPressureMode::Active
                                                                  : FluidPressureMode::Uniform;
    fluidDesc.pressureCycles = options.fluidPressureCycles;
    fluidDesc.bulkInventory = options.fluidBulk;
    fluidDesc.bulkProjected = options.fluidBulkProjected;
    fluidDesc.bulkCapacity = options.fluidBulkCapacity;
    fluidDesc.bulkBounded = options.fluidBulkBounded;
    fluidDesc.bulkFixture = options.fluidBulkFixture;
    fluidDesc.cutCells = options.fluidCutCells;
    fluidDesc.cutPressure = options.fluidCutPressure;
    fluidDesc.cutTimeCentered = options.fluidCutTimeCentered;
    fluidDesc.bulkPressure = options.fluidBulkPressure;
    fluidDesc.bulkImplicit = options.fluidBulkImplicit;
    fluidDesc.bulkAirExtension = options.fluidBulkAirExtension;
    fluidDesc.bulkCoupled = options.fluidBulkCoupled;
    fluidDesc.capacityPressureCycles = options.fluidCapacityCycles;
    fluidDesc.cutKernelCache = options.fluidCutKernelCache;
    fluidDesc.cutCellFixture = options.fluidCutFixture;
    fluidDesc.adaptiveParticles = options.fluidResample;
    fluidDesc.ownedParticles = options.fluidOwnedParticles;
    fluidDesc.narrowBand = options.fluidNarrowBand;
    fluidDesc.coarseInterior = options.fluidInterior;
    fluidDesc.adaptiveMac = options.fluidMac;
    fluidDesc.macMultigrid = options.fluidMacMultigrid;
    fluidDesc.deterministicBins = options.fluidDeterministicBins;
    fluidDesc.initialDepth = options.fluidDepth;
    fluidDesc.gravity.y = -options.fluidGravity;
    fluidDesc.viscosity = options.fluidViscosity;
    fluidDesc.surfaceTension = options.fluidSurfaceTension;
    fluidDesc.materialTest = options.fluidMaterialTest;
    fluidDesc.ballisticTest = options.fluidBallistic;
    fluidDesc.transfer = options.fluidFlip ? FluidTransfer::Flip : FluidTransfer::Apic;
    fluid = std::make_unique<FluidSystem>(device.Get(), folder, fluidDesc);
    if (options.fluidWorkView)
        fluid->debugMode = 7;
    if (fluid->mac)
        fluid->mac->debugVisible = options.fluidMacView;
    if (fluid->mac)
        fluid->mac->splitCoarse = options.fluidMacSplitCoarse;
    if (fluid->bulk)
        fluid->bulk->debugMode = options.fluidBulkView;
    if (fluid->cutCells)
        fluid->cutCells->debugVisible = options.fluidCutView;
    fluid->emitter.enabled = options.fluidEmitter;
    if (options.largeWaterLab)
        fluid->emitter.position = largeWater::inlet;
    if (options.oceanLab)
        fluid->emitter.position = ocean::inlet;
    if (options.fluidDeepPool) {
        fluid->emitter.position = deepPool::inlet;
        fluid->emitter.radius = .32f;
    }
    if (!options.fluidSolverOnly) {
        std::vector<XMFLOAT3> triangleSoup;
        for (const auto &v : meshes[2].vertices)
            triangleSoup.push_back(v.position);
        fluidPrismSdf = MeshSdfAsset::bake(triangleSoup, .04f);
        auto collisionSdfs = fluidPrismSdf;
        if (options.boat) {
            auto hull = watercraft::hullTriangles();
            fluidBoatSdf = MeshSdfAsset::bake({hull.begin(),hull.end()}, .06f);
            collisionSdfs.phi.insert(collisionSdfs.phi.end(),fluidBoatSdf.phi.begin(),fluidBoatSdf.phi.end());
        }
        fluid->setMeshSdf(collisionSdfs);
        fluidSurface = std::make_unique<FluidSurface>(device.Get(), folder, fluidDesc);
        fluidSurface->fixture = options.fluidSurfaceFixture;
        fluidSurface->anisotropic = !options.fluidIsotropic && !options.hamiltonian.enabled;
        fluidSurface->adaptive = options.fluidSurfaceLod;
        fluidSurface->forceFine = options.fluidSurfaceLodFine;
        fluidSurface->lodCoarseAxes = options.fluidSurfaceLodAxes;
        if (options.fluidSurfaceLodView)
            fluidSurface->debugMode = 4;
        if (options.fluidAdaptive) {
            fluidComplexity = std::make_unique<FluidComplexity>(device.Get(), folder, *fluidSurface);
            fluidComplexity->debugMode = options.fluidComplexityView;
            fluidComplexity->frozen = options.fluidComplexityFreeze;
        }
        if (options.boat)
            buoyancy = std::make_unique<FluidBuoyancy>(device.Get(), folder, options.hamiltonian.enabled);
        if (options.fluidRoom && options.whitewater)
            whitewater = std::make_unique<Whitewater>(device.Get(), folder, *fluidSurface, *fluid);
        if (options.fluidSurfaceFixture)
            fluid->paused = true;
        fluid->debugVisible = options.fluidWorkView;
    }
}
void Renderer::applyWaterSettings() {
    experience.rebuildWater = false;
    if (!options.fluidRoom || !fluid)
        return;
    wait(); // Explicit apply/reset only, never while dragging a slider.
    dlss.suspend();
    options.fluidCapacity =
        std::clamp(experience.particleCapacity, std::max(100000u, options.fluidParticles), 1000000u);
    options.fluidCellSize = std::clamp(experience.cellSize, options.oceanLab ? .8f : options.fluidDeepPool ? .5f : (options.largeWaterLab ? .20f : .10f),
                                       options.oceanLab ? 1.2f : options.fluidDeepPool ? 1.f : (options.largeWaterLab ? .32f : .24f));
    options.fluidSimulationHz = std::clamp(experience.simulationHz, 60.f, 180.f);
    experience.particleCapacity = options.fluidCapacity;
    whitewater.reset();
    buoyancy.reset();
    fluidComplexity.reset();
    fluidSurface.reset();
    fluid.reset();
    if (optical) {
        optical->world = nullptr;
        optical->feedbackReady = false;
    }
    createFluid();
    reset = true;
}
XMFLOAT3 Renderer::displayRay(float x, float y) const {
    const float aspect = float(options.width) / options.height;
    displayedLens.rectilinear(x, y, aspect);
    float sy = y * displayedLens.tanHalfVertical(aspect),
          sx = x * displayedLens.tanHalfVertical(aspect) * aspect;
    XMFLOAT3 d;
    XMStoreFloat3(&d, XMVector3Normalize(
                          XMVectorAdd(XMLoadFloat3(&previousCamera.forward),
                                      XMVectorAdd(XMVectorScale(XMLoadFloat3(&previousCamera.right), sx),
                                                  XMVectorScale(XMLoadFloat3(&previousCamera.up), sy)))));
    return d;
}
void Renderer::loadScene() {
    meshes = options.fixture ? makeScene()
                             : makePlayScene(options.water, options.flatWater,
                                             options.fluid && !options.fluidSolverOnly && !options.fluidRoom,
                                             options.fluidRoom, options.boat, options.fluidDeepPool, options.largeWaterLab, options.oceanLab);
    sceneObjects.resize(meshes.size() + (options.fluid && !options.fluidSolverOnly ? 1 : 0) +
                        (options.fluidRoom && options.whitewater ? 1 : 0));
    if (sceneObjects.size() > meshes.size()) {
        auto &water = sceneObjects[meshes.size()];
        XMStoreFloat4x4(&water.world, XMMatrixIdentity());
        water.previous = water.world;
        water.info = {0, 0, 8, 0};
        if (options.fluidRoom && options.whitewater) {
            sceneObjects.back() = water;
            sceneObjects.back().info.z = 9;
        }
    }
    blas.resize(meshes.size());
    blasScratch.resize(meshes.size());
    size_t total = 0;
    for (auto &m : meshes)
        total += m.vertices.size();
    vertices = buffer(total * sizeof(Vertex), D3D12_HEAP_TYPE_UPLOAD);
    objects = buffer(sceneObjects.size() * sizeof(Object), D3D12_HEAP_TYPE_UPLOAD);
    instances = buffer(sceneObjects.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD);
    uint32_t offset = 0;
    begin();
    for (size_t i = 0; i < meshes.size(); ++i) {
        const auto &m = meshes[i];
        memcpy(static_cast<Vertex *>(vertices.mapped) + offset, m.vertices.data(),
               m.vertices.size() * sizeof(Vertex));
        XMStoreFloat4x4(&sceneObjects[i].world, XMMatrixIdentity());
        sceneObjects[i].previous = sceneObjects[i].world;
        sceneObjects[i].info = {offset, uint32_t(m.vertices.size()), 0, 0};
        D3D12_RAYTRACING_GEOMETRY_DESC geo{};
        geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geo.Triangles.VertexBuffer = {vertices.resource->GetGPUVirtualAddress() + offset * sizeof(Vertex),
                                      sizeof(Vertex)};
        geo.Triangles.VertexCount = uint32_t(m.vertices.size());
        geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        input.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.NumDescs = 1;
        input.pGeometryDescs = &geo;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input, &info);
        blas[i] = buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        blasScratch[i] =
            buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc{};
        desc.Inputs = input;
        desc.DestAccelerationStructureData = blas[i].resource->GetGPUVirtualAddress();
        desc.ScratchAccelerationStructureData = blasScratch[i].resource->GetGPUVirtualAddress();
        commands->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
        uav(commands.Get(), blas[i].resource.Get());
        offset += uint32_t(m.vertices.size());
    }
    submit(false);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.NumDescs = UINT(sceneObjects.size());
    input.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&input, &info);
    tlas = buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    tlasScratch = buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::array<XMFLOAT4, 802> table{};
    std::array<bool, 401> loaded{};
    std::ifstream cie(folder / "assets/CIE_xyz_1931_2deg.csv");
    std::string line;
    while (std::getline(cie, line)) {
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream s(line);
        int nm;
        float x, y, z;
        if (s >> nm >> x >> y >> z && nm >= 380 && nm <= 780) {
            table[nm - 380] = {x, y, z, 0};
            loaded[nm - 380] = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        }
    }
    if (!std::all_of(loaded.begin(), loaded.end(), [](bool b) { return b; }))
        throw std::runtime_error("Invalid CIE 1931 LUT");
    for (int i = 0; i < 401; ++i)
        table[401 + i] = {float((waterIndex(i + 380) + (options.oceanLab ? .006f : 0.f)) / 1.00027), waterAbsorption(float(i + 380)), 0, 0};
    cieData = buffer(sizeof(table), D3D12_HEAP_TYPE_UPLOAD);
    memcpy(cieData.mapped, table.data(), sizeof(table));
    if (options.oceanLab) {
        auto environment = ocean::loadEnvironment(folder / "assets/ocean");
        oceanSun = environment[1];
        const auto size = environment.size() * sizeof(XMFLOAT4);
        auto staging = buffer(size, D3D12_HEAP_TYPE_UPLOAD);
        memcpy(staging.mapped, environment.data(), size);
        oceanEnvironment = buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        begin();
        commands->CopyBufferRegion(oceanEnvironment.resource.Get(),0,staging.resource.Get(),0,size);
        transition(commands.Get(),oceanEnvironment.resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        submit(false);
        wait();
    }
}
void Renderer::buildTlas() {
    auto out = static_cast<D3D12_RAYTRACING_INSTANCE_DESC *>(instances.mapped);
    for (size_t i = 0; i < meshes.size(); ++i) {
        out[i] = {};
        XMFLOAT4X4 transposed;
        XMStoreFloat4x4(&transposed, XMMatrixTranspose(XMLoadFloat4x4(&sceneObjects[i].world)));
        memcpy(out[i].Transform, &transposed, sizeof(out[i].Transform));
        out[i].InstanceID = UINT(i);
        out[i].InstanceMask = meshes[i].mask;
        out[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        out[i].AccelerationStructure = blas[i].resource->GetGPUVirtualAddress();
    }
    if (fluidSurface) {
        auto &instance = out[meshes.size()];
        instance = {};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
        instance.InstanceID = UINT(meshes.size());
        instance.InstanceMask = 8;
        instance.InstanceContributionToHitGroupIndex = 1;
        instance.AccelerationStructure = fluidSurface->accelerationStructure();
    }
    if (whitewater) {
        auto &instance = out[meshes.size() + 1];
        instance = {};
        instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1;
        instance.InstanceID = UINT(meshes.size() + 1);
        instance.InstanceMask = 16;
        instance.InstanceContributionToHitGroupIndex = 2;
        instance.AccelerationStructure = whitewater->accelerationStructure();
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.NumDescs = UINT(sceneObjects.size());
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    d.Inputs.InstanceDescs = instances.resource->GetGPUVirtualAddress();
    d.DestAccelerationStructureData = tlas.resource->GetGPUVirtualAddress();
    d.ScratchAccelerationStructureData = tlasScratch.resource->GetGPUVirtualAddress();
    commands->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    uav(commands.Get(), tlas.resource.Get());
}
void Renderer::targets() {
    auto resolution = dlss.configure(options.width, options.height, options.quality);
    renderWidth = resolution.first;
    renderHeight = resolution.second;
    const uint64_t ptPitch = uint64_t((renderWidth + 15) / 16) * ((renderHeight + 15) / 16) * 256;
    const uint64_t reservoirBytes = options.restirPt ? ptPitch * 4 * 64 : 64;
    const uint64_t surfaceBytes = options.restirPt ? uint64_t(renderWidth) * renderHeight * 2 * 64 : 64;
    ptReservoirs = buffer(reservoirBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ptSurfaces = buffer(surfaceBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ptReservoirs.resource->SetName(L"RTXDI PT / initial, temporal and ping-pong path reservoirs");
    ptSurfaces.resource->SetName(L"RTXDI PT / current and previous diffuse surfaces");
    ptNeighborOffsets = buffer(256 * sizeof(XMFLOAT2), D3D12_HEAP_TYPE_UPLOAD);
    auto offsets = static_cast<XMFLOAT2 *>(ptNeighborOffsets.mapped);
    for (uint32_t i = 0; i < 256; ++i) {
        const float radius = std::sqrt((i + .5f) / 256), angle = i * 2.39996323f;
        offsets[i] = {radius * std::cos(angle), radius * std::sin(angle)};
    }
    ptAllocatedBytes = reservoirBytes + surfaceBytes + 256 * sizeof(XMFLOAT2);
    if (options.opticalImportance)
        optical = std::make_unique<OpticalImportance>(device.Get(), root.Get(), folder, renderWidth,
                                                      renderHeight, options.opticalValidate);
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                   DXGI_FORMAT_R32_FLOAT,          DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                   DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16G16B16A16_FLOAT};
    for (int i = 0; i < 8; ++i) {
        guides[i] =
            texture(i == 7 ? options.width : renderWidth, i == 7 ? options.height : renderHeight, formats[i]);
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format = formats[i];
        device->CreateUnorderedAccessView(guides[i].Get(), nullptr, &d, cpu(i == 7 ? 13 : i));
    }
    caustics = texture(atlasWidth, chartSize, DXGI_FORMAT_R32G32B32A32_FLOAT);
    fluidCaustics = texture(atlasWidth, chartSize, DXGI_FORMAT_R32G32B32A32_FLOAT);
    fluidCaustics->SetName(L"Animated liquid caustic irradiance / independent history");
    surfaceMap = texture(renderWidth, renderHeight, DXGI_FORMAT_R32G32B32A32_FLOAT);
    for (auto entry : {std::pair{caustics.Get(), 8u}, std::pair{surfaceMap.Get(), 9u},
                       std::pair{fluidCaustics.Get(), 17u}}) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(entry.first, nullptr, &d, cpu(entry.second));
    }
    for (auto entry : {std::pair{photonSum.resource.Get(), 7u}, std::pair{stats.resource.Get(), 10u},
                       std::pair{fluidPhotonSum.resource.Get(), 16u}}) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.Buffer.NumElements = UINT(entry.first->GetDesc().Width / 4);
        d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(entry.first, nullptr, &d, cpu(entry.second));
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
    D3D12_UNORDERED_ACCESS_VIEW_DESC beamUav{};
    beamUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    beamUav.Buffer.NumElements = 32;
    beamUav.Buffer.StructureByteStride = 64;
    device->CreateUnorderedAccessView(beams.resource.Get(), nullptr, &beamUav, cpu(11));
    nullUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    nullUav.Format = DXGI_FORMAT_R32_TYPELESS;
    nullUav.Buffer.NumElements = 1;
    nullUav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, cpu(31));
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = formats[7];
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(guides[7].Get(), &srv, cpu(14));
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    device->CreateShaderResourceView(guides[2].Get(), &srv, cpu(15));
    fgHudless.Reset();
    fgUi.Reset();
    fgDepth.Reset();
    fgDistortion.Reset();
    fgDistortionFov = -1;
    if (dlss.fgLoaded) {
        fgHudless = texture(options.width, options.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        fgUi = texture(options.width, options.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        fgDepth = texture(renderWidth, renderHeight, DXGI_FORMAT_R32_FLOAT);
        fgHudless->SetName(L"DLSS-FG tonemapped HUDless scene");
        fgUi->SetName(L"DLSS-FG premultiplied RmlUi layer");
        fgDepth->SetName(L"DLSS-FG device depth");
        for (uint32_t i = 0; i < 2; ++i) {
            auto resource = i ? fgUi.Get() : fgHudless.Get();
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            device->CreateShaderResourceView(resource, &srv, cpu(18 + i));
            auto r = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            r.ptr += SIZE_T(2 + i) * rtvSize;
            device->CreateRenderTargetView(resource, nullptr, r);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        u.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateUnorderedAccessView(fgDepth.Get(), nullptr, &u, cpu(20));
        // Match output pixel centres: half-resolution fields with clamped edge
        // sampling can miss by 5 pixels at 160 degrees. The map is cached, so
        // full resolution adds memory rather than recurring lens-evaluation work.
        fgDistortion = texture(options.width, options.height,
                               DXGI_FORMAT_R16G16B16A16_FLOAT);
        fgDistortion->SetName(L"DLSS-FG equisolid bidirectional UV displacement");
        u.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(fgDistortion.Get(), nullptr, &u, cpu(21));
    }
    for (UINT i = 0; i < 2; ++i) {
        check(swapchain->GetBuffer(i, IID_PPV_ARGS(&backbuffers[i])), "Backbuffer");
        auto h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtvSize;
        device->CreateRenderTargetView(backbuffers[i].Get(), nullptr, h);
    }
    reset = true;
}
void Renderer::setDlssQuality(int quality) {
    if (quality < 0 || quality > 2)
        throw std::runtime_error("Invalid DLSS quality mode");
    if (quality == options.quality)
        return;
    // The old RR/FG inputs can still be in use on the GPU. Reconfigure only at
    // the frame boundary, after both rendering and interpolation have drained.
    dlss.suspend();
    wait();
    dlss.free();
    options.quality = quality;
    targets(); // Rebuild resolution-dependent guides/reservoirs and reset history.
}
void Renderer::resize(uint32_t w, uint32_t h) {
    if (!w || !h || (w == options.width && h == options.height))
        return;
    dlss.suspend();
    wait();
    dlss.free();
    guides = {};
    backbuffers = {};
    caustics.Reset();
    fluidCaustics.Reset();
    surfaceMap.Reset();
    check(swapchain->ResizeBuffers(2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, swapchainFlags), "Resize lab");
    options.width = w;
    options.height = h;
    targets();
}
void Renderer::bind() {
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetComputeRootSignature(root.Get());
    commands->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(1, tlas.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(2, vertices.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(3, objects.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(4, cieData.resource->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(5, gpu(0));
    commands->SetComputeRootDescriptorTable(6, gpu(31));
    // Valid dummy SRVs keep the non-fluid pipeline binding complete. FluidState
    // guards all accesses when no procedural instance is present.
    commands->SetComputeRootShaderResourceView(7, fluidSurface
                                                      ? fluidSurface->fieldResource()->GetGPUVirtualAddress()
                                                      : cieData.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(8, fluidSurface
                                                      ? fluidSurface->mapResource()->GetGPUVirtualAddress()
                                                      : cieData.resource->GetGPUVirtualAddress());
    commands->SetComputeRootDescriptorTable(9, gpu(16));
    commands->SetComputeRootShaderResourceView(
        10, whitewater ? whitewater->particleResource()->GetGPUVirtualAddress()
                       : cieData.resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(11, ptReservoirs.resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(12, ptSurfaces.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(13, ptNeighborOffsets.resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(
        14, (optical ? optical->pixels : ptSurfaces).resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(
        15, (optical ? optical->counters : stats).resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(
        16, (optical ? optical->receivers : ptSurfaces).resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(
        17,
        (optical && optical->feedback.resource ? optical->feedback : stats).resource->GetGPUVirtualAddress());
    commands->SetComputeRootUnorderedAccessView(
        18, (optical && optical->world ? optical->world : stats.resource.Get())->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(19, whitewater
                                                       ? whitewater->foamResource()->GetGPUVirtualAddress()
                                                       : cieData.resource->GetGPUVirtualAddress());
    commands->SetComputeRootShaderResourceView(20, oceanEnvironment.resource
        ? oceanEnvironment.resource->GetGPUVirtualAddress() : cieData.resource->GetGPUVirtualAddress());
}
void Renderer::dispatch(uint32_t raygen, uint32_t w, uint32_t h) {
    commands->SetPipelineState1(transport.state.Get());
    auto base = transport.table.resource->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC d{};
    d.RayGenerationShaderRecord = {base + raygen * 64, 32};
    d.MissShaderTable = {base + 192, 32, 32};
    d.HitGroupTable = {base + 256, 192, 64};
    d.Width = w;
    d.Height = h;
    d.Depth = 1;
    commands->DispatchRays(&d);
}
void Renderer::render(float angle, float azimuth, float elevation, Game *game, Hud *hud, float delta) {
    auto start = std::chrono::steady_clock::now();
    std::optional<gpu::SubmissionTimeline> cpuTimeline;
    if (latency)
        cpuTimeline.emplace(start);
    auto *timeline = cpuTimeline ? &*cpuTimeline : nullptr;
    if (optical && hud) {
        const char *views[]{"shaded",   "importance",    "variance",  "temporal change",
                            "caustics", "sample budget", "confidence"};
        hud->rendererStatus = std::string(options.adaptiveRays ? "Adaptive rays" : "Optical observer") +
                              " · " + views[options.opticalView] +
                              (options.opticalFreeze ? " · budgets frozen" : "") + " · F4 view / F5 freeze";
    }
    dlss.marker(sl::PCLMarker::eRenderSubmitStart);
    // Present two genuine frames before interpolation to establish history and
    // keep first-frame RR creation out of the interpolation pacing interval.
    Lens lens = experience.lens;
    if (options.fixture || options.opticalView || (fluid && fluid->debugVisible) ||
        (fluidComplexity && fluidComplexity->debugMode))
        lens.fisheye = false;
    if (lens.fisheye != displayedLens.fisheye || lens.diagonalDegrees != displayedLens.diagonalDegrees ||
        (game && game->firstPerson != previousFirstPerson))
        reset = true;
    displayedLens = lens;
    previousFirstPerson = game && game->firstPerson;
    dlss.prepareFrame(frame >= 2 && !IsIconic(window) &&
                      (!game || (!game->paused && !game->won)) && (!fluid || !fluid->debugVisible) &&
                      (!fluidComplexity || !fluidComplexity->debugMode) &&
                      (!fluid || !fluid->cutCells || !fluid->cutCells->debugVisible) && !options.opticalView);
    if (reset || (game && game->resetHistory))
        cameraFollow.reset();
    Camera cam = game ? game->camera(float(options.width) / options.height, &cameraFollow, delta)
                      : camera(float(options.width) / options.height, azimuth, elevation);
    if (options.fluidView)
        cam = camera(float(options.width) / options.height,
                     (options.fluidRoom ? 1.55f : -2.45f) +
                         (options.orbitTest ? frame * .003f : (game ? game->azimuth - .42f : 0)),
                     game ? std::clamp(game->elevation, .1f, 1.4f) : .56f, true, options.fluidRoom,
                     options.fluidDeepPool, options.largeWaterLab, options.oceanLab);
    Constants c{};
    const float aspect = float(options.width) / options.height;
    float tanHalf = lens.tanHalfVertical(aspect);
    auto projection = XMMatrixPerspectiveFovRH(2 * std::atan(tanHalf), aspect, .05f, options.oceanLab ? 10000.f : 200.f);
    XMStoreFloat4x4(&cam.projection, projection);
    XMStoreFloat4x4(&cam.viewProjection, XMLoadFloat4x4(&cam.view) * projection);
    c.camera = {cam.position.x, cam.position.y, cam.position.z, 1};
    c.right = {cam.right.x, cam.right.y, cam.right.z, tanHalf * options.width / options.height};
    c.up = {cam.up.x, cam.up.y, cam.up.z, tanHalf};
    c.forward = {cam.forward.x, cam.forward.y, cam.forward.z, 0};
    c.viewProjection = cam.viewProjection;
    c.previousViewProjection = frame ? previousCamera.viewProjection : cam.viewProjection;
    if (game) {
        const auto poses = game->poses();
        if (poses.size() + (options.water ? 1 : 0) + (fluidSurface && !options.fluidRoom ? 1 : 0) !=
            meshes.size())
            throw std::runtime_error("Physics/DXR body mismatch");
        for (size_t i = 0; i < poses.size(); ++i)
            sceneObjects[i].world = poses[i];
        // The untextured spherical avatar has no optical orientation. Keep its
        // tessellation fixed while Bullet spins the rigid body, avoiding rotating
        // facet silhouettes, spurious motion vectors and atlas invalidations.
        auto &ball = sceneObjects[1].world;
        XMStoreFloat4x4(&ball, XMMatrixTranslation(ball._41, ball._42, ball._43));
        angle = std::atan2(sceneObjects[2].world._31, sceneObjects[2].world._11);
        c.play = {1, game->charge, game->elapsed, fluid && fluid->emitter.enabled ? 1.f : 0.f};
        c.sensor = {game->level().receiver.x, game->level().receiver.y, game->level().receiver.z,
                    game->level().receiverHalf.z};
        reset = reset || game->resetHistory;
        game->resetHistory = false;
    } else
        sceneObjects[1].world = prismTransform(angle);
    lastPrismAngle = angle;
    c.dimensions = {renderWidth, renderHeight, frame, options.photons};
    c.controls = {0, floatAtomics ? 1u : 0u, reorder ? 1u : 0u, options.history};
    c.jitter = {halton(frame % 1024 + 1, 2) - .5f, halton(frame % 1024 + 1, 3) - .5f,
                halton((frame ? frame - 1 : 0) % 1024 + 1, 2) - .5f,
                halton((frame ? frame - 1 : 0) % 1024 + 1, 3) - .5f};
    c.lightOrigin = {0, 2 + (options.largeWaterLab ? largeWater::lift : 0.f), -5, 0};
    if (options.fluidDeepPool)
        c.lightOrigin = {0, 10, -20, 0};
    XMVECTOR ld = XMVector3Normalize(XMVectorSet(0, -.07f, 1, 0));
    XMFLOAT3 ldf;
    XMStoreFloat3(&ldf, ld);
    c.lightDirection = {ldf.x, ldf.y, ldf.z, 0};
    c.lightRight = {1, 0, 0, .9f};
    c.lightUp = {0, ldf.z, -ldf.y, .8f};
    c.optics = {1.5046f, .00420f, .035f, 40};
    c.water = {game && (options.water || fluidSurface) ? 1.f : 0.f, options.flatWater ? 1.f : 0.f,
               game && options.lasers ? 1.f : 0.f, laserWavelength};
    c.medium = {game && options.haze ? .012f : 0.f, .005f, 1.5f, 28.f};
    if (options.fluidRoom)
        c.medium.w = 140.f; // Broad, real overhead fill: ~0.86 radiant W/m^2, not an emissive floor.
    if (options.largeWaterLab)
        c.medium.w = 560.f; // Four times the aperture area, same irradiance as Water Lab.
    if (options.fluidDeepPool)
        c.medium.w = 2240.f; // 16x aperture area, same irradiance; still below energy-ledger overflow.
    c.lighting = {1, 1, 1, float(experience.environment)}; // local lights, emissives, sky, preset
    if (game && experience.environment) {
        c.lighting = {0, 0, 0, float(experience.environment)};
        c.optics.w = 0;
        c.water.z = 0;
        if (experience.environment == 2) {
            c.medium.w *= 2;
            c.lighting.x = 1;
        }
        if (experience.environment == 3)
            c.medium.w = 0;
    }
    c.cameraState = {game && game->firstPerson ? 1u : 0u, 0, 0, 0};
    c.cameraState.w = options.fluidDeepPool ? 1u : (options.largeWaterLab ? 2u : 0u);
    if (options.oceanLab) {
        c.cameraState.w = 3;
        c.lighting = {0, 0, 1, 4.f + float(experience.environment % 2)};
        c.water.z = c.optics.w = 0;
        c.medium.x = options.haze ? .00002f : 0;
        c.medium.w = experience.environment ? 0 : oceanSun.w * 256.f * 256.f;
    }
    if (game && experience.flashlight) {
        const auto ball = game->playerPosition();
        c.flashlightOrigin = {ball.x + cam.forward.x * .74f, ball.y + cam.forward.y * .74f,
                              ball.z + cam.forward.z * .74f, 8.f}; // radiant W, uniform 24 degree half-cone
        c.flashlightDirection = {cam.forward.x, cam.forward.y, cam.forward.z, std::cos(XM_PI * 24 / 180)};
    }
    // All XYZ component sums are bounded below 8*flux for this CIE LUT; leaves
    // >2x overflow margin even if every photon lands in the same atlas texel.
    const float totalFlux =
        std::max(1.f, c.optics.w + c.water.x * c.medium.w + c.water.z * c.medium.z * (1 + c.water.x) +
                          c.flashlightOrigin.w);
    c.atlas = {float(atlasWidth), float(chartSize), float(chartSize), 268435456.f / totalFlux};
    uint64_t hash = 14695981039346656037ull;
    for (auto &object : sceneObjects)
        hash = hashBytes(&object.world, sizeof(XMFLOAT4X4), hash);
    hash = hashBytes(&c.lightOrigin, 5 * sizeof(XMFLOAT4), hash); // light + optics; never camera/frame/jitter
    hash = hashBytes(&c.water, 2 * sizeof(XMFLOAT4), hash);
    const auto sourceHash =
        hashBytes(&c.water, 2 * sizeof(XMFLOAT4), hashBytes(&c.lightOrigin, 5 * sizeof(XMFLOAT4)));
    const auto lightHash = hashBytes(&c.lighting, 3 * sizeof(XMFLOAT4), sourceHash);
    // A moving flashlight changes transport, but not the emitted energy/spectrum.
    // Keep the short animated-water EMA while it moves; toggles, cone/power,
    // environment and optical material changes still discard obsolete lighting.
    const auto fluidLightHash = hashBytes(&c.flashlightDirection.w, sizeof(float),
                                          hashBytes(&c.flashlightOrigin.w, sizeof(float),
                                                    hashBytes(&c.lighting, sizeof(XMFLOAT4), sourceHash)));
    bool dirty = reset || lightHash != lastLightHash;
    const bool fluidHistoryReset = reset || fluidLightHash != lastFluidLightHash;
    for (size_t i = 0; i < sceneObjects.size(); ++i) {
        const bool moved = transportMoved(sceneObjects[i].world, transportAnchors[i], transportRadii[i]);
        dirty = dirty || moved;
        // Moving balls, boats, cubes and optics all use the same short water
        // history. Resetting it for every buoyant-body transform reduced it to
        // one noisy photon frame indefinitely, even when the camera was still.
        // Controls.x still shortens the EMA and invalidates static caustics/PT.
    }
    c.controls.x = dirty ? 1 : 0;
    if (dirty) {
        ++historyResets;
        for (size_t i = 0; i < sceneObjects.size(); ++i)
            transportAnchors[i] = sceneObjects[i].world;
    }
    // Geometry/lighting motion is not a camera cut. Its dense motion guides and
    // new noisy radiance must reach RR WITHOUT discarding its entire history.
    // The photon atlas still invalidates immediately on transport changes.
    if (reset)
        ++rrHistoryResets;
    if (!frame)
        for (auto &object : sceneObjects)
            object.previous = object.world;
    memcpy(objects.mapped, sceneObjects.data(), sceneObjects.size() * sizeof(Object));
    begin();
    if (fluid) {
        if (fluidSurface && game) {
            std::vector<FluidCollider> colliders;
            uint32_t discontinuities = 0;
            for (uint32_t i = 1; i < game->level().props.size() + 2; ++i) {
                if (i == 5 && !options.fluidRoom)
                    continue;
                const size_t firstCollider = colliders.size();
                FluidCollider collider{};
                const auto &now = sceneObjects[i].world, &previous = sceneObjects[i].previous;
                XMStoreFloat4x4(&collider.worldToLocal, XMMatrixInverse(nullptr, XMLoadFloat4x4(&now)));
                collider.centerRestitution = {now._41, now._42, now._43, 0};
                collider.extentType = i == 1 ? XMFLOAT4{.68f, .68f, .68f, 0} : XMFLOAT4{.38f, .38f, .38f, 1};
                if (i == 5)
                    collider.extentType = {1.48f, 1.4f, .16f, 1};
                if (i == 2) {
                    collider.extentType.w = 5;
                    collider.meshMinimumSpacing = fluidPrismSdf.minimumSpacing;
                    collider.meshDimensions = fluidPrismSdf.dimensions;
                }
                const float invDt = frame && !game->paused && delta > 0 ? 1 / delta : 0;
                collider.velocityFriction = {(now._41 - previous._41) * invDt,
                                             (now._42 - previous._42) * invDt,
                                             (now._43 - previous._43) * invDt, .08f};
                collider.angularSlip = {0, 0, 0, 1};
                if (invDt > 0) {
                    XMVECTOR qNow = XMQuaternionRotationMatrix(XMLoadFloat4x4(&now));
                    XMVECTOR qPrevious = XMQuaternionRotationMatrix(XMLoadFloat4x4(&previous));
                    XMVECTOR dq = XMQuaternionMultiply(XMQuaternionInverse(qPrevious), qNow);
                    if (XMVectorGetW(dq) < 0)
                        dq = XMVectorNegate(dq);
                    XMVECTOR axis;
                    float rotation;
                    XMQuaternionToAxisAngle(&axis, &rotation, dq);
                    if (rotation > 1e-6f) {
                        XMFLOAT3 omega;
                        XMStoreFloat3(&omega, XMVector3Normalize(axis) * (rotation * invDt));
                        collider.angularSlip = {omega.x, omega.y, omega.z, 1};
                    }
                }
                if (int(i) == game->boatBody + 1 && game->boatBody >= 0) {
                    collider.extentType = {1.3f,.45f,3.1f,5};
                    collider.meshMinimumSpacing = fluidBoatSdf.minimumSpacing;
                    collider.meshDimensions = fluidBoatSdf.dimensions;
                    collider.meshDimensions.w = uint32_t(fluidPrismSdf.phi.size());
                }
                colliders.push_back(collider);
                if (i - 1 < game->poseDiscontinuities.size() && game->poseDiscontinuities[i - 1]) {
                    for (size_t slot = firstCollider; slot < colliders.size(); ++slot) {
                        if (slot >= FluidColliderTimeline::capacity)
                            throw std::runtime_error("Teleported collider exceeds fluid capacity");
                        discontinuities |= 1u << slot;
                    }
                }
            }
            // Match the closed optical shell. Reconstruction also consumes these
            // SDFs, so its kernel support cannot protrude through the glass walls.
            for (uint32_t wall = 0; !options.fluidRoom && wall < 4; ++wall) {
                const XMFLOAT3 p = wall < 2 ? XMFLOAT3{wall ? 5.7f : 1.7f, .6f, -3.1f}
                                            : XMFLOAT3{3.7f, .6f, wall == 2 ? -5.f : -1.2f};
                FluidCollider collider{};
                XMStoreFloat4x4(&collider.worldToLocal, XMMatrixTranslation(-p.x, -p.y, -p.z));
                collider.centerRestitution = {p.x, p.y, p.z, 0};
                collider.extentType = wall < 2 ? XMFLOAT4{.1f, .6f, 2.f, 1} : XMFLOAT4{1.9f, .6f, .1f, 1};
                collider.angularSlip.w = 1;
                colliders.push_back(collider);
            }
            if (options.fluidRoom && !options.oceanLab) {
                const float pedestal = options.fluidDeepPool ? 4.29f : .29f + (options.largeWaterLab ? largeWater::lift * .5f : 0.f);
                const float column = options.fluidDeepPool ? 5.2f : 1.2f + (options.largeWaterLab ? largeWater::lift * .5f : 0.f);
                const float sourceZ = options.fluidDeepPool ? -20.25f : -5.25f;
                for (auto [p, e] : {std::pair{XMFLOAT3{0, pedestal, 0}, XMFLOAT3{1.05f, pedestal, 1.05f}},
                                    std::pair{XMFLOAT3{0, column, sourceZ}, XMFLOAT3{.30f, column, .25f}}}) {
                    FluidCollider collider{};
                    XMStoreFloat4x4(&collider.worldToLocal, XMMatrixTranslation(-p.x, -p.y, -p.z));
                    collider.centerRestitution = {p.x, p.y, p.z, 0};
                    collider.extentType = {e.x, e.y, e.z, 1};
                    collider.angularSlip.w = 1;
                    colliders.push_back(collider);
                }
            }
            if (options.oceanLab) {
                FluidCollider terrain{};
                XMStoreFloat4x4(&terrain.worldToLocal, XMMatrixIdentity());
                terrain.extentType = {128, 11, 128, 6};
                terrain.angularSlip.w = 1;
                colliders.push_back(terrain);
                for (const auto &part : ocean::pier) {
                    FluidCollider solid{};
                    XMStoreFloat4x4(&solid.worldToLocal, XMMatrixTranslation(-part.center.x,-part.center.y,-part.center.z));
                    solid.centerRestitution = {part.center.x,part.center.y,part.center.z,0};
                    solid.extentType = {part.half.x,part.half.y,part.half.z,1};
                    solid.angularSlip.w = 1;
                    colliders.push_back(solid);
                }
            }
            fluid->setColliders(colliders, discontinuities);
            game->poseDiscontinuities.clear();
        }
        fluid->validatePressureThisFrame =
            (options.fluidValidate || options.fluidPressureValidate) && frame + 1 == options.frames;
        fluid->validateMacThisFrame = options.fluidMacValidate;
        fluid->validateWorkThisFrame =
            options.fluidWorkValidate || (options.fluidValidate && frame + 1 == options.frames);
        fluid->validateBulkThisFrame =
            (options.fluidValidate || options.fluidBulkValidate) && frame + 1 == options.frames;
        fluid->validateCutCellsThisFrame = options.fluidCutValidate;
        fluid->validateResamplingThisFrame =
            options.fluidResampleValidate || (options.fluidValidate && frame + 1 == options.frames);
        fluid->validateInteriorThisFrame =
            options.fluidInteriorValidate || (options.fluidValidate && frame + 1 == options.frames);
        gpu::stamp(timeline, gpu::SubmissionStage::FluidBegin);
        if (game && game->paused) {
            const bool paused = fluid->paused;
            fluid->paused = true;
            fluid->record(commands.Get(), delta, cam, queue.Get(), allocator.Get(), timeline);
            fluid->paused = paused;
        } else
            fluid->record(commands.Get(), delta, cam, queue.Get(), allocator.Get(), timeline);
        gpu::stamp(timeline, gpu::SubmissionStage::FluidRecorded);
        if (fluidSurface) {
            // Explicit liquid reset is a topology teleport, unlike normal fluid
            // evolution. It is a legitimate reconstruction discontinuity.
            if (fluid->resetThisFrame && !reset) {
                reset = true;
                ++rrHistoryResets;
            }
            fluidSurface->validateLodThisFrame = options.fluidSurfaceLodValidate;
            fluidSurface->record(commands.Get(), *fluid, cam, delta);
            if (fluidComplexity) {
                if (optical) {
                    const auto view = fluidComplexity->gpuView();
                    optical->setWorldBricks(device.Get(), view.state, view.grid, view.minimumSpacing);
                    fluidComplexity->setOpticalFeedback(optical->feedback.resource.Get(),
                                                        optical->feedbackReady);
                }
                fluidComplexity->record(commands.Get(), *fluid, *fluidSurface, cam, delta);
                fluidSurface->setImportance(fluidComplexity->gpuView());
                if (fluid->resampling)
                    fluid->resampling->setImportance(fluidComplexity->gpuView());
                if (fluid->interior)
                    fluid->interior->setImportance(fluidComplexity->gpuView());
                if (fluid->mac)
                    fluid->mac->setImportance(fluidComplexity->gpuView());
            }
            if (whitewater)
                whitewater->record(commands.Get(), *fluid, *fluidSurface);
            if (buoyancy && game)
                buoyancy->record(commands.Get(), *fluid, *fluidSurface,
                    game->waterQueries(2.5f * fluid->description().gridCellSize));
            c.fluidMinimumSpacing = fluidSurface->minimumSpacing;
            c.fluidBricks = fluidSurface->brickGrid;
            c.fluidState = {
                1,
                fluid->changedThisFrame && !fluid->resetThisFrame && !options.fluidSurfaceFixture ? 1u : 0u,
                fluid->resetThisFrame ? 1u : 0u,
                options.fluidValidate ? (1u | options.fluidSurfaceFixture << 1) : 0u};
            if (fluidSurface->adaptive && fluidSurface->changedThisFrame)
                c.fluidState.y |= 2u; // Consume current GPU-measured LOD deformation, including fixtures.
            c.fluidState.w |= fluidSurface->debugMode << 8;
            c.fluidState.w |= options.fluidRoom ? 16u : 0u;
            c.fluidState.w |= whitewater ? 32u : 0u;
            c.fluidState.w |= options.fluidFullBrickTraversal ? 64u : 0u;
            c.fluidState.w |= fluidHistoryReset ? 128u : 0u;
            if (fluidHistoryReset || fluid->resetThisFrame)
                ++fluidHistoryResets;
        }
    }
    gpu::stamp(timeline, gpu::SubmissionStage::SurfaceRecorded);
    // Cached suffix lighting is not valid across moving occluders or changing
    // fluid/caustics. Reuse within this frame remains safe; RR keeps its history.
    const uint64_t ptTransportHash =
        hashBytes(&c.play.w, sizeof(float), hashBytes(&c.play.y, sizeof(float), hash));
    const bool ptStatic = frame && !reset && !dirty && ptTransportHash == lastPtTransportHash &&
                          !(fluidSurface && fluidSurface->changedThisFrame) && !whitewater;
    ptStableFrames = ptStatic ? std::min(ptStableFrames + 1, options.history) : 0;
    const bool ptTemporal = options.restirPt && ptStableFrames >= options.history && options.ptHistory > 1;
    lastPtTransportHash = ptTransportHash;
    c.ptControls = {options.restirPt ? 1u : 0u, ptTemporal ? 1u : 0u, options.ptHistory, options.ptSpatial};
    c.ptPreviousCamera = {previousCamera.position.x, previousCamera.position.y, previousCamera.position.z, 0};
    c.opticalControls = {
        optical ? (1u | (options.adaptiveRays ? 2u : 0u) | (options.opticalFreeze ? 4u : 0u)) : 0u,
        frame && !reset && lightHash == lastLightHash ? 1u : 0u, options.opticalSamples,
        options.opticalView | (options.opticalUniform ? 256u : 0u) | (options.retracePrimary ? 512u : 0u) |
            (options.lambertianReference ? 1024u : 0u) | (options.waterVisibilityReference ? 2048u : 0u)};
    c.opticalParameters = {delta, .015f, .0008f, .25f};
    if (options.oceanSwimTest) c.opticalControls.w |= 4096u;
    if (optical && optical->world)
        c.opticalControls.x |= 8u;
    if (ptTemporal)
        ++ptTemporalFrames;
    memcpy(uniforms.mapped, &c, sizeof(c));
    if (!frame || hash != lastHash || (fluidSurface && fluidSurface->changedThisFrame) || whitewater)
        buildTlas();
    bind();
    commands->SetPipelineState(clear.Get());
    commands->Dispatch(atlasWidth / 8, chartSize / 8, 1);
    uav(commands.Get());
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    dispatch(2, 2, 1);
    uav(commands.Get(), beams.resource.Get());
    dispatch(0, options.photons, 1);
    if (fluidSurface && (options.fluidValidate || options.oceanSwimTest))
        dispatch(7, 4096, 1);
    uav(commands.Get(), photonSum.resource.Get());
    uav(commands.Get(), fluidPhotonSum.resource.Get());
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    commands->SetPipelineState(accumulate.Get());
    commands->Dispatch(atlasWidth / 8, chartSize / 8, 1);
    uav(commands.Get(), caustics.Get());
    uav(commands.Get(), fluidCaustics.Get());
    if (optical)
        uav(commands.Get(), optical->receivers.resource.Get());
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    gpu::stamp(timeline, gpu::SubmissionStage::PhotonsRecorded);
    dispatch(1, renderWidth, renderHeight);
    uav(commands.Get());
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 6);
    if (options.restirPt) {
        gpu::Event event(commands.Get(), L"RTXDI ReSTIR PT / temporal + spatial hybrid path reuse");
        dispatch(8, renderWidth, renderHeight);
        uav(commands.Get());
        commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
        dispatch(9, renderWidth, renderHeight);
        uav(commands.Get());
    } else
        commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 7);
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
    gpu::stamp(timeline, gpu::SubmissionStage::CameraRecorded);
    // This real compute pass also supplies the measured post-raygen DLSS handoff.
    commands->SetPipelineState(composite.Get());
    commands->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
    if (optical) {
        uav(commands.Get());
        optical->record(commands.Get(), frame);
    }
    for (int i = 0; i < 7; ++i)
        transition(commands.Get(), guides[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 4);
    gpu::stamp(timeline, gpu::SubmissionStage::CompositeRecorded);
    std::array<ID3D12Resource *, 8> tagged{};
    for (int i = 0; i < 8; ++i)
        tagged[i] = guides[i].Get();
    // pixel + jitter moves projected geometry by -jitter. SL expects that
    // projected image offset; the motion guides themselves are unjittered.
    gpu::stamp(timeline, gpu::SubmissionStage::RRBegin);
    dlss.evaluate(commands.Get(), tagged, cam, frame ? previousCamera : cam, {-c.jitter.x, -c.jitter.y},
                  reset, frame);
    gpu::stamp(timeline, gpu::SubmissionStage::RRRecorded);
    commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 5);
    XMFLOAT4 lensConstants{float(options.width), float(options.height), lens.fisheye ? 1.f : 0.f,
                           lens.diagonalDegrees * XM_PI / 360};
    if (fgDepth) {
        ID3D12DescriptorHeap *fgHeaps[] = {heap.Get()};
        commands->SetDescriptorHeaps(1, fgHeaps);
        commands->SetComputeRootSignature(presentRoot.Get());
        commands->SetPipelineState(fgPrepareDepth.Get());
        commands->SetComputeRootDescriptorTable(0, gpu(15));
        commands->SetComputeRootDescriptorTable(2, gpu(20));
        commands->Dispatch((renderWidth + 7) / 8, (renderHeight + 7) / 8, 1);
        uav(commands.Get(), fgDepth.Get());
    }
    if (dlss.fgEnabled && lens.fisheye && fgDistortionFov != lens.diagonalDegrees) {
        // Lens geometry is independent of moving water, reflection motion and
        // camera pose. Recompute only after a lens/FOV or target-size change.
        commands->SetComputeRootSignature(presentRoot.Get());
        commands->SetPipelineState(fgPrepareDistortion.Get());
        commands->SetComputeRootDescriptorTable(2, gpu(21));
        commands->SetComputeRoot32BitConstants(3, 4, &lensConstants, 0);
        commands->Dispatch((options.width + 7) / 8, (options.height + 7) / 8, 1);
        uav(commands.Get(), fgDistortion.Get());
        fgDistortionFov = lens.diagonalDegrees;
        ++fgDistortionUpdates;
    }
    for (int i = 0; i < 7; ++i)
        transition(commands.Get(), guides[i].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(commands.Get(), guides[7].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    lastBuffer = swapchain->GetCurrentBackBufferIndex();
    auto bb = backbuffers[lastBuffer].Get();
    transition(commands.Get(), bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(presentRoot.Get());
    commands->SetPipelineState(present.Get());
    commands->SetGraphicsRootDescriptorTable(0, gpu(14));
    commands->SetGraphicsRoot32BitConstants(3, 4, &lensConstants, 0);
    D3D12_VIEWPORT vp{0, 0, float(options.width), float(options.height), 0, 1};
    D3D12_RECT rect{0, 0, LONG(options.width), LONG(options.height)};
    commands->RSSetViewports(1, &vp);
    commands->RSSetScissorRects(1, &rect);
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += SIZE_T(lastBuffer) * rtvSize;
    auto sceneRtv = rtv;
    if (fgHudless) {
        transition(commands.Get(), fgHudless.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        sceneRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        sceneRtv.ptr += SIZE_T(2) * rtvSize;
    }
    commands->OMSetRenderTargets(1, &sceneRtv, FALSE, nullptr);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    if (fluid) {
        transition(commands.Get(), guides[2].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        fluid->drawDebug(commands.Get(), gpu(15));
        if (fluid->bulk)
            fluid->bulk->drawDebug(commands.Get());
        if (fluidComplexity)
            fluidComplexity->drawDebug(commands.Get());
        transition(commands.Get(), guides[2].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        fluid->recordTimings(commands.Get());
        if (fluid->hamiltonian)
            fluid->hamiltonian->recordReadback(commands.Get());
        if (fluidSurface)
            fluidSurface->recordReadback(commands.Get());
        if (fluidComplexity)
            fluidComplexity->recordReadback(commands.Get(),
                                            options.fluidComplexityValidate && frame + 1 == options.frames);
        if (whitewater)
            whitewater->recordReadback(commands.Get(), (options.fluidValidate || options.oceanSwimTest) && frame + 1 == options.frames);
        if (options.fluidValidate && frame + 1 == options.frames)
            fluid->recordValidationReadback(commands.Get());
    }
    if (fgUi) {
        gpu::Event fgEvent(commands.Get(), L"DLSS-FG / HUDless scene and premultiplied UI handoff");
        transition(commands.Get(), fgHudless.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(commands.Get(), fgUi.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto uiRtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        uiRtv.ptr += SIZE_T(3) * rtvSize;
        const float transparent[4] = {};
        commands->ClearRenderTargetView(uiRtv, transparent, 0, nullptr);
        commands->OMSetRenderTargets(1, &uiRtv, FALSE, nullptr);
        if (hud)
            hud->render(commands.Get(), int(options.width), int(options.height));
        transition(commands.Get(), fgUi.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commands->SetDescriptorHeaps(1, heaps);
        commands->SetGraphicsRootSignature(presentRoot.Get());
        commands->SetPipelineState(fgComposite.Get());
        commands->SetGraphicsRootDescriptorTable(0, gpu(18));
        commands->SetGraphicsRootDescriptorTable(1, gpu(19));
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->RSSetViewports(1, &vp);
        commands->RSSetScissorRects(1, &rect);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        dlss.tagFrameGeneration(commands.Get(), fgDepth.Get(), guides[1].Get(), fgHudless.Get(), fgUi.Get(),
                                lens.fisheye ? fgDistortion.Get() : nullptr);
    } else if (hud)
        hud->render(commands.Get(), int(options.width), int(options.height));
    transition(commands.Get(), bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    transition(commands.Get(), guides[7].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(commands.Get(), stats.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    commands->CopyBufferRegion(statsReadback.resource.Get(), 0, stats.resource.Get(), 0, sizeof(counters));
    transition(commands.Get(), stats.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commands->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 8, timingReadback.resource.Get(),
                               0);
    if (optical)
        optical->recordReadback(commands.Get());
    if (hud)
        uiGeometryUploads = hud->geometryUploadStats();
    gpu::stamp(timeline, gpu::SubmissionStage::UIRecorded);
    submit(true, timeline);
    if (buoyancy && game)
        game->receiveWater(buoyancy->collect(), fluid->description().density,
                           -fluid->description().gravity.y);
    if (fluid)
        fluid->collectTimings(frequency);
    if (fluid && fluid->hamiltonian)
        fluid->collectHamiltonian();
    if (fluidSurface)
        fluidSurface->collect(frequency);
    if (fluidComplexity)
        fluidComplexity->collect(frequency);
    if (whitewater)
        whitewater->collect(frequency);
    if (optical)
        optical->collect(frequency);
    void *mapped;
    D3D12_RANGE range{0, 64};
    check(timingReadback.resource->Map(0, &range, &mapped), "Read timestamps");
    auto times = static_cast<uint64_t *>(mapped);
    std::array<double, 11> sample{};
    for (int i = 0; i < 5; ++i)
        sample[i] = double(times[i + 1] - times[i]) * 1000 / frequency;
    if (options.frames && frame >= 32)
        ptTimings.push_back({options.restirPt ? double(times[7] - times[6]) * 1000 / frequency : 0,
                             options.restirPt ? double(times[3] - times[7]) * 1000 / frequency : 0});
    timingReadback.resource->Unmap(0, nullptr);
    range = {0, sizeof(counters)};
    check(statsReadback.resource->Map(0, &range, &mapped), "Read photon diagnostics");
    memcpy(counters.data(), mapped, sizeof(counters));
    statsReadback.resource->Unmap(0, nullptr);
    if (options.oceanSwimTest && counters[28])
        throw std::runtime_error("Artificial air pocket beside submerged player at frame " + std::to_string(frame));
    receiverWatts = counters[12] / 1048576.f;
    cameraGlassPixels += counters[8];
    cameraTransmissions += counters[9];
    cameraTir += counters[10];
    cameraReflections += counters[11];
    if (counters[5] || counters[6])
        throw std::runtime_error("Photon accumulation failed at frame " + std::to_string(frame) +
                                 ": overflow=" + std::to_string(counters[5]) +
                                 ", invalid=" + std::to_string(counters[6]) +
                                 ", footprint caps=" + std::to_string(counters[7]));
    if (counters[40])
        throw std::runtime_error("Invalid ReSTIR PT estimator at frame " + std::to_string(frame));
    photonMs = sample[0];
    accumulationMs = sample[1];
    cameraMs = sample[2];
    compositeMs = sample[3];
    rrMs = sample[4];
    frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    sample[5] = frameMs;
    sample[6] = fluid ? fluid->simulationMs : 0;
    sample[7] = fluidSurface ? fluidSurface->reconstructionMs : 0;
    sample[8] = fluidSurface ? fluidSurface->blasMs : 0;
    sample[9] = whitewater ? whitewater->simulationMs : 0;
    sample[10] = whitewater ? whitewater->blasMs : 0;
    if (options.frames && frame >= 32)
        samples.push_back(sample);
    gpu::stamp(timeline, gpu::SubmissionStage::Collected);
    if (latency)
        latency->rendered(frame, sample, fluid ? fluid->cudaTelemetry() : std::array<double, 5>{},
                          fluid ? double(fluid->advancedSeconds) : 0, fluid ? fluid->droppedSeconds : 0,
                          *timeline);
    for (auto &object : sceneObjects)
        object.previous = object.world;
    previousCamera = cam;
    reset = false;
    lastHash = hash;
    lastLightHash = lightHash;
    lastFluidLightHash = fluidLightHash;
    ++frame;
}
void Renderer::report(const std::filesystem::path &path) {
    const double energyLedgerScale = options.oceanLab ? 1024.0 : 1048576.0;
    std::ofstream out(path);
    if (!out)
        throw std::runtime_error("Cannot write lab report");
    std::array<double, 11> medians{};
    std::array<double, 2> ptMedians{};
    for (uint32_t i = 0; i < 2; ++i) {
        std::vector<double> values;
        for (auto row : ptTimings)
            values.push_back(row[i]);
        if (!values.empty()) {
            std::sort(values.begin(), values.end());
            ptMedians[i] = values[values.size() / 2];
        }
    }
    for (int column = 0; column < 11; ++column) {
        std::vector<double> values;
        for (const auto &row : samples)
            values.push_back(row[column]);
        if (!values.empty()) {
            std::sort(values.begin(), values.end());
            medians[column] = values[values.size() / 2];
        }
    }
    out << "{\n  \"opticalImportance\": ";
    if (optical)
        optical->report(out);
    else
        out << "null";
    out << ",\n  \"adaptiveRays\": " << (options.adaptiveRays ? "true" : "false")
        << ",\n  \"opticalUniform\": " << (options.opticalUniform ? "true" : "false")
        << ",\n  \"lambertianReduction\": " << (options.lambertianReference ? "false" : "true")
        << ",\n  \"waterVisibilityBracket\": " << (options.waterVisibilityReference ? "false" : "true")
        << ",\n  \"opticalMaxSamples\": " << options.opticalSamples
        << ",\n  \"primaryTraceReused\": " << (options.retracePrimary ? "false" : "true")
        << ",\n  \"experience\": {\"fisheye\":" << (displayedLens.fisheye ? "true" : "false")
        << ",\"diagonalFov\":" << displayedLens.diagonalDegrees
        << ",\"firstPerson\":" << (previousFirstPerson ? "true" : "false")
        << ",\"environment\":" << experience.environment
        << ",\"flashlight\":" << (experience.flashlight ? "true" : "false") << ",\"camera\":["
        << previousCamera.position.x << ',' << previousCamera.position.y << ',' << previousCamera.position.z
        << "]}"
        << ",\n  \"stage\": \"raygen-spectral-playground\",\n  \"fixture\": "
        << (options.fixture ? "true" : "false") << ",\n  \"receiverWatts\": " << receiverWatts
        << ",\n  \"cameraPathTotals\": [" << cameraGlassPixels << ", " << cameraTransmissions << ", "
        << cameraTir << ", " << cameraReflections << "]"
        << ",\n  \"cameraPathColumns\": "
           "[\"glassPixels\",\"transmissions\",\"totalInternalReflections\",\"reflections\"]"
        << ",\n  \"cameraTruncatedLastFrame\": " << counters[13] << ",\n  \"adapter\": \"" << adapterName
        << "\",\n  \"shaderModel\": " << shaderModel << ", \"raytracingTier\": " << raytracingTier
        << ",\n  \"hitMode\": " << hitMode << ", \"serActive\": " << (reorder ? "true" : "false")
        << ", \"standardActuallyReorders\": " << (standardReorders ? "true" : "false")
        << ", \"nvapiActuallyReorders\": " << (nvReorders ? "true" : "false")
        << ",\n  \"floatAtomics\": " << (floatAtomics ? "true" : "false")
        << ", \"pipelineStackBytes\": " << stackBytes << ",\n  \"frames\": " << frame
        << ", \"historyResets\": " << historyResets << ", \"dlssEvaluations\": " << dlss.evaluations
        << ", \"rrHistoryResets\": " << rrHistoryResets << ", \"fluidHistoryResets\": " << fluidHistoryResets
        << ", \"fluidTemporalTest\": " << (options.fluidTemporalTest ? "true" : "false")
        << ",\n  \"restirPT\": {\"enabled\": " << (options.restirPt ? "true" : "false")
        << ", \"scope\": \"opaque-primary diffuse indirect paths\", \"temporalFrames\": " << ptTemporalFrames
        << ", \"spatialNeighbors\": " << options.ptSpatial << ", \"historyCap\": " << options.ptHistory
        << ", \"allocatedBytes\": " << ptAllocatedBytes << ", \"invalidLastFrame\": " << counters[40]
        << ", \"pixelsLastFrame\": " << counters[41] << ", \"reusedLastFrame\": " << counters[42]
        << ", \"temporalMedianMs\": " << ptMedians[0] << ", \"spatialMedianMs\": " << ptMedians[1] << "}"
        << ",\n  \"fluidTightTraversal\": " << (options.fluidFullBrickTraversal ? "false" : "true")
        << ", \"pacedPresentation\": " << (frameLatencyEvent ? "true" : "false")
        << ", \"orbitTest\": " << (options.orbitTest ? "true" : "false")
        << ", \"rollingTest\": " << (options.rollingTest ? "true" : "false") << ",\n  \"waterEnabled\": "
        << (!options.fixture && (options.water || fluidSurface) ? "true" : "false")
        << ", \"flatWater\": " << (options.flatWater ? "true" : "false")
        << ", \"lasersEnabled\": " << (!options.fixture && options.lasers ? "true" : "false")
        << ", \"laserNm\": " << laserWavelength << ", \"laserWattsEach\": 1.5"
        << ", \"deepPool\": " << (options.fluidDeepPool ? "true" : "false")
        << ", \"waterFloodWatts\": " << (options.oceanLab ? (experience.environment ? 0 : oceanSun.w * 65536.) : (options.fluidDeepPool ? 2240 : (options.largeWaterLab ? 560 : (options.fluidRoom ? 140 : 28))))
        << ", \"waterPhotonEntries\": " << counters[14]
        << ", \"waterFloorWatts\": " << counters[15] / energyLedgerScale << ", \"laserDeposits\": " << counters[16]
        << ", \"beamSegments\": " << counters[17] << ", \"beamTruncated\": " << counters[18]
        << ",\n  \"photonEnergyWatts\": [" << counters[20] / energyLedgerScale << ", " << counters[21] / energyLedgerScale
        << ", " << counters[22] / energyLedgerScale << ", " << counters[23] / energyLedgerScale << ", "
        << counters[24] / energyLedgerScale << ", " << counters[25] / energyLedgerScale << ", " << counters[26] / energyLedgerScale
        << "]"
        << ",\n  \"photonEnergyColumns\": "
           "[\"emitted\",\"deposited\",\"mediumLoss\",\"escaped\",\"unmappedOrBlocked\",\"truncated\","
           "\"excludedEntranceReflection\"]"
        << ",\n  \"dlssMode\": \""
        << (options.quality == 0 ? "quality" : (options.quality == 1 ? "balanced" : "performance")) << "\""
        << ",\n  \"internalWidth\": " << renderWidth << ", \"internalHeight\": " << renderHeight
        << ", \"outputWidth\": " << options.width << ", \"outputHeight\": " << options.height
        << ",\n  \"photonsPerFrame\": " << options.photons << ", \"historyLength\": " << options.history
        << ", \"warmupFrames\": 32, \"sampleCount\": " << samples.size() << ",\n  \"medianMs\": [";
    for (int i = 0; i < 11; ++i)
        out << (i ? ", " : "") << medians[i];
    out << "],\n  \"prismAngle\": " << lastPrismAngle << ",\n  \"transportHash\": \"" << lastHash
        << "\",\n  \"timingColumns\": "
           "[\"photons\",\"atlasEma\",\"camera\",\"composite\",\"dlssRR\",\"frame\","
           "\"fluidSimulation\",\"fluidReconstruction\",\"fluidBlas\",\"whitewaterSimulation\","
           "\"whitewaterBlas\"],\n  \"photonCounters\": "
           "[";
    for (int i = 0; i < 8; ++i)
        out << (i ? ", " : "") << counters[i];
    out << "],\n  \"counterColumns\": "
           "[\"emitted\",\"entered\",\"exited\",\"splat\",\"truncated\",\"overflow\",\"invalid\","
           "\"footprintCapped\"],\n  \"samplesMs\": [";
    for (size_t i = 0; i < samples.size(); ++i) {
        out << (i ? ",\n    [" : "\n    [");
        for (int j = 0; j < 11; ++j)
            out << (j ? ", " : "") << samples[i][j];
        out << "]";
    }
    out << "\n  ]";
    out << ",\n  \"frameGeneration\":";
    dlss.report(out);
    out << ",\n  \"frameGenerationDistortionUpdates\":" << fgDistortionUpdates;
    out << ",\n  \"frameGenerationDistortionFov\":" << fgDistortionFov;
    if (latency) {
        out << ",\n  \"latency\":";
        latency->report(out);
    }
    out << ",\n  \"uiGeometryUploads\":{\"created\":" << uiGeometryUploads[0]
        << ",\"reused\":" << uiGeometryUploads[1] << ",\"cachedBytes\":" << uiGeometryUploads[2]
        << ",\"peakCachedBytes\":" << uiGeometryUploads[3] << '}';
    if (fluid) {
        out << ",\n  \"fluid\": ";
        fluid->validateAndReport(out);
    }
    out << ",\n  \"largeWaterLab\":" << (options.largeWaterLab ? "true" : "false");
    out << ",\n  \"oceanLab\":" << (options.oceanLab ? "true" : "false");
    if (options.oceanLab)
        out << ",\n  \"oceanEnvironment\":{\"timeOfDay\":\"" << (experience.environment ? "night" : "day")
            << "\",\"referenceLux\":" << (experience.environment ? .002 : 80000.)
            << ",\"exposure\":" << (experience.environment ? 4096. : 1./128)
            << ",\"activeLanterns\":" << (experience.environment ? ocean::OCEAN_LANTERN_COUNT : 0)
            << ",\"lanternLumensEach\":" << ocean::OCEAN_LANTERN_LUMENS
            << ",\"interactiveWidthMetres\":256,\"interactiveLengthMetres\":256}";
    out << ",\n  \"waterPath\":\"" << (options.hamiltonian.enabled ? "hamiltonian" : "baseline") << '"';
    if (fluid && fluid->hamiltonian) {
        out << ",\n  \"hamiltonian\":";
        fluid->hamiltonian->report(out);
    }
    if (fluidSurface) {
        out << ",\n  \"fluidSurface\": ";
        fluidSurface->report(out);
        if (fluidComplexity) {
            out << ",\n  \"complexity\": ";
            fluidComplexity->report(out);
        }
        out << ",\n  \"fluidProbes\":{\"hits\":" << counters[27] << ",\"badRoots\":" << counters[28]
            << ",\"glassShellHits\":" << counters[19] << ",\"truncated\":" << counters[29]
            << ",\"cellSteps\":" << counters[30] << ",\"intersections\":" << counters[31] << "}";
        if (options.fluidValidate && (counters[28] || counters[29]))
            throw std::runtime_error("Fluid procedural intersection validation failed");
    }
    if (whitewater) {
        out << ",\n  \"whitewater\": ";
        whitewater->report(out);
        out << ",\n  \"whitewaterProbes\":{\"foam\":" << counters[35] << ",\"bubbles\":" << counters[36]
            << ",\"spray\":" << counters[37] << ",\"bad\":" << counters[38] << '}';
        if (options.fluidValidate && counters[38])
            throw std::runtime_error("Whitewater DXR optics validation failed");
    }
    out << "\n}\n";
    if (!out)
        throw std::runtime_error("Cannot finish lab report");
}
void Renderer::capture(const std::filesystem::path &prefix) {
    // Fenced, tightly packed raw textures for reference comparisons, plus the displayed image.
    struct Readback {
        Buffer buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT rows;
        UINT64 rowBytes;
    };
    std::array<ID3D12Resource *, 12> resources{};
    for (int i = 0; i < 8; ++i)
        resources[i] = guides[i].Get();
    resources[8] = caustics.Get();
    resources[9] = backbuffers[lastBuffer].Get();
    resources[10] = fluidSurface ? fluidCaustics.Get() : nullptr;
    resources[11] = fgDistortionFov >= 0 ? fgDistortion.Get() : nullptr;
    const int resourceCount = resources[11] ? 12 : fluidSurface ? 11 : 10;
    std::array<Readback, 12> read{};
    begin();
    for (int i = 0; i < resourceCount; ++i) {
        if (!resources[i])
            continue;
        auto desc = resources[i]->GetDesc();
        UINT64 size;
        auto &r = read[i];
        device->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, &r.rows, &r.rowBytes, &size);
        r.buffer =
            buffer(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto state = i == 9 ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        transition(commands.Get(), resources[i], state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = resources[i];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = r.buffer.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = r.footprint;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(commands.Get(), resources[i], D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    }
    submit(false);
    std::ofstream raw(prefix.string() + ".inputs", std::ios::binary),
        ppm(prefix.string() + ".ppm", std::ios::binary);
    if (!raw || !ppm)
        throw std::runtime_error("Cannot create lab captures");
    raw.write("PTGI0001", 8);
    uint32_t count = fluidSurface ? 10 : 9;
    raw.write(reinterpret_cast<char *>(&count), 4);
    ppm << "P6\n" << options.width << " " << options.height << "\n255\n";
    for (int i = 0; i < resourceCount; ++i) {
        if (!resources[i])
            continue;
        auto &r = read[i];
        void *ptr;
        D3D12_RANGE range{0, SIZE_T(r.buffer.resource->GetDesc().Width)};
        check(r.buffer.resource->Map(0, &range, &ptr), "Map capture");
        if (i == 11) {
            // Optional diagnostic beside the existing capture format. Read the
            // actual GPU map, including FP16 quantization and row pitch.
            std::ofstream distortion(prefix.string() + ".distortion", std::ios::binary);
            distortion.write("FGLENS01", 8);
            const uint32_t size[] = {r.footprint.Footprint.Width, r.footprint.Footprint.Height};
            distortion.write(reinterpret_cast<const char *>(size), sizeof(size));
            distortion.write(reinterpret_cast<const char *>(&fgDistortionFov), sizeof(fgDistortionFov));
            for (UINT y = 0; y < r.rows; ++y)
                distortion.write(static_cast<char *>(ptr) + r.footprint.Offset +
                                     size_t(y) * r.footprint.Footprint.RowPitch, r.rowBytes);
            if (!distortion)
                throw std::runtime_error("Cannot finish lens distortion capture");
        } else if (i != 9) {
            uint32_t header[] = {uint32_t(i == 10 ? 9 : i), r.footprint.Footprint.Width,
                                 r.footprint.Footprint.Height, uint32_t(r.footprint.Footprint.Format),
                                 uint32_t(r.rowBytes)};
            raw.write(reinterpret_cast<char *>(header), sizeof(header));
            for (UINT y = 0; y < r.rows; ++y)
                raw.write(static_cast<char *>(ptr) + r.footprint.Offset +
                              size_t(y) * r.footprint.Footprint.RowPitch,
                          r.rowBytes);
        } else {
            for (UINT y = 0; y < r.rows; ++y)
                for (UINT x = 0; x < options.width; ++x)
                    ppm.write(static_cast<char *>(ptr) + r.footprint.Offset +
                                  size_t(y) * r.footprint.Footprint.RowPitch + x * 4,
                              3);
        }
        r.buffer.resource->Unmap(0, nullptr);
    }
    if (!raw || !ppm)
        throw std::runtime_error("Cannot finish captures");
}
} // namespace lab
