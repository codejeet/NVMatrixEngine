#include "fluid_cuda_bricks.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
constexpr uint32_t absent = 0xffffffffu;
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
__global__ void beginFrame(BrickView v, uint32_t limit) {
    if (threadIdx.x || blockIdx.x || v.control[BrickInvalid])
        return;
    v.control[BrickRemaining] = limit;
    v.control[BrickFrameChanges] = 0;
}
// One bounded metadata transaction. Large field initialization is a separate
// parallel consumer pass. This first scheduler scans at most 16k virtual bricks;
// no inter-block spin lock, device allocation, or unbounded retry loop is used.
__global__ void stage(BrickView v, uint32_t *freeSlots) {
    if (threadIdx.x || blockIdx.x || v.control[BrickInvalid])
        return;
    auto c = v.control;
    const uint32_t back = 1 - c[BrickFront];
    c[BrickCandidate] = back;
    c[BrickReady] = 0;
    uint32_t freeCount = 0, required = 0, missing = 0, resident = 0;
    uint32_t budget = c[BrickRemaining], changed = 0;
    auto pages = v.pages[back];
    auto keys = v.keys[back];
    for (uint32_t slot = 0; slot < v.capacity; ++slot) {
        const uint32_t key = keys[slot];
        if (key != absent && (key >= v.bricks || pages[key] != slot)) {
            c[BrickInvalid] = 1;
            return;
        }
        if (key != absent && !v.requests[key] && budget) {
            pages[key] = absent;
            keys[slot] = absent;
            --budget;
            ++changed;
            ++c[BrickRetirements];
        }
        if (keys[slot] == absent)
            freeSlots[freeCount++] = slot;
    }
    for (uint32_t key = 0; key < v.bricks; ++key) {
        if (!v.requests[key])
            continue;
        ++required;
        uint32_t slot = pages[key];
        if (slot == absent && budget && freeCount) {
            slot = freeSlots[--freeCount];
            pages[key] = slot;
            keys[slot] = key;
            --budget;
            ++changed;
            ++c[BrickAllocations];
        }
        if (slot == absent)
            ++missing;
        else {
            if (slot >= v.capacity || keys[slot] != key) {
                c[BrickInvalid] = 1;
                return;
            }
            ++resident;
        }
    }
    c[BrickRemaining] = budget;
    c[BrickFrameChanges] += changed;
    c[BrickPeakFrameChanges] = max(c[BrickPeakFrameChanges], c[BrickFrameChanges]);
    c[BrickRequired] = required;
    c[BrickMissing] = missing;
    c[BrickResident] = resident;
    c[BrickBacklogPeak] = max(c[BrickBacklogPeak], missing);
    c[BrickResidentPeak] = max(c[BrickResidentPeak], resident);
    if (required > v.capacity)
        ++c[BrickOverflows];
    if (missing)
        ++c[BrickDeferrals];
    else if (!c[BrickInvalid])
        c[BrickReady] = 1;
}
__global__ void publish(BrickView v) {
    if (threadIdx.x || blockIdx.x)
        return;
    if (v.control[BrickReady] && !v.control[BrickInvalid]) {
        v.control[BrickFront] = v.control[BrickCandidate];
        ++v.control[BrickVersion];
        ++v.control[BrickCommits];
    }
}
__global__ void stageActive(BrickView v) {
    if (!v.control[BrickReady] || v.control[BrickInvalid])
        return;
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < v.bricks)
        v.active[v.control[BrickCandidate]][i] = v.requests[i] != 0;
}
__global__ void clearRequests(BrickView v) {
    if (v.control[BrickInvalid])
        return;
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < v.bricks)
        v.requests[i] = 0;
}
__global__ void mirrorPages(BrickView v) {
    if (!v.control[BrickReady] || v.control[BrickInvalid])
        return;
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x, front = v.control[BrickFront];
    if (i < v.bricks)
        v.pages[1 - front][i] = v.pages[front][i];
    if (i < v.capacity)
        v.keys[1 - front][i] = v.keys[front][i];
    // Only reservation metadata is mirrored. The next consumer must rebuild its
    // candidate fields before publishing; it must never treat stale back data as live.
}
} // namespace
struct BrickPool {
    BrickView view{};
    uint32_t limit = 0;
    uint32_t *freeSlots = nullptr;
    size_t bytes = 0;
    ~BrickPool() {
        for (auto p : view.pages)
            if (p)
                cudaFree(p);
        for (auto p : view.keys)
            if (p)
                cudaFree(p);
        for (auto p : view.active)
            if (p)
                cudaFree(p);
        for (auto p : view.fields)
            if (p)
                cudaFree(p);
        if (view.requests)
            cudaFree(view.requests);
        if (view.control)
            cudaFree(view.control);
        if (freeSlots)
            cudaFree(freeSlots);
    }
};
BrickPool *createBricks(const BrickConfig &cfg) {
    if (!cfg.nx || !cfg.ny || !cfg.nz || cfg.nx > 1048576 || cfg.ny > 1048576 || cfg.nz > 1048576 ||
        uint64_t(cfg.nx) * cfg.ny * cfg.nz > 1048576 || !cfg.capacity || !cfg.changesPerFrame ||
        cfg.changesPerFrame > 32768 || !cfg.bytesPerCell || cfg.bytesPerCell > 1024 || cfg.bytesPerCell % 4)
        throw std::runtime_error("Invalid CUDA brick pool dimensions/budget/stride");
    auto p = std::make_unique<BrickPool>();
    auto &v = p->view;
    v.bx = (cfg.nx + 3) / 4;
    v.by = (cfg.ny + 3) / 4;
    v.bz = (cfg.nz + 3) / 4;
    v.bricks = v.bx * v.by * v.bz;
    // Thin domains can have more than ceil(cellCount/64) boundary bricks.
    if (v.bricks > 16384 || cfg.capacity > v.bricks)
        throw std::runtime_error("Invalid CUDA physical brick capacity");
    v.capacity = cfg.capacity;
    v.bytesPerCell = cfg.bytesPerCell;
    p->limit = cfg.changesPerFrame;
    auto allocate = [&](void **dst, size_t bytes, int fill) {
        check(cudaMalloc(dst, bytes), "CUDA persistent brick allocation");
        p->bytes += bytes;
        check(cudaMemset(*dst, fill, bytes), "CUDA brick initialization");
    };
    for (uint32_t i = 0; i < 2; ++i) {
        allocate(reinterpret_cast<void **>(&v.pages[i]), size_t(v.bricks) * 4, 0xff);
        allocate(reinterpret_cast<void **>(&v.keys[i]), size_t(v.capacity) * 4, 0xff);
        allocate(reinterpret_cast<void **>(&v.active[i]), size_t(v.bricks) * 4, 0);
        allocate(&v.fields[i], size_t(v.capacity) * 64 * v.bytesPerCell, 0);
    }
    allocate(reinterpret_cast<void **>(&v.requests), size_t(v.bricks) * 4, 0);
    allocate(reinterpret_cast<void **>(&v.control), BrickCounterCount * 4, 0);
    allocate(reinterpret_cast<void **>(&p->freeSlots), size_t(v.capacity) * 4, 0);
    // cudaMemset may return before its default-stream work finishes. Runtime
    // users submit on nonblocking interop streams, which do not inherit that
    // ordering. Complete initialization once, never in ordinary frame work.
    check(cudaStreamSynchronize(nullptr), "CUDA brick initialization completion");
    return p.release();
}
void destroyBricks(BrickPool *p) noexcept {
    delete p;
}
BrickView brickView(const BrickPool *p) {
    if (!p)
        throw std::runtime_error("Missing CUDA brick pool");
    return p->view;
}
size_t brickBytes(const BrickPool *p) {
    return p ? p->bytes : 0;
}
void beginBrickFrame(BrickPool *p, void *stream) {
    if (!stream)
        throw std::runtime_error("Missing CUDA brick stream");
    beginFrame<<<1, 1, 0, static_cast<cudaStream_t>(stream)>>>(brickView(p), p->limit);
    check(cudaGetLastError(), "CUDA brick frame budget");
}
void clearBrickRequests(BrickPool *p, void *stream) {
    const auto v = brickView(p);
    if (!stream)
        throw std::runtime_error("Missing CUDA brick stream");
    clearRequests<<<(v.bricks + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(v);
    check(cudaGetLastError(), "CUDA clear brick requests");
}
void stageBricks(BrickPool *p, void *stream) {
    if (!stream)
        throw std::runtime_error("Missing CUDA brick stream");
    stage<<<1, 1, 0, static_cast<cudaStream_t>(stream)>>>(brickView(p), p->freeSlots);
    check(cudaGetLastError(), "CUDA stage bounded brick transaction");
    const auto v = brickView(p);
    stageActive<<<(v.bricks + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(v);
    check(cudaGetLastError(), "CUDA stage active brick mask");
}
void publishBricks(BrickPool *p, void *stream) {
    if (!stream)
        throw std::runtime_error("Missing CUDA brick stream");
    publish<<<1, 1, 0, static_cast<cudaStream_t>(stream)>>>(brickView(p));
    check(cudaGetLastError(), "CUDA publish brick transaction");
    const auto v = brickView(p);
    mirrorPages<<<(v.bricks + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(v);
    check(cudaGetLastError(), "CUDA mirror inactive page reservations");
}
} // namespace lab::cuda_fluid
