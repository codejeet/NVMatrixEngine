#pragma once
#include "gpu_resources.h"
#include "gpu_submission_profile.h"
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace lab::gpu {
// Available only in CUDA-enabled builds. The public header does not require
// CUDA headers; the existing renderer still owns all DX12 submissions.
class CudaInterop {
  public:
    enum class ContextMode { Primary, Graphics };
    // CUDA runtime allocations/captures must use the same context as imported
    // resources. A scope restores the caller's thread context; never leak CIG
    // selection into unrelated libraries (including DLSS).
    class ContextScope {
      public:
        ~ContextScope();
        ContextScope(const ContextScope &) = delete;
        ContextScope &operator=(const ContextScope &) = delete;

      private:
        friend class CudaInterop;
        explicit ContextScope(void *);
        bool pushed = false;
    };
    struct Binding {
        ID3D12Resource *resource;
        uint64_t minimumBytes;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    };
    struct Timing {
        uint64_t submissions = 0;
        float cudaMs = 0;
        double spanMs = 0, handoffMs = 0, enqueueMs = 0;
    };
    CudaInterop(ID3D12Device *, std::span<const Binding>, ID3D12CommandQueue * = nullptr,
                ContextMode = ContextMode::Primary);
    ~CudaInterop();
    CudaInterop(const CudaInterop &) = delete;
    CudaInterop &operator=(const CudaInterop &) = delete;
    std::span<void *const> pointers() const;
    const std::string &deviceName() const;
    [[nodiscard]] ContextScope activate() const;
    bool graphicsContext() const;
    uint64_t graphicsSharedMemoryBytes() const;

    // Caller closes PIX regions before this boundary and rebinds root/PSO state
    // afterwards. Submits the prefix, enqueues CUDA, then reopens the list without
    // resetting its in-flight allocator. No CPU wait in this method.
    // An enqueue failure poisons this instance: abandon the frame, do not render
    // partially updated buffers. No DX12 wait on an unsignaled CUDA fence is queued.
    void execute(ID3D12GraphicsCommandList *, ID3D12CommandQueue *, ID3D12CommandAllocator *,
                 const std::function<void(void *stream)> &enqueue, SubmissionTimeline * = nullptr);
    // Requires completion of the caller's existing frame fence, including the
    // resumed DX12 list. Single frame in flight, matching the current renderer.
    Timing collect();
    bool failed() const;
    // Teardown only, before freeing buffers used by partially enqueued work.
    void drainForTeardown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace lab::gpu
