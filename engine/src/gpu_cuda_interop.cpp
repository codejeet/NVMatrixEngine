#include "gpu_cuda_interop.h"
#include <chrono>
#include <cuda.h>
#include <cuda_runtime_api.h>
#include <cstring>
#include <limits>

namespace lab::gpu {
namespace {
void driverCheck(CUresult result, const char *where) {
    if (result != CUDA_SUCCESS) {
        const char *message = nullptr;
        cuGetErrorString(result, &message);
        throw std::runtime_error(std::string(where) + ": " + (message ? message : "CUDA driver error"));
    }
}
void cudaCheck(cudaError_t result, const char *where) {
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(result));
}
template <class T> ComPtr<T> nativeInterface(T *object) {
    // Streamline's documented native-interface QI contract (Programming Guide
    // section 5.3). CUDA must see the physical queue, not the DLSS proxy. This
    // requires no SL DLL/header dependency in the standalone interop library.
    constexpr GUID nativeId{0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
    ComPtr<T> native;
    const HRESULT result = object->QueryInterface(nativeId, reinterpret_cast<void **>(native.GetAddressOf()));
    if (result == E_NOINTERFACE)
        return object;
    check(result, "Resolve CUDA native DX12 interface");
    if (!native)
        throw std::runtime_error("DX12 proxy returned an empty native interface");
    return native;
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value)
            CloseHandle(value);
    }
};
struct SharedBuffer {
    ComPtr<ID3D12Resource> resource;
    cudaExternalMemory_t memory = nullptr;
    void *pointer = nullptr;
    D3D12_RESOURCE_STATES state{};
    ~SharedBuffer() {
        if (pointer)
            cudaFree(pointer);
        if (memory)
            cudaDestroyExternalMemory(memory);
    }
};
} // namespace
struct CudaInterop::Impl {
    ComPtr<ID3D12CommandQueue> submissionQueue;
    ComPtr<ID3D12CommandQueue> graphicsQueue;
    CUcontext graphicsContext = nullptr;
    size_t graphicsSharedMemory = 0;
    bool graphicsRequested = false;
    std::vector<std::unique_ptr<SharedBuffer>> buffers;
    std::vector<void *> pointers;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> queries;
    Buffer timestamps;
    cudaExternalSemaphore_t semaphore = nullptr;
    cudaStream_t stream = nullptr;
    cudaEvent_t start = nullptr, end = nullptr;
    int device = -1;
    uint64_t sequence = 0, finished = 0, frequency = 0;
    Timing timing{};
    bool poisoned = false;
    std::string name;
    ~Impl() {
        // Teardown only. The renderer must finish its DX12 frame before releasing
        // any shared resources, just as for its other GPU subsystems.
        const bool active = graphicsContext ? cuCtxPushCurrent(graphicsContext) == CUDA_SUCCESS
                            : !graphicsRequested && device >= 0 ? cudaSetDevice(device) == cudaSuccess
                                                                : false;
        if (!active && graphicsContext) {
            // A lost context owns the remaining CUDA allocations. Destroy it
            // before dropping DX12 references; do not free its handles in an
            // unrelated caller's CUDA context.
            cuCtxDestroy(graphicsContext);
            graphicsContext = nullptr;
        }
        if (!active) {
            for (auto &b : buffers) {
                b->pointer = nullptr;
                b->memory = nullptr;
            }
            return;
        }
        if (stream)
            cudaStreamSynchronize(stream);
        if (start)
            cudaEventDestroy(start);
        if (end)
            cudaEventDestroy(end);
        if (semaphore)
            cudaDestroyExternalSemaphore(semaphore);
        if (stream)
            cudaStreamDestroy(stream);
        buffers.clear();
        if (graphicsContext) {
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
            cuCtxDestroy(graphicsContext);
        }
    }
};
CudaInterop::ContextScope::ContextScope(void *context) {
    if (context) {
        driverCheck(cuCtxPushCurrent(static_cast<CUcontext>(context)), "Activate CUDA graphics context");
        pushed = true;
    }
}
CudaInterop::ContextScope::~ContextScope() {
    if (pushed) {
        CUcontext context = nullptr;
        cuCtxPopCurrent(&context);
    }
}
CudaInterop::ContextScope CudaInterop::activate() const {
    return ContextScope(impl->graphicsContext);
}
bool CudaInterop::graphicsContext() const {
    return impl->graphicsContext != nullptr;
}
uint64_t CudaInterop::graphicsSharedMemoryBytes() const {
    return impl->graphicsSharedMemory;
}
CudaInterop::CudaInterop(ID3D12Device *device, std::span<const Binding> bindings, ID3D12CommandQueue *queue,
                         ContextMode mode)
    : impl(std::make_unique<Impl>()) {
    impl->graphicsRequested = mode == ContextMode::Graphics;
    if (!device || device->GetNodeCount() != 1 || bindings.empty())
        throw std::runtime_error("CUDA interop requires a single-node device and shared buffers");
    const LUID luid = device->GetAdapterLuid();
    int count = 0;
    driverCheck(cuInit(0), "Initialize CUDA driver");
    driverCheck(cuDeviceGetCount(&count), "Enumerate CUDA devices");
    for (int i = 0; i < count; ++i) {
        char cudaLuid[8]{}, name[256]{};
        unsigned nodeMask = 0;
        CUdevice candidate;
        driverCheck(cuDeviceGet(&candidate, i), "CUDA ordinal");
        driverCheck(cuDeviceGetLuid(cudaLuid, &nodeMask, candidate), "CUDA adapter LUID");
        if (!std::memcmp(cudaLuid, &luid, sizeof(luid)) && nodeMask == 1) {
            impl->device = i;
            driverCheck(cuDeviceGetName(name, sizeof(name), candidate), "CUDA device name");
            impl->name = name;
            break;
        }
    }
    if (impl->device < 0)
        throw std::runtime_error("No CUDA device matches the DX12 adapter LUID");
    if (mode == ContextMode::Graphics) {
#if CUDA_VERSION < 13010
        throw std::runtime_error("CUDA CIG requires a CUDA 13.1+ build; select --fluid-cuda-context=primary");
#else
        if (!queue || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::runtime_error("CUDA CIG requires the renderer's direct queue");
        impl->submissionQueue = queue;
        impl->graphicsQueue = nativeInterface(queue);
        ComPtr<ID3D12Device> queueDevice;
        check(impl->graphicsQueue->GetDevice(IID_PPV_ARGS(&queueDevice)), "CUDA CIG queue device");
        ComPtr<IUnknown> queueIdentity, deviceIdentity;
        check(nativeInterface(queueDevice.Get()).As(&queueIdentity), "CUDA queue device identity");
        check(nativeInterface(device).As(&deviceIdentity), "CUDA resource device identity");
        if (queueIdentity != deviceIdentity)
            throw std::runtime_error("CUDA CIG queue belongs to another DX12 device");
        CUdevice selected;
        driverCheck(cuDeviceGet(&selected, impl->device), "CUDA CIG device");
        int supported = 0;
        driverCheck(cuDeviceGetAttribute(&supported, CU_DEVICE_ATTRIBUTE_D3D12_CIG_SUPPORTED, selected),
                    "Query CUDA CIG support");
        if (!supported)
            throw std::runtime_error("CUDA CIG is unsupported; select --fluid-cuda-context=primary");
        CUctxCigParam cig{CIG_DATA_TYPE_D3D12_COMMAND_QUEUE, impl->graphicsQueue.Get()};
        CUctxCreateParams params{};
        params.cigParams = &cig;
        driverCheck(cuCtxCreate(&impl->graphicsContext, &params, CU_CTX_SCHED_AUTO, selected),
                    "Create CUDA graphics context");
        CUcontext popped = nullptr;
        driverCheck(cuCtxPopCurrent(&popped), "Restore caller after CUDA CIG creation");
#endif
    } else {
        cudaCheck(cudaSetDevice(impl->device), "Select DX12-matched CUDA device");
    }
    const auto context = activate();
#if CUDA_VERSION >= 13010
    if (impl->graphicsContext) {
        size_t enabled = 0;
        driverCheck(cuCtxGetLimit(&enabled, CU_LIMIT_CIG_ENABLED), "Confirm CUDA CIG mode");
        if (!enabled)
            throw std::runtime_error("CUDA did not enable the requested graphics context");
        driverCheck(cuCtxGetLimit(&impl->graphicsSharedMemory, CU_LIMIT_SHMEM_SIZE),
                    "CUDA CIG shared memory");
        // A kernel exceeding the graphics context's shared-memory allowance
        // must fail explicitly, not invisibly schedule in a separate context.
        driverCheck(cuCtxSetLimit(CU_LIMIT_CIG_SHMEM_FALLBACK_ENABLED, 0),
                    "Disable hidden CUDA CIG fallback");
    }
#endif
    cudaCheck(cudaStreamCreateWithFlags(&impl->stream, cudaStreamNonBlocking), "CUDA interop stream");
    cudaCheck(cudaEventCreate(&impl->start), "CUDA start event");
    cudaCheck(cudaEventCreate(&impl->end), "CUDA end event");
    for (const auto &binding : bindings) {
        if (!binding.resource || !binding.minimumBytes)
            throw std::runtime_error("Missing/empty CUDA shared resource");
        for (const auto &previous : impl->buffers)
            if (previous->resource.Get() == binding.resource)
                throw std::runtime_error("Duplicate CUDA shared resource binding");
        // Insert before importing so every partial-construction failure has an
        // owner for its external memory and mapped address.
        auto &b = impl->buffers.emplace_back(std::make_unique<SharedBuffer>());
        b->resource = binding.resource;
        b->state = binding.state;
        ComPtr<ID3D12Device> owner;
        check(b->resource->GetDevice(IID_PPV_ARGS(&owner)), "CUDA buffer device");
        const LUID ownerLuid = owner->GetAdapterLuid();
        if (std::memcmp(&ownerLuid, &luid, sizeof(luid)))
            throw std::runtime_error("CUDA resource belongs to another adapter");
        const auto desc = b->resource->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width < binding.minimumBytes)
            throw std::runtime_error("CUDA shared buffer dimension/size mismatch");
        D3D12_HEAP_PROPERTIES hp{};
        D3D12_HEAP_FLAGS flags{};
        check(b->resource->GetHeapProperties(&hp, &flags), "CUDA shared heap properties");
        if (hp.Type != D3D12_HEAP_TYPE_DEFAULT || !(flags & D3D12_HEAP_FLAG_SHARED))
            throw std::runtime_error("CUDA buffer must use committed shared DEFAULT memory");
        const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        if (allocation.SizeInBytes == std::numeric_limits<uint64_t>::max() ||
            allocation.SizeInBytes < desc.Width)
            throw std::runtime_error("Invalid CUDA shared allocation size");
        Handle handle;
        check(device->CreateSharedHandle(b->resource.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value),
              "Export CUDA buffer");
        cudaExternalMemoryHandleDesc import{};
        import.type = cudaExternalMemoryHandleTypeD3D12Resource;
        import.handle.win32.handle = handle.value;
        import.size = allocation.SizeInBytes;
        import.flags = cudaExternalMemoryDedicated;
        cudaCheck(cudaImportExternalMemory(&b->memory, &import), "Import DX12 buffer into CUDA");
        cudaExternalMemoryBufferDesc map{};
        map.size = desc.Width;
        cudaCheck(cudaExternalMemoryGetMappedBuffer(&b->pointer, b->memory, &map), "Map DX12 buffer in CUDA");
        impl->pointers.push_back(b->pointer);
    }
    check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&impl->fence)), "CUDA shared fence");
    impl->fence->SetName(L"Fluid / DX12-CUDA timeline");
    Handle handle;
    check(device->CreateSharedHandle(impl->fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value),
          "Export CUDA fence");
    cudaExternalSemaphoreHandleDesc sync{};
    sync.type = cudaExternalSemaphoreHandleTypeD3D12Fence;
    sync.handle.win32.handle = handle.value;
    cudaCheck(cudaImportExternalSemaphore(&impl->semaphore, &sync), "Import DX12 fence into CUDA");
    D3D12_QUERY_HEAP_DESC query{};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = 2;
    check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&impl->queries)), "CUDA handoff timestamp heap");
    impl->timestamps = buffer(device, 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                              D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid / CUDA handoff timing");
}
CudaInterop::~CudaInterop() = default;
std::span<void *const> CudaInterop::pointers() const {
    return impl->pointers;
}
const std::string &CudaInterop::deviceName() const {
    return impl->name;
}
bool CudaInterop::failed() const {
    return impl->poisoned;
}
void CudaInterop::drainForTeardown() noexcept {
    if (impl->graphicsContext) {
        if (cuCtxPushCurrent(impl->graphicsContext) == CUDA_SUCCESS) {
            if (impl->stream)
                cudaStreamSynchronize(impl->stream);
            CUcontext popped = nullptr;
            cuCtxPopCurrent(&popped);
        }
        return;
    }
    if (impl->device >= 0)
        cudaSetDevice(impl->device);
    if (impl->stream)
        cudaStreamSynchronize(impl->stream);
}
void CudaInterop::execute(ID3D12GraphicsCommandList *cmd, ID3D12CommandQueue *queue,
                          ID3D12CommandAllocator *allocator, const std::function<void(void *)> &enqueue,
                          SubmissionTimeline *timeline) {
    stamp(timeline, SubmissionStage::InteropBegin);
    if (impl->poisoned)
        throw std::runtime_error("CUDA interop is poisoned after a failed submission");
    if (!cmd || !queue || !allocator || !enqueue || cmd->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT ||
        queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        throw std::runtime_error("CUDA interop requires a direct renderer submission context");
    if (impl->finished && impl->fence->GetCompletedValue() < impl->finished)
        throw std::runtime_error("CUDA interop expects the previous frame to be complete");
    if (impl->submissionQueue && queue != impl->submissionQueue.Get())
        throw std::runtime_error("CUDA CIG submission changed its renderer queue");
    const auto context = activate();
    if (!impl->graphicsContext)
        cudaCheck(cudaSetDevice(impl->device), "Select CUDA interop device");
    check(queue->GetTimestampFrequency(&impl->frequency), "CUDA handoff timestamp frequency");
    stamp(timeline, SubmissionStage::InteropContextReady);
    try {
        cmd->EndQuery(impl->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        for (const auto &b : impl->buffers)
            if (b->state != D3D12_RESOURCE_STATE_COMMON)
                transition(cmd, b->resource.Get(), b->state, D3D12_RESOURCE_STATE_COMMON);
        check(cmd->Close(), "Close CUDA handoff prefix");
        stamp(timeline, SubmissionStage::PrefixClosed);
        ID3D12CommandList *lists[]{cmd};
        queue->ExecuteCommandLists(1, lists);
        stamp(timeline, SubmissionStage::PrefixSubmitted);
        const uint64_t ready = ++impl->sequence, finished = ++impl->sequence;
        check(queue->Signal(impl->fence.Get(), ready), "DX12 release to CUDA");
        stamp(timeline, SubmissionStage::DxReleased);
        cudaExternalSemaphoreWaitParams wait{};
        wait.params.fence.value = ready;
        cudaCheck(cudaWaitExternalSemaphoresAsync(&impl->semaphore, &wait, 1, impl->stream),
                  "CUDA wait for DX12");
        stamp(timeline, SubmissionStage::CudaWaitQueued);
        cudaCheck(cudaEventRecord(impl->start, impl->stream), "CUDA work start");
        stamp(timeline, SubmissionStage::CudaStartQueued);
        const auto encodeStart = std::chrono::steady_clock::now();
        enqueue(impl->stream);
        impl->timing.enqueueMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encodeStart).count();
        stamp(timeline, SubmissionStage::CudaWorkQueued);
        cudaCheck(cudaEventRecord(impl->end, impl->stream), "CUDA work end");
        stamp(timeline, SubmissionStage::CudaEndQueued);
        cudaExternalSemaphoreSignalParams signal{};
        signal.params.fence.value = finished;
        cudaCheck(cudaSignalExternalSemaphoresAsync(&impl->semaphore, &signal, 1, impl->stream),
                  "CUDA release to DX12");
        stamp(timeline, SubmissionStage::CudaReleaseQueued);
        // Do not queue this wait before the signal has successfully been enqueued.
        // In particular, callback/launch failure must not deadlock the DX12 queue.
        check(queue->Wait(impl->fence.Get(), finished), "DX12 GPU wait for CUDA");
        stamp(timeline, SubmissionStage::DxWaitQueued);
        check(cmd->Reset(allocator, nullptr), "Resume DX12 after CUDA");
        for (const auto &b : impl->buffers)
            if (b->state != D3D12_RESOURCE_STATE_COMMON)
                transition(cmd, b->resource.Get(), D3D12_RESOURCE_STATE_COMMON, b->state);
        cmd->EndQuery(impl->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        cmd->ResolveQueryData(impl->queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                              impl->timestamps.resource.Get(), 0);
        impl->finished = finished;
        ++impl->timing.submissions;
        stamp(timeline, SubmissionStage::InteropResumed);
    } catch (...) {
        impl->poisoned = true;
        throw;
    }
}
CudaInterop::Timing CudaInterop::collect() {
    const auto context = activate();
    if (impl->poisoned)
        throw std::runtime_error("Cannot collect a failed CUDA submission");
    if (!impl->finished)
        return impl->timing;
    if (impl->fence->GetCompletedValue() < impl->finished)
        throw std::runtime_error("CUDA timing requested before completion");
    cudaCheck(cudaEventElapsedTime(&impl->timing.cudaMs, impl->start, impl->end), "CUDA work timing");
    void *mapped = nullptr;
    D3D12_RANGE range{0, 16};
    check(impl->timestamps.resource->Map(0, &range, &mapped), "Map CUDA timing");
    uint64_t ticks[2]{};
    std::memcpy(ticks, mapped, sizeof(ticks));
    D3D12_RANGE written{0, 0};
    impl->timestamps.resource->Unmap(0, &written);
    if (ticks[1] < ticks[0] || !impl->frequency)
        throw std::runtime_error("Invalid CUDA handoff timestamps");
    impl->timing.spanMs = double(ticks[1] - ticks[0]) * 1000.0 / double(impl->frequency);
    impl->timing.handoffMs = std::max(0.0, impl->timing.spanMs - impl->timing.cudaMs);
    return impl->timing;
}
} // namespace lab::gpu
