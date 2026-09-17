#include "../src/gpu_cuda_interop.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <cuda.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
void cudaInteropTransform(void *stream, void *data, uint32_t count, uint32_t round);
using namespace lab;
using Microsoft::WRL::ComPtr;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Device {
    gpu::CudaInterop::ContextMode contextMode = gpu::CudaInterop::ContextMode::Primary;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12InfoQueue> info;
    HANDLE event = nullptr;
    uint64_t serial = 0;
    explicit Device(const std::filesystem::path &runtime) {
        ComPtr<ID3D12Debug1> debug;
        const HRESULT debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (SUCCEEDED(debugResult)) {
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
        }
        std::cout << "{\"gpuValidation\":" << (debug ? "true" : "false")
                  << ",\"debugHRESULT\":" << uint32_t(debugResult) << "}\n";
        ComPtr<IDXGIFactory6> factory;
        gpu::check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CUDA test DXGI");
        for (UINT i = 0; !device; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                throw std::runtime_error("No NVIDIA DX12 adapter for CUDA validation");
            DXGI_ADAPTER_DESC1 desc{};
            gpu::check(adapter->GetDesc1(&desc), "CUDA test adapter");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        }
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC q{};
        gpu::check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CUDA test queue");
        gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                   "CUDA test allocator");
        gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&cmd)),
                   "CUDA test command list");
        gpu::check(cmd->Close(), "CUDA test initial close");
        gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                   "CUDA test frame fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "CUDA test frame event");
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 4;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = 2;
        desc.pParameters = parameters;
        ComPtr<ID3DBlob> blob, error;
        gpu::check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                   "CUDA test root serialization");
        gpu::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root)),
                   "CUDA test root");
        auto code = gpu::bytes(runtime / "shaders/CudaInteropTest.dxil");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = root.Get();
        pso.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CUDA test pipeline");
    }
    ~Device() {
        if (event)
            CloseHandle(event);
    }
    void begin() {
        gpu::check(allocator->Reset(), "CUDA test allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "CUDA test command reset");
    }
    void wait() {
        gpu::check(queue->Signal(fence.Get(), ++serial), "CUDA test frame signal");
        gpu::check(fence->SetEventOnCompletion(serial, event), "CUDA test frame completion");
        require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0,
                "CUDA test timeout / possible unsignaled fence");
    }
    void submit() {
        gpu::check(cmd->Close(), "CUDA test close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        wait();
        if (info) {
            const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T bytes = 0;
                info->GetMessage(i, nullptr, &bytes);
                std::vector<char> storage(bytes);
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                gpu::check(info->GetMessage(i, message, &bytes), "CUDA test validation message");
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
            info->ClearStoredMessages();
        }
    }
    void dispatch(ID3D12Resource *buffer, uint32_t count, uint32_t allocated, uint32_t mode, uint32_t seed) {
        const uint32_t constants[]{count, allocated, mode, seed};
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetPipelineState(pipeline.Get());
        cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);
        cmd->SetComputeRootUnorderedAccessView(1, buffer->GetGPUVirtualAddress());
        cmd->Dispatch((allocated + 127) / 128, 1, 1);
        gpu::uav(cmd.Get(), buffer);
    }
    gpu::Buffer shared(uint64_t bytes) {
        return gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           L"CUDA interop / test data", D3D12_HEAP_FLAG_SHARED);
    }
};
void roundTrip(Device &d, uint32_t count, uint32_t frames, bool noop = false) {
    const uint32_t allocated = std::max(64u, count + 17), seed = 0x13579bdf;
    const uint64_t bytes = uint64_t(allocated) * 4;
    auto data = d.shared(bytes);
    auto readback = gpu::buffer(d.device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                D3D12_RESOURCE_STATE_COPY_DEST);
    const gpu::CudaInterop::Binding binding{data.resource.Get(), bytes};
    gpu::CudaInterop interop(d.device.Get(), {&binding, 1}, d.queue.Get(), d.contextMode);
    require(interop.graphicsContext() == (d.contextMode == gpu::CudaInterop::ContextMode::Graphics),
            "Wrong interop context mode");
    std::vector<uint32_t> expected(allocated, 0xcafebabe);
    for (uint32_t i = 0; i < count; ++i)
        expected[i] = (i * 1664525u) ^ seed;
    std::vector<double> spans, gaps;
    double workMs = 0;
    for (uint32_t round = 0; round < frames; ++round) {
        d.begin();
        if (!round)
            d.dispatch(data.resource.Get(), count, allocated, 0, seed);
        interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
            if (!noop)
                cudaInteropTransform(stream, interop.pointers()[0], count, round);
        });
        // This shader consumes CUDA's actual shared-memory writes. No upload or
        // readback is used between producers/consumers; the copy is diagnostic.
        d.dispatch(data.resource.Get(), count, allocated, 1, seed + round);
        gpu::transition(d.cmd.Get(), data.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        d.cmd->CopyBufferRegion(readback.resource.Get(), 0, data.resource.Get(), 0, bytes);
        gpu::transition(d.cmd.Get(), data.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        d.submit();
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t x = expected[i];
            if (!noop) {
                x ^= 0x9e3779b9u + round;
                x = ((x << 7) | (x >> 25)) + i;
            }
            expected[i] = (x * 3u) ^ (seed + round + i);
        }
        void *mapped = nullptr;
        D3D12_RANGE range{0, size_t(bytes)};
        gpu::check(readback.resource->Map(0, &range, &mapped), "CUDA test readback");
        const bool equal = std::memcmp(mapped, expected.data(), size_t(bytes)) == 0;
        D3D12_RANGE written{0, 0};
        readback.resource->Unmap(0, &written);
        require(equal, "DX12-CUDA-DX12 round trip or tail guard mismatch");
        auto timing = interop.collect();
        require(timing.submissions == round + 1 && std::isfinite(timing.spanMs) && timing.spanMs > 0 &&
                    std::isfinite(timing.cudaMs) && timing.cudaMs >= 0,
                "Invalid interop timing / submission count");
        if (round >= 2) {
            spans.push_back(timing.spanMs);
            gaps.push_back(timing.handoffMs);
            workMs += timing.cudaMs;
        }
    }
    std::sort(spans.begin(), spans.end());
    std::sort(gaps.begin(), gaps.end());
    std::cout << "{\"case\":\"" << (noop ? "empty-handoff" : "round-trip") << "\",\"device\":\""
              << interop.deviceName() << "\",\"words\":" << count << ",\"frames\":" << frames
              << ",\"contextMode\":\"" << (interop.graphicsContext() ? "cig" : "primary") << '"'
              << ",\"cigSharedMemoryBytes\":" << interop.graphicsSharedMemoryBytes()
              << ",\"spanMedianMs\":" << spans[spans.size() / 2] << ",\"spanMaxMs\":" << spans.back()
              << ",\"handoffMedianMs\":" << gaps[gaps.size() / 2]
              << ",\"cudaMeanMs\":" << workMs / spans.size() << ",\"guardWords\":" << allocated - count
              << ",\"pass\":true}\n";
}
template <class F> void rejects(F &&f) {
    bool rejected = false;
    try {
        f();
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    require(rejected, "Invalid CUDA interop input was accepted");
}
void failureCases(Device &d) {
    auto good = d.shared(512);
    auto bad = gpu::buffer(d.device.Get(), 512, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const gpu::CudaInterop::Binding valid{good.resource.Get(), 512}, notShared{bad.resource.Get(), 512};
    rejects([&] { gpu::CudaInterop invalid(nullptr, {&valid, 1}, d.queue.Get(), d.contextMode); });
    rejects([&] { gpu::CudaInterop invalid(d.device.Get(), {}, d.queue.Get(), d.contextMode); });
    const gpu::CudaInterop::Binding tooLarge{good.resource.Get(), 516};
    rejects([&] { gpu::CudaInterop invalid(d.device.Get(), {&tooLarge, 1}, d.queue.Get(), d.contextMode); });
    const std::array duplicate{valid, valid};
    rejects([&] { gpu::CudaInterop invalid(d.device.Get(), duplicate, d.queue.Get(), d.contextMode); });
    // Second import fails after the first mapping already exists. Repetition
    // catches leaked NT handles and validates partially constructed ownership.
    const std::array partial{valid, notShared};
    rejects([&] {
        gpu::CudaInterop invalid(d.device.Get(), partial, d.queue.Get(), d.contextMode);
    }); // warm runtime caches
    DWORD before = 0, after = 0;
    require(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "Read process handle count");
    for (int i = 0; i < 16; ++i)
        rejects([&] { gpu::CudaInterop invalid(d.device.Get(), partial, d.queue.Get(), d.contextMode); });
    require(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "Read process handle count");
    require(after <= before, "Partial CUDA imports leaked Win32 handles");
    for (bool afterKernel : {false, true}) {
        auto failing = d.shared(512);
        const gpu::CudaInterop::Binding failingBinding{failing.resource.Get(), 512};
        gpu::CudaInterop interop(d.device.Get(), {&failingBinding, 1}, d.queue.Get(), d.contextMode);
        d.begin();
        d.dispatch(failing.resource.Get(), 128, 128, 0, 17);
        bool injected = false;
        try {
            interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
                if (afterKernel)
                    cudaInteropTransform(stream, interop.pointers()[0], 128, 0);
                throw std::runtime_error("injected enqueue failure");
            });
        } catch (const std::runtime_error &e) {
            injected = std::string(e.what()) == "injected enqueue failure";
        }
        require(injected && interop.failed(), "Enqueue failure did not poison CUDA interop");
        rejects([&] { interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [](void *) {}); });
        rejects([&] { interop.collect(); });
        d.wait(); // must not be stuck on a release signal that was never enqueued
        // execute failed after submission: list is closed; the next test begins
        // normally after the frame wait and the CUDA destructor's teardown drain.
        // The failed resource remains COMMON and is discarded after CUDA drains.
    }
    std::cout << "{\"case\":\"failure-cleanup\",\"handlesBefore\":" << before << ",\"handlesAfter\":" << after
              << ",\"pass\":true}\n";
}
void contexts(Device &d) {
    require(cuInit(0) == CUDA_SUCCESS, "Initialize context test");
    CUcontext before = nullptr, current = nullptr, first = nullptr;
    require(cuCtxGetCurrent(&before) == CUDA_SUCCESS, "Caller context");
    auto data = d.shared(512);
    const gpu::CudaInterop::Binding binding{data.resource.Get(), 512};
    const auto mode = gpu::CudaInterop::ContextMode::Graphics;
    rejects([&] { gpu::CudaInterop invalid(d.device.Get(), {&binding, 1}, nullptr, mode); });
    ComPtr<ID3D12CommandQueue> otherQueue, computeQueue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    gpu::check(d.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&otherQueue)), "Other CIG queue");
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    gpu::check(d.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&computeQueue)), "Compute CIG queue");
    rejects([&] { gpu::CudaInterop invalid(d.device.Get(), {&binding, 1}, computeQueue.Get(), mode); });
    require(cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == before,
            "Rejected CIG setup changed caller context");
    {
        // Exercise coexistence with another library's primary CUDA context.
        // The renderer owns one CIG context. This driver rejects a second
        // simultaneous CIG context with INVALID_VALUE; do not depend on it.
        struct Caller {
            CUdevice device{};
            CUcontext context = nullptr;
            Caller() {
                require(cuDeviceGet(&device, 0) == CUDA_SUCCESS, "Caller CUDA device");
                require(cuDevicePrimaryCtxRetain(&context, device) == CUDA_SUCCESS, "Retain caller");
                if (cuCtxPushCurrent(context) != CUDA_SUCCESS) {
                    cuDevicePrimaryCtxRelease(device);
                    throw std::runtime_error("Activate caller");
                }
            }
            ~Caller() {
                CUcontext popped = nullptr;
                cuCtxPopCurrent(&popped);
                cuDevicePrimaryCtxRelease(device);
            }
        } caller;
        gpu::CudaInterop a(d.device.Get(), {&binding, 1}, d.queue.Get(), mode);
        require(a.graphicsSharedMemoryBytes() > 0, "Missing CIG shared-memory limit");
        require(cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == caller.context,
                "CIG construction leaked current context");
        rejects([&] { a.execute(d.cmd.Get(), otherQueue.Get(), d.allocator.Get(), [](void *) {}); });
        require(!a.failed(), "Rejected CIG queue poisoned a context before submission");
        {
            const auto outer = a.activate();
            require(cuCtxGetCurrent(&first) == CUDA_SUCCESS && first && first != caller.context,
                    "CIG scope not bound");
            CUdeviceptr memory = 0;
            require(cuMemAlloc(&memory, 16) == CUDA_SUCCESS, "Scoped CUDA allocation");
            try {
                const auto inner = a.activate();
                require(cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == first,
                        "Nested CIG scope did not retain its owner");
                throw 1;
            } catch (int) {
            }
            require(cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == first,
                    "Exception did not restore outer CIG context");
            require(cuMemFree(memory) == CUDA_SUCCESS, "Scoped CUDA release");
        }
        require(cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == caller.context, "CIG scope leaked");
    }
    require(cuCtxGetCurrent(&current) == CUDA_SUCCESS && current == before, "CIG teardown changed caller");
    std::cout << "{\"case\":\"cig-context-lifetime\",\"pass\":true}\n";
}
} // namespace
int main(int argc, char **argv) try {
    require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--cig"),
            "Pass runtime folder and optional --cig");
    Device d(argv[1]);
    if (argc == 3) {
        d.contextMode = gpu::CudaInterop::ContextMode::Graphics;
        contexts(d);
    }
    roundTrip(d, 1, 6);
    failureCases(d);
    for (uint32_t count : {63u, 65u, 4097u, 65539u, 1048577u})
        roundTrip(d, count, 12);
    roundTrip(d, 1, 32, true);
    std::cout << "PASS CUDA interop: shared GPU data, fence ordering, allocation tails, repeated teardown "
                 "and failures\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA interop: " << e.what() << '\n';
    return 1;
}
