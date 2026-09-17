#include "../src/gpu_resources.h"
#include "../../shared/src/ui_renderer.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <iostream>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
void hrCheck(HRESULT result, const char *operation) {
    lab::gpu::check(result, operation);
}
using Microsoft::WRL::ComPtr;
using namespace lab;
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) try {
    require(argc == 2, "Pass the runtime folder");
    ComPtr<ID3D12Debug1> debug;
    const auto debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
    if (debug) {
        debug->EnableDebugLayer();
        debug->SetEnableGPUBasedValidation(TRUE);
    }
    ComPtr<IDXGIFactory6> factory;
    gpu::check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "UI test DXGI");
    ComPtr<IDXGIAdapter1> adapter;
    gpu::check(
        factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
        "UI test adapter");
    ComPtr<ID3D12Device> device;
    gpu::check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
               "UI test device");
    ComPtr<ID3D12InfoQueue> info;
    device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    gpu::check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "UI queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "UI allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;
    gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&commands)),
               "UI commands");
    gpu::check(commands->Close(), "UI initial close");
    ComPtr<ID3D12Fence> fence;
    gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "UI fence");
    struct Event {
        HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ~Event() {
            if (handle)
                CloseHandle(handle);
        }
    } event;
    require(event.handle != nullptr, "UI frame event");
    uint64_t serial = 0;
    auto submit = [&] {
        gpu::check(commands->Close(), "UI close");
        ID3D12CommandList *lists[]{commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "UI signal");
        gpu::check(fence->SetEventOnCompletion(serial, event.handle), "UI completion event");
        require(WaitForSingleObject(event.handle, 15000) == WAIT_OBJECT_0, "UI fixture timeout");
    };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = desc.Height = 16;
    desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> target;
    gpu::check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                               IID_PPV_ARGS(&target)),
               "UI target");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    gpu::check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "UI RTV heap");
    const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    auto readback = gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
    UiRenderer ui(device.Get(), std::filesystem::path(argv[1]) / "shaders/ui.hlsl");
    const std::array<int, 6> indices{0, 1, 2, 0, 2, 3};
    for (uint32_t frame = 0; frame < 64; ++frame) {
        gpu::check(allocator->Reset(), "UI reset allocator");
        gpu::check(commands->Reset(allocator.Get(), nullptr), "UI reset commands");
        ui.begin(commands.Get(), 16, 16);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const float clear[4]{};
        commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
        for (int half = 0; half < 2; ++half) {
            const Rml::ColourbPremultiplied colour = half        ? Rml::ColourbPremultiplied{0, 0, 255, 255}
                                                     : frame % 2 ? Rml::ColourbPremultiplied{0, 255, 0, 255}
                                                                 : Rml::ColourbPremultiplied{255, 0, 0, 255};
            const float x = float(half * 8);
            const std::array<Rml::Vertex, 4> vertices{{{{x, 0}, colour, {0, 0}},
                                                       {{x + 8, 0}, colour, {0, 0}},
                                                       {{x + 8, 16}, colour, {0, 0}},
                                                       {{x, 16}, colour, {0, 0}}}};
            auto geometry =
                ui.CompileGeometry({vertices.data(), vertices.size()}, {indices.data(), indices.size()});
            ui.RenderGeometry(geometry, {0, 0}, 0);
            // Releasing before ExecuteCommandLists must not let the second draw
            // overwrite storage still referenced by the first draw's commands.
            ui.ReleaseGeometry(geometry);
        }
        gpu::transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = target.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.resource.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu::transition(commands.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit();
        void *mapped = nullptr;
        D3D12_RANGE range{0, size_t(bytes)};
        gpu::check(readback.resource->Map(0, &range, &mapped), "UI pixels");
        auto *pixels = static_cast<const uint8_t *>(mapped);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) {
                auto *pixel = pixels + y * footprint.Footprint.RowPitch + x * 4;
                const int channel = x >= 8 ? 2 : frame % 2 ? 1 : 0;
                for (int c = 0; c < 4; ++c)
                    require(pixel[c] == (c == channel || c == 3 ? 255 : 0),
                            "UI upload lifetime/content mismatch");
            }
        D3D12_RANGE written{};
        readback.resource->Unmap(0, &written);
        require(ui.geometryUploadStats()[0] == 4, "Steady UI geometry still creates committed resources");
    }
    // Exceed both entry and byte cache budgets, without drawing the extra data.
    std::vector<Rml::Vertex> large(300000);
    for (int i = 0; i < 2; ++i)
        ui.ReleaseGeometry(
            ui.CompileGeometry({large.data(), large.size()}, {indices.data(), indices.size()}));
    std::array<Rml::Vertex, 4> quadVertices{};
    for (int i = 0; i < 300; ++i)
        ui.ReleaseGeometry(
            ui.CompileGeometry({quadVertices.data(), quadVertices.size()}, {indices.data(), indices.size()}));
    gpu::check(allocator->Reset(), "UI budget allocator");
    gpu::check(commands->Reset(allocator.Get(), nullptr), "UI budget commands");
    ui.begin(commands.Get(), 16, 16);
    submit();
    const auto stats = ui.geometryUploadStats();
    require(stats[1] == 252 && stats[2] <= 256 * 256 && stats[3] <= 8 * 1024 * 1024,
            "UI reuse count or cache budget mismatch");
    if (info) {
        for (uint64_t i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            info->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
            gpu::check(info->GetMessage(i, message, &size), "UI validation message");
            require(message->Severity > D3D12_MESSAGE_SEVERITY_ERROR, message->pDescription);
        }
    }
    std::cout << "{\"frames\":64,\"pixelsChecked\":16384,\"created\":" << stats[0]
              << ",\"reused\":" << stats[1] << ",\"cachedBytes\":" << stats[2]
              << ",\"peakCachedBytes\":" << stats[3] << ",\"gpuValidation\":" << (debug ? "true" : "false")
              << ",\"debugHRESULT\":" << uint32_t(debugResult) << ",\"pass\":true}\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL UI upload cache: " << e.what() << '\n';
    return 1;
}
