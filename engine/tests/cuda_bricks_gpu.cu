#include "../src/fluid/cuda/fluid_cuda_bricks.h"
#include <cuda_runtime.h>
#include <array>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
using namespace lab::cuda_fluid;
namespace {
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
void require(bool b, const char *why) {
    if (!b)
        throw std::runtime_error(why);
}
__global__ void fill(BrickView v, uint32_t stamp, bool invalidate) {
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (!v.control[BrickReady])
        return;
    if (invalidate) {
        if (!i)
            v.control[BrickInvalid] = 1;
        return;
    }
    if (i >= v.capacity * 64)
        return;
    const uint32_t back = v.control[BrickCandidate], key = v.keys[back][i / 64];
    if (key == 0xffffffffu || !v.active[back][key])
        return;
    auto out = static_cast<uint4 *>(v.fields[back]);
    out[i] = make_uint4(stamp, key, i % 64, 0xabcdef01u);
}
struct Fixture {
    BrickPool *pool = nullptr;
    cudaStream_t stream = nullptr;
    Fixture(uint32_t capacity = 4, uint32_t budget = 2) {
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        try {
            pool = createBricks({16, 16, 16, capacity, budget, 16});
        } catch (...) {
            cudaStreamDestroy(stream);
            throw;
        }
    }
    ~Fixture() {
        cudaStreamSynchronize(stream);
        destroyBricks(pool);
        cudaStreamDestroy(stream);
    }
    void request(std::initializer_list<uint32_t> keys) {
        auto v = brickView(pool);
        std::vector<uint32_t> flags(v.bricks);
        for (auto k : keys)
            flags.at(k) = 1;
        check(cudaMemcpyAsync(v.requests, flags.data(), flags.size() * 4, cudaMemcpyHostToDevice, stream));
        check(cudaStreamSynchronize(stream)); // fixture upload lifetime, not production scheduling
    }
    void advance(uint32_t stamp, bool fresh = true, bool invalid = false) {
        if (fresh)
            beginBrickFrame(pool, stream);
        stageBricks(pool, stream);
        auto v = brickView(pool);
        fill<<<(v.capacity * 64 + 127) / 128, 128, 0, stream>>>(v, stamp, invalid);
        check(cudaGetLastError());
        publishBricks(pool, stream);
        check(cudaStreamSynchronize(stream));
    }
    std::array<uint32_t, BrickCounterCount> counters() {
        std::array<uint32_t, BrickCounterCount> c{};
        check(cudaMemcpy(c.data(), brickView(pool).control, sizeof(c), cudaMemcpyDeviceToHost));
        return c;
    }
    std::vector<uint32_t> snapshot() {
        auto v = brickView(pool);
        const auto c = counters();
        uint32_t front = c[BrickFront];
        std::vector<uint32_t> words(v.bricks * 2 + v.capacity + v.capacity * 64 * 4);
        auto *p = words.data();
        check(cudaMemcpy(p, v.pages[front], v.bricks * 4, cudaMemcpyDeviceToHost));
        p += v.bricks;
        check(cudaMemcpy(p, v.active[front], v.bricks * 4, cudaMemcpyDeviceToHost));
        p += v.bricks;
        check(cudaMemcpy(p, v.keys[front], v.capacity * 4, cudaMemcpyDeviceToHost));
        p += v.capacity;
        check(cudaMemcpy(p, v.fields[front], v.capacity * 64 * 16, cudaMemcpyDeviceToHost));
        return words;
    }
    void audit(uint32_t stamp, std::initializer_list<uint32_t> wanted) {
        const auto v = brickView(pool);
        const auto c = counters();
        const auto data = snapshot();
        const auto *pages = data.data(), *active = pages + v.bricks, *keys = active + v.bricks;
        const auto *fields = keys + v.capacity;
        std::vector<bool> expected(v.bricks);
        for (auto k : wanted)
            expected[k] = true;
        for (uint32_t k = 0; k < v.bricks; ++k) {
            require(bool(active[k]) == expected[k], "Published active mask is not atomic");
            if (!expected[k])
                continue;
            require(pages[k] < v.capacity && keys[pages[k]] == k, "Invalid/inconsistent page ownership");
            for (uint32_t j = 0; j < 64; ++j) {
                const auto *f = fields + (pages[k] * 64 + j) * 4;
                require(f[0] == stamp && f[1] == k && f[2] == j && f[3] == 0xabcdef01u,
                        "Published uninitialized, stale or aliased field cell");
            }
        }
        require(!c[BrickInvalid], "Unexpected invalid pool");
        require(c[BrickFrameChanges] <= 2 && c[BrickPeakFrameChanges] <= 2, "Exceeded topology frame budget");
    }
};
void invalidConfig() {
    uint32_t rejected = 0;
    for (auto c : {BrickConfig{0, 16, 16, 4, 2, 16}, BrickConfig{16, 16, 16, 65, 2, 16},
                   BrickConfig{16, 16, 16, 4, 0, 16}, BrickConfig{16, 16, 16, 4, 2, 15},
                   BrickConfig{1048576, 1, 1, 4, 2, 16}}) {
        try {
            auto p = createBricks(c);
            destroyBricks(p);
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    require(rejected == 5, "Malformed/unbounded pool accepted");
    std::cout << "{\"case\":\"brick-invalid-config\",\"pass\":true}\n";
}
void transactions() {
    Fixture f;
    const auto initial = f.snapshot();
    f.request({0, 5, 17, 63});
    f.advance(1);
    require(f.counters()[BrickVersion] == 0 && f.counters()[BrickMissing] == 2,
            "Premature partial topology publication");
    require(f.snapshot() == initial, "Staging overwrote the live field");
    f.advance(1, false);
    require(f.snapshot() == initial && f.counters()[BrickFrameChanges] == 2,
            "Substeps evaded the per-frame change budget");
    f.advance(2);
    f.audit(2, {0, 5, 17, 63});
    require(f.counters()[BrickVersion] == 1, "Backlog did not converge");
    f.advance(3);
    f.audit(3, {0, 5, 17, 63});
    require(!f.counters()[BrickFrameChanges], "Stable topology reallocated on ping-pong");
    auto old = f.snapshot();
    auto version = f.counters()[BrickVersion];
    f.request({1, 6, 18, 62});
    for (uint32_t step = 0; step < 4; ++step) {
        f.advance(4 + step);
        if (step < 3)
            require(f.snapshot() == old && f.counters()[BrickVersion] == version,
                    "Budgeted recycle damaged the active field");
    }
    f.audit(7, {1, 6, 18, 62});
    std::cout << "{\"case\":\"brick-budgeted-recycle\",\"peakChanges\":"
              << f.counters()[BrickPeakFrameChanges] << ",\"bytes\":" << brickBytes(f.pool)
              << ",\"pass\":true}\n";
    old = f.snapshot();
    version = f.counters()[BrickVersion];
    f.request({0, 1, 2, 3, 4});
    for (uint32_t i = 0; i < 10; ++i)
        f.advance(10 + i);
    require(f.snapshot() == old && f.counters()[BrickVersion] == version && f.counters()[BrickOverflows],
            "Capacity overflow discarded published data");
    f.request({2, 3});
    for (uint32_t i = 0; i < 6; ++i)
        f.advance(20 + i);
    f.audit(25, {2, 3});
    std::cout << "{\"case\":\"brick-overflow-recovery\",\"pass\":true}\n";
    old = f.snapshot();
    version = f.counters()[BrickVersion];
    f.advance(26, true, true);
    require(f.snapshot() == old && f.counters()[BrickVersion] == version && f.counters()[BrickInvalid],
            "Invalid candidate was published");
    std::cout << "{\"case\":\"brick-field-rejection\",\"pass\":true}\n";
}
void captured() {
    Fixture f;
    f.request({0, 7});
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t executable = nullptr;
    check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
    beginBrickFrame(f.pool, f.stream);
    stageBricks(f.pool, f.stream);
    fill<<<2, 128, 0, f.stream>>>(brickView(f.pool), 77, false);
    publishBricks(f.pool, f.stream);
    check(cudaStreamEndCapture(f.stream, &graph));
    check(cudaGraphInstantiate(&executable, graph, 0));
    for (uint32_t i = 0; i < 8; ++i)
        check(cudaGraphLaunch(executable, f.stream));
    check(cudaStreamSynchronize(f.stream));
    f.audit(77, {0, 7});
    require(f.counters()[BrickVersion] == 8 && f.counters()[BrickAllocations] == 2,
            "Graph replay reallocates stable topology");
    check(cudaGraphExecDestroy(executable));
    check(cudaGraphDestroy(graph));
    std::cout << "{\"case\":\"brick-graph-replay\",\"pass\":true}\n";
}
} // namespace
int main() try {
    invalidConfig();
    transactions();
    captured();
    std::cout
        << "PASS CUDA brick pool: atomic fields/pages, bounded churn, overflow recovery and graph replay\n";
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA brick pool: " << e.what() << '\n';
    return 1;
}
