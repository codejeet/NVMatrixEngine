#include "fluid_cuda_pressure.h"
#include "fluid_cuda_pressure_loop.cuh"
#include "fluid_cuda_mac_device.cuh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
using namespace mac_detail;
struct alignas(16) ReductionVector {
    double x, y, z, w;
};
__device__ ReductionVector makeReduction(double x, double y, double z, double w) {
    return {x, y, z, w};
}
struct HierarchyRow {
    float weight[6], diagonal, boundary;
};
static_assert(sizeof(HierarchyRow) == 32);
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
__host__ __device__ uint32_t product(PressureLevel g) {
    return g.x * g.y * g.z;
}
__device__ int3 coord(uint32_t i, PressureLevel g) {
    return make_int3(i % g.x, i / g.x % g.y, i / (g.x * g.y));
}
__device__ uint32_t index(int3 p, PressureLevel g) {
    return (p.z * g.y + p.y) * g.x + p.x;
}
__device__ bool inside(int3 p, PressureLevel g) {
    return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < int(g.x) && p.y < int(g.y) && p.z < int(g.z);
}
__device__ bool running(PressureView v, MacView m) {
    return !m.pool.control[BrickInvalid] && v.counts[PressureRunning] && !v.counts[PressureInvalid];
}
__device__ HierarchyRow &row(PressureView v, uint32_t i) {
    return static_cast<HierarchyRow *>(v.rows)[i];
}
__device__ float4 &vector(PressureView v, uint32_t i) {
    return static_cast<float4 *>(v.vectors)[i];
}
__device__ ReductionVector reduceGroup(ReductionVector value, uint32_t lane, ReductionVector *scratch) {
    scratch[lane] = value;
    __syncthreads();
    for (uint32_t n = 64; n; n >>= 1) {
        if (lane < n) {
            auto a = scratch[lane], b = scratch[lane + n];
            scratch[lane] = makeReduction(a.x + b.x, a.y + b.y, a.z + b.z, fmax(a.w, b.w));
        }
        __syncthreads();
    }
    return scratch[0];
}
__device__ float coarseApply(PressureView v, uint32_t level, uint32_t id, uint32_t pi) {
    auto g = v.levels[level];
    auto p = coord(id, g);
    const auto &r = row(v, g.offset + id);
    float value = r.diagonal * v.correction[pi][g.offset + id];
    for (int a = 0; a < 3; ++a)
        for (uint32_t s = 0; s < 2; ++s) {
            int3 q = p;
            component(q, a) += s ? 1 : -1;
            if (inside(q, g))
                value -= r.weight[a * 2 + s] * v.correction[pi][g.offset + index(q, g)];
        }
    return value;
}
__device__ float baseApply(const Row &r, PressureView v, uint32_t id, uint32_t pi) {
    float value = r.diagonal * v.fine[pi][id];
    for (uint32_t i = 0; i < r.count; ++i)
        value += r.coefficient[i] * v.fine[pi][r.neighbor[i]];
    return value;
}
struct DirectionSample {
    PressureView v;
    __device__ double operator()(uint32_t i) const {
        return vector(v, i).z;
    }
};
struct PressureSample {
    Frame f;
    MacView m;
    __device__ double operator()(uint32_t i) const {
        return precisePressure(i, f, m);
    }
};
__global__ void clear(PressureView v, MacView m) {
    if (blockIdx.x || threadIdx.x || m.pool.control[BrickInvalid])
        return;
    for (uint32_t i = 0; i <= PressureCapped; ++i)
        v.counts[i] = 0;
    for (uint32_t i = 0; i < 8; ++i)
        v.scalars[i] = 0;
    for (uint32_t i = 0; i < 64; ++i)
        v.trace[i] = 0;
    if (!m.pool.control[BrickInvalid]) {
        v.counts[PressureRunning] = 1;
        v.counts[PressureCacheDirty] = !v.counts[PressureCacheValid];
        v.counts[PressureUseWarmStart] = 0;
        ++v.counts[PressureCalls];
    }
}
__global__ void checkTopology(Frame f, Fields d, MacView m, PressureView v) {
    if (m.pool.control[BrickInvalid])
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    const int3 p = cellFromIndex(id, f);
    // Cell classification, solid boundary and chosen pressure ownership fully
    // determine this operator. Velocity/curvature change its RHS, not its matrix.
    const uint32_t tag = (liquid(p, f, d) ? 1u : 0u) | (solid(p, f, d) ? 2u : 0u) |
                         (solveWidth(p, f, m) == 2 ? 4u : 0u) | (m.pool.control[BrickReady] ? 8u : 0u);
    if (v.topology[id] != tag)
        atomicExch(v.counts + PressureCacheDirty, 1u);
    v.topology[id] = tag;
}
__global__ void selectHistory(Frame f, MacView m, PressureView v) {
    if (threadIdx.x || blockIdx.x || m.pool.control[BrickInvalid])
        return;
    const bool reuse = !v.counts[PressureCacheDirty];
    ++v.counts[reuse ? PressureHierarchyReuses : PressureHierarchyBuilds];
    v.counts[PressureUseWarmStart] = reuse && v.previousParameters[0] == f.minimumCell.w &&
                                     v.previousParameters[1] == f.gravityDt.w &&
                                     v.previousParameters[2] == f.solver.x;
}
__global__ void resetHistory(PressureView v, MacView m) {
    if (!threadIdx.x && !blockIdx.x && !m.pool.control[BrickInvalid])
        v.counts[PressureCacheValid] = 0;
}
__global__ void saveHistory(Frame f, Fields d, MacView m, PressureView v) {
    if (m.pool.control[BrickInvalid] || !v.counts[PressureConverged])
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < f.grid.w)
        v.previousPressure[id] = solveActive(id, f, d, m) ? precisePressure(id, f, m) : 0.;
    if (!id) {
        v.previousParameters[0] = f.minimumCell.w;
        v.previousParameters[1] = f.gravityDt.w;
        v.previousParameters[2] = f.solver.x;
        v.counts[PressureCacheValid] = 1;
    }
}
// Two 4^3 aggregates per block, one thread per child. In particular, do not
// serialize 64 sparse-row gathers on each parent thread. The parent reduction
// remains in geometric child order; atomic leaf-list ordering cannot affect it.
__global__ void assembleBase(Frame f, Fields d, MacView m, PressureView v) {
    if (m.pool.control[BrickInvalid] || !v.counts[PressureCacheDirty])
        return;
    __shared__ HierarchyRow children[128];
    const auto g = v.levels[0];
    const uint32_t lane = threadIdx.x & 63, group = threadIdx.x / 64;
    const uint32_t id = blockIdx.x * 2 + group;
    HierarchyRow out{};
    if (id < product(g)) {
        const int3 p = coord(id, g);
        const int3 q = p * 4 + make_int3(lane & 3, (lane >> 2) & 3, lane >> 4);
        if (liquid(q, f, d)) {
            const uint32_t child = cellIndex(q, f);
            if (solveLeaf(q, f, m) == child) {
                const auto r = solveRow(child, f, d, m);
                out.boundary = r.boundaryDiagonal;
                for (uint32_t j = 0; j < r.count; ++j) {
                    const int3 n = cellFromIndex(r.neighbor[j], f);
                    int3 delta = make_int3(n.x / 4, n.y / 4, n.z / 4) - p;
                    if (!delta.x && !delta.y && !delta.z)
                        continue;
                    if (abs(delta.x) + abs(delta.y) + abs(delta.z) != 1) {
                        atomicExch(v.counts + PressureInvalid, 1u);
                        continue;
                    }
                    const int a = delta.x ? 0 : delta.y ? 1 : 2;
                    out.weight[a * 2 + (component(delta, a) > 0)] -= r.coefficient[j];
                }
            }
        }
    }
    children[threadIdx.x] = out;
    __syncthreads();
    if (!lane && id < product(g)) {
        HierarchyRow sum{};
        for (uint32_t child = 0; child < 64; ++child) {
            const auto &r = children[group * 64 + child];
            sum.boundary += r.boundary;
            for (uint32_t side = 0; side < 6; ++side)
                sum.weight[side] += r.weight[side];
        }
        row(v, g.offset + id) = sum;
        v.rhs[g.offset + id] = v.correction[0][g.offset + id] = v.correction[1][g.offset + id] = 0;
    }
}
__global__ void assemble(Frame f, Fields d, MacView m, PressureView v, uint32_t level) {
    if (m.pool.control[BrickInvalid] || !v.counts[PressureCacheDirty])
        return;
    auto g = v.levels[level];
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= product(g))
        return;
    const int extent = 2;
    int3 p = coord(id, g), base = p * extent;
    HierarchyRow out{};
    for (int z = 0; z < extent; ++z)
        for (int y = 0; y < extent; ++y)
            for (int x = 0; x < extent; ++x) {
                int3 q = base + make_int3(x, y, z);
                auto childGrid = v.levels[level - 1];
                if (!inside(q, childGrid))
                    continue;
                const auto &r = row(v, childGrid.offset + index(q, childGrid));
                out.boundary += r.boundary;
                for (int a = 0; a < 3; ++a)
                    for (uint32_t s = 0; s < 2; ++s) {
                        int3 n = q;
                        component(n, a) += s ? 1 : -1;
                        if (inside(n, childGrid) && component(n, a) / 2 != component(p, a))
                            out.weight[a * 2 + s] += r.weight[a * 2 + s];
                    }
            }
    row(v, g.offset + id) = out;
    v.rhs[g.offset + id] = v.correction[0][g.offset + id] = v.correction[1][g.offset + id] = 0;
}
__global__ void canonical(MacView m, PressureView v, uint32_t level) {
    if (m.pool.control[BrickInvalid] || !v.counts[PressureCacheDirty])
        return;
    auto g = v.levels[level];
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= product(g))
        return;
    int3 p = coord(id, g);
    auto &r = row(v, g.offset + id);
    float diag = r.boundary;
    for (int a = 0; a < 3; ++a) {
        int3 q = p;
        component(q, a)--;
        const float negative = inside(q, g) ? row(v, g.offset + index(q, g)).weight[a * 2 + 1] : 0;
        r.weight[a * 2] = negative;
        diag += negative + r.weight[a * 2 + 1];
        if (negative < 0 || r.weight[a * 2 + 1] < 0)
            atomicExch(v.counts + PressureInvalid, 1u);
    }
    r.diagonal = diag;
    if (!isfinite(diag) || diag < 0 || r.boundary < 0)
        atomicExch(v.counts + PressureInvalid, 1u);
}
__global__ void factor(MacView m, PressureView v) {
    if (!running(v, m) || !v.counts[PressureCacheDirty])
        return;
    __shared__ float dense[4096];
    __shared__ ReductionVector scratch[128];
    uint32_t lane = threadIdx.x;
    auto g = v.levels[v.levelCount - 1];
    uint32_t n = product(g);
    auto maximum =
        reduceGroup(makeReduction(0, 0, 0, lane < n ? row(v, g.offset + lane).diagonal : 0), lane, scratch);
    const float shift = float(fmax(1., maximum.w)) * 1e-4f;
    for (uint32_t t = lane; t < 4096; t += 128) {
        uint32_t i = t / 64, j = t % 64;
        float a = 0;
        if (i < n && j < n) {
            const auto &r = row(v, g.offset + i);
            if (i == j)
                a = r.diagonal + shift;
            else {
                int3 delta = coord(j, g) - coord(i, g);
                if (abs(delta.x) + abs(delta.y) + abs(delta.z) == 1) {
                    int axis = delta.x ? 0 : delta.y ? 1 : 2;
                    a = -r.weight[axis * 2 + (component(delta, axis) > 0)];
                }
            }
        }
        dense[t] = a;
    }
    __syncthreads();
    for (uint32_t k = 0; k < n; ++k) {
        if (lane == k) {
            float a = dense[k * 64 + k];
            for (uint32_t j = 0; j < k; ++j)
                a -= dense[k * 64 + j] * dense[k * 64 + j];
            if (!(a > 0) || !isfinite(a)) {
                atomicExch(v.counts + PressureInvalid, 1u);
                a = 1;
            }
            dense[k * 64 + k] = sqrtf(a);
        }
        __syncthreads();
        if (lane > k && lane < n) {
            float a = dense[lane * 64 + k];
            for (uint32_t j = 0; j < k; ++j)
                a -= dense[lane * 64 + j] * dense[k * 64 + j];
            dense[lane * 64 + k] = a / dense[k * 64 + k];
        }
        __syncthreads();
    }
    for (uint32_t t = lane; t < 4096; t += 128)
        v.factor[t] = dense[t];
}
__global__ void initialize(Frame f, Fields d, MacView m, PressureView v) {
    if (!running(v, m))
        return;
    __shared__ ReductionVector scratch[128];
    uint32_t id = blockIdx.x * 128 + threadIdx.x;
    ReductionVector value = makeReduction(0, 0, 0, 0);
    if (solveActive(id, f, d, m)) {
        precisePressure(id, f, m) = v.counts[PressureUseWarmStart] ? v.previousPressure[id] : 0.;
        const double rhs = preciseRhs(id, f, m);
        const uint32_t w = solveWidth(cellFromIndex(id, f), f, m);
        vector(v, id) = make_float4(float(rhs), 0, 0, 0);
        const double divergence = fabs(rhs) * f.gravityDt.w /
                                  (double(f.solver.x) * f.minimumCell.w * f.minimumCell.w * (w * w * w));
        value = makeReduction(0, 0, rhs * rhs, divergence);
    }
    auto sum = reduceGroup(value, threadIdx.x, scratch);
    if (!threadIdx.x)
        static_cast<ReductionVector *>(v.partials)[blockIdx.x] = sum;
}
__global__ void reduce(Frame f, MacView m, PressureView v, PressureConfig cfg, uint32_t mode,
                       uint32_t iteration) {
    if (!running(v, m))
        return;
    __shared__ ReductionVector scratch[128];
    ReductionVector sum = makeReduction(0, 0, 0, 0);
    for (uint32_t i = threadIdx.x; i < (f.grid.w + 127) / 128; i += 128) {
        auto a = static_cast<ReductionVector *>(v.partials)[i];
        sum = makeReduction(sum.x + a.x, sum.y + a.y, sum.z + a.z, fmax(sum.w, a.w));
    }
    sum = reduceGroup(sum, threadIdx.x, scratch);
    if (threadIdx.x)
        return;
    if (!isfinite(sum.x) || !isfinite(sum.y) || !isfinite(sum.z) || !isfinite(sum.w)) {
        v.counts[PressureInvalid] = 1;
        return;
    }
    // A captured WHILE body has one static reduction node per phase. Its
    // iteration index lives on the GPU; direct/unrolled calls retain literals.
    if (iteration == 0xffffffffu)
        iteration = v.counts[PressureIterations];
    if (mode == 0 || mode == 3 || mode == 4) {
        if (!mode) {
            v.scalars[0] = sum.z;
            v.scalars[3] = sum.w;
            // The relative convergence reference is always ||b||, not the
            // smaller (or larger) warm residual. Never weaken the solve gate.
            if (v.counts[PressureUseWarmStart])
                return;
        } else if (mode == 3) {
            ++v.counts[PressureIterations];
            v.trace[iteration * 2] = sum.z;
            v.trace[iteration * 2 + 1] = sum.w;
        } else {
            if (!v.counts[PressureUseWarmStart])
                return;
            // Reject history that worsens either physical residual criterion.
            // The following kernel restores the zero guess before iteration.
            if (sum.z > v.scalars[0] || sum.w > v.scalars[3]) {
                v.counts[PressureUseWarmStart] = 0;
                sum.z = v.scalars[0];
                sum.w = v.scalars[3];
            } else
                ++v.counts[PressureWarmStarts];
        }
        v.scalars[1] = sum.z;
        v.scalars[2] = sum.w;
        if (sum.z <= v.scalars[0] * double(cfg.relativeTolerance) * cfg.relativeTolerance &&
            // Leave headroom for FP32 face storage and compatible prolongation.
            // The final published velocities have a separate physical-flux gate.
            sum.w <= .5 * cfg.divergenceTolerance) {
            v.counts[PressureRunning] = 0;
            v.counts[PressureConverged] = 1;
        }
    } else if (mode == 1) {
        if (!(sum.x > 0)) {
            v.counts[PressureInvalid] = 1;
            return;
        }
        v.scalars[5] = iteration ? sum.x / v.scalars[4] : 0;
        v.scalars[4] = sum.x;
    } else {
        if (!(sum.y > 0)) {
            v.counts[PressureInvalid] = 1;
            return;
        }
        v.scalars[6] = v.scalars[4] / sum.y;
    }
}
__global__ void resetFine(Frame f, MacView m, PressureView v) {
    if (!running(v, m))
        return;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < solveCount(f, m);
         i += blockDim.x * gridDim.x) {
        uint32_t id = solveId(i, m);
        v.fine[0][id] = v.fine[1][id] = 0;
    }
}
__global__ void fineSmooth(Frame f, Fields d, MacView m, PressureView v, uint32_t pi) {
    if (!running(v, m))
        return;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < solveCount(f, m);
         i += blockDim.x * gridDim.x) {
        uint32_t id = solveId(i, m);
        if (!solveActive(id, f, d, m))
            continue;
        const auto r = solveRow(id, f, d, m);
        // Same symmetric pre/post smoother as the existing DX12 MGPCG path.
        v.fine[1 - pi][id] = v.fine[pi][id] + .8f * r.step * (vector(v, id).x - baseApply(r, v, id, pi));
    }
}
__global__ void restrictBase(Frame f, Fields d, MacView m, PressureView v) {
    if (!running(v, m))
        return;
    __shared__ float children[128];
    const auto g = v.levels[0];
    const uint32_t lane = threadIdx.x & 63, group = threadIdx.x / 64;
    const uint32_t id = blockIdx.x * 2 + group;
    float value = 0;
    if (id < product(g)) {
        const int3 q = coord(id, g) * 4 + make_int3(lane & 3, (lane >> 2) & 3, lane >> 4);
        if (liquid(q, f, d)) {
            const uint32_t child = cellIndex(q, f);
            if (solveLeaf(q, f, m) == child)
                value = vector(v, child).x - baseApply(solveRow(child, f, d, m), v, child, 0);
        }
    }
    children[threadIdx.x] = value;
    __syncthreads();
    if (!lane && id < product(g)) {
        float rhs = 0;
        for (uint32_t child = 0; child < 64; ++child)
            rhs += children[group * 64 + child];
        v.rhs[g.offset + id] = rhs;
        v.correction[0][g.offset + id] = v.correction[1][g.offset + id] = 0;
    }
}
__global__ void coarseSmooth(MacView m, PressureView v, uint32_t level, uint32_t pi) {
    if (!running(v, m))
        return;
    auto g = v.levels[level];
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= product(g))
        return;
    uint32_t at = g.offset + id;
    float diag = row(v, at).diagonal;
    v.correction[1 - pi][at] =
        diag > 0 ? v.correction[pi][at] + (2.f / 3) * (v.rhs[at] - coarseApply(v, level, id, pi)) / diag : 0;
}
__global__ void restrictCoarse(MacView m, PressureView v, uint32_t level) {
    if (!running(v, m))
        return;
    auto g = v.levels[level], childGrid = v.levels[level - 1];
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= product(g))
        return;
    int3 base = coord(id, g) * 2;
    float rhs = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        int3 q = base + make_int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        if (inside(q, childGrid)) {
            uint32_t child = index(q, childGrid);
            rhs += v.rhs[childGrid.offset + child] - coarseApply(v, level - 1, child, 0);
        }
    }
    v.rhs[g.offset + id] = rhs;
    v.correction[0][g.offset + id] = v.correction[1][g.offset + id] = 0;
}
__global__ void bottom(MacView m, PressureView v) {
    if (!running(v, m))
        return;
    __shared__ float b[64];
    uint32_t lane = threadIdx.x;
    auto g = v.levels[v.levelCount - 1];
    uint32_t n = product(g);
    if (lane < n)
        b[lane] = v.rhs[g.offset + lane];
    __syncthreads();
    for (uint32_t i = 0; i < n; ++i) {
        if (lane == i)
            b[i] /= v.factor[i * 64 + i];
        __syncthreads();
        if (lane > i && lane < n)
            b[lane] -= v.factor[lane * 64 + i] * b[i];
        __syncthreads();
    }
    for (int i = int(n) - 1; i >= 0; --i) {
        if (lane == uint32_t(i))
            b[i] /= v.factor[i * 64 + i];
        __syncthreads();
        if (lane < uint32_t(i))
            b[lane] -= v.factor[i * 64 + lane] * b[i];
        __syncthreads();
    }
    if (lane < n)
        v.correction[0][g.offset + lane] = b[lane];
}
__global__ void prolongCoarse(MacView m, PressureView v, uint32_t level) {
    if (!running(v, m))
        return;
    auto g = v.levels[level], parent = v.levels[level + 1];
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= product(g))
        return;
    int3 p = coord(id, g);
    v.correction[0][g.offset + id] += v.correction[0][parent.offset + index(half(p), parent)];
}
__global__ void prolongFine(Frame f, Fields d, MacView m, PressureView v) {
    if (!running(v, m))
        return;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < solveCount(f, m);
         i += blockDim.x * gridDim.x) {
        uint32_t id = solveId(i, m);
        if (!solveActive(id, f, d, m))
            continue;
        int3 p = cellFromIndex(id, f);
        v.fine[0][id] +=
            v.correction[0][v.levels[0].offset + index(make_int3(p.x / 4, p.y / 4, p.z / 4), v.levels[0])];
    }
}
// Fixed geometric reduction order; active-list atomic append order never
// determines the floating-point dot-product tree.
__global__ void dot(Frame f, Fields d, MacView m, PressureView v, uint32_t mode) {
    if (!running(v, m) || (mode == 4 && !v.counts[PressureUseWarmStart]))
        return;
    __shared__ ReductionVector scratch[128];
    uint32_t id = blockIdx.x * 128 + threadIdx.x;
    ReductionVector out = makeReduction(0, 0, 0, 0);
    if (solveActive(id, f, d, m)) {
        auto &q = vector(v, id);
        if (mode == 1) {
            q.y = v.fine[0][id];
            out.x = double(q.x) * q.y;
        } else if (mode == 2) {
            double ap = physicalApply(id, f, d, m, DirectionSample{v});
            q.w = float(ap);
            out.y = double(q.z) * ap;
        } else {
            double r = preciseRhs(id, f, m) - physicalApply(id, f, d, m, PressureSample{f, m});
            const uint32_t w = solveWidth(cellFromIndex(id, f), f, m);
            q.x = float(r);
            out.z = r * r;
            out.w = fabs(r) * f.gravityDt.w /
                    (double(f.solver.x) * f.minimumCell.w * f.minimumCell.w * (w * w * w));
        }
    }
    auto sum = reduceGroup(out, threadIdx.x, scratch);
    if (!threadIdx.x)
        static_cast<ReductionVector *>(v.partials)[blockIdx.x] = sum;
}
__global__ void discardWarmStart(Frame f, Fields d, MacView m, PressureView v) {
    if (m.pool.control[BrickInvalid] || v.counts[PressureInvalid] || v.counts[PressureUseWarmStart])
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (solveActive(id, f, d, m)) {
        precisePressure(id, f, m) = 0.;
        vector(v, id) = make_float4(float(preciseRhs(id, f, m)), 0, 0, 0);
    }
}
__global__ void direction(Frame f, Fields d, MacView m, PressureView v) {
    if (!running(v, m))
        return;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < solveCount(f, m);
         i += blockDim.x * gridDim.x) {
        uint32_t id = solveId(i, m);
        if (!solveActive(id, f, d, m))
            continue;
        auto &q = vector(v, id);
        q.z = float(double(q.y) + v.scalars[5] * q.z);
    }
}
__global__ void update(Frame f, Fields d, MacView m, PressureView v) {
    if (!running(v, m))
        return;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < solveCount(f, m);
         i += blockDim.x * gridDim.x) {
        uint32_t id = solveId(i, m);
        if (solveActive(id, f, d, m))
            precisePressure(id, f, m) += v.scalars[6] * vector(v, id).z;
    }
}
__global__ void finish(MacView m, PressureView v) {
    if (blockIdx.x || threadIdx.x || m.pool.control[BrickInvalid])
        return;
    v.counts[PressureTotalIterations] += v.counts[PressureIterations];
    v.counts[PressurePeakIterations] = max(v.counts[PressurePeakIterations], v.counts[PressureIterations]);
    if (!v.counts[PressureConverged] && !v.counts[PressureInvalid]) {
        v.counts[PressureCapped] = 1;
        ++v.counts[PressureTotalCapped];
    }
    // Reject incomplete solves distinctly from convergence. No approximate
    // candidate is published and no overwritten scratch is used as fallback.
    if (!v.counts[PressureConverged])
        m.pool.control[BrickInvalid] = 1;
    v.counts[PressureRunning] = 0;
}
} // namespace
struct Pressure {
    PressureConfig config;
    PressureView view{};
    size_t bytes = 0;
    cudaStream_t bodyCapture = nullptr;
    uint64_t capturedBodyNodes = 0;
    ~Pressure() {
        if (bodyCapture)
            cudaStreamDestroy(bodyCapture);
        for (void *p :
             {view.rows, view.vectors, view.partials, static_cast<void *>(view.rhs),
              static_cast<void *>(view.correction[0]), static_cast<void *>(view.correction[1]),
              static_cast<void *>(view.fine[0]), static_cast<void *>(view.fine[1]),
              static_cast<void *>(view.factor), static_cast<void *>(view.counts),
              static_cast<void *>(view.scalars), static_cast<void *>(view.trace),
              static_cast<void *>(view.fallback), static_cast<void *>(view.topology),
              static_cast<void *>(view.previousPressure), static_cast<void *>(view.previousParameters)})
            if (p)
                cudaFree(p);
    }
};
Pressure *createPressure(const PressureConfig &c) {
    if (!c.nx || !c.ny || !c.nz || c.nx > 1048576 || c.ny > 1048576 || c.nz > 1048576 ||
        uint64_t(c.nx) * c.ny * c.nz > 1048576 || !c.maxIterations || c.maxIterations > 32 ||
        !std::isfinite(c.relativeTolerance) || c.relativeTolerance <= 0 || c.relativeTolerance >= 1 ||
        !std::isfinite(c.divergenceTolerance) || c.divergenceTolerance <= 0)
        throw std::runtime_error("Invalid CUDA multigrid configuration");
    auto p = std::make_unique<Pressure>();
    p->config = c;
    if (c.conditionalGraphs)
        check(cudaStreamCreateWithFlags(&p->bodyCapture, cudaStreamNonBlocking),
              "CUDA pressure body capture stream");
    auto &v = p->view;
    PressureLevel g{(c.nx + 3) / 4, (c.ny + 3) / 4, (c.nz + 3) / 4, 0};
    for (;;) {
        if (v.levelCount == 16)
            throw std::runtime_error("CUDA pressure hierarchy too deep");
        g.offset = v.capacity;
        v.levels[v.levelCount++] = g;
        v.capacity += product(g);
        if (product(g) <= 64)
            break;
        g = {(g.x + 1) / 2, (g.y + 1) / 2, (g.z + 1) / 2, 0};
    }
    auto allocate = [&](auto &ptr, size_t bytes) {
        check(cudaMalloc(reinterpret_cast<void **>(&ptr), bytes), "CUDA persistent pressure allocation");
        p->bytes += bytes;
        check(cudaMemset(ptr, 0, bytes), "CUDA pressure initialization");
    };
    const size_t cells = size_t(c.nx) * c.ny * c.nz;
    allocate(v.rows, size_t(v.capacity) * sizeof(HierarchyRow));
    allocate(v.vectors, cells * sizeof(float4));
    allocate(v.partials, (cells + 127) / 128 * sizeof(ReductionVector));
    allocate(v.rhs, size_t(v.capacity) * 4);
    for (auto &b : v.correction)
        allocate(b, size_t(v.capacity) * 4);
    for (auto &b : v.fine)
        allocate(b, cells * 4);
    allocate(v.factor, 4096 * 4);
    allocate(v.counts, PressureCounterCount * 4);
    allocate(v.scalars, 8 * 8);
    allocate(v.trace, 64 * 8);
    allocate(v.fallback, cells * 2 * sizeof(double));
    allocate(v.topology, cells * sizeof(uint32_t));
    allocate(v.previousPressure, cells * sizeof(double));
    allocate(v.previousParameters, 3 * sizeof(float));
    check(cudaStreamSynchronize(nullptr), "CUDA pressure initialization completion");
    return p.release();
}
void destroyPressure(Pressure *p) noexcept {
    delete p;
}
PressureView pressureView(const Pressure *p) {
    return p ? p->view : PressureView{};
}
size_t pressureBytes(const Pressure *p) {
    return p ? p->bytes : 0;
}
uint64_t pressureCapturedBodyNodes(const Pressure *p) {
    return p ? p->capturedBodyNodes : 0;
}
void resetPressureHistory(Pressure *p, void *rawStream, MacView m) {
    if (p)
        resetHistory<<<1, 1, 0, static_cast<cudaStream_t>(rawStream)>>>(p->view, m);
}
void preparePressure(Pressure *p, void *rawStream, void *const (&buffers)[BufferCount], const void *rawFrame,
                     MacView m) {
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto stream = static_cast<cudaStream_t>(rawStream);
    clear<<<1, 1, 0, stream>>>(p->view, m);
    checkTopology<<<(f.grid.w + 127) / 128, 128, 0, stream>>>(f, fields(buffers), m, p->view);
    selectHistory<<<1, 1, 0, stream>>>(f, m, p->view);
}
void publishPressureHistory(Pressure *p, void *rawStream, void *const (&buffers)[BufferCount],
                            const void *rawFrame, MacView m) {
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    saveHistory<<<(f.grid.w + 127) / 128, 128, 0, static_cast<cudaStream_t>(rawStream)>>>(f, fields(buffers),
                                                                                          m, p->view);
}
void enqueuePressure(Pressure *p, void *rawStream, void *const (&buffers)[BufferCount], const void *rawFrame,
                     MacView m) {
    if (!p || !rawStream || !rawFrame)
        throw std::runtime_error("Invalid CUDA multigrid invocation");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    if (f.grid.x != p->config.nx || f.grid.y != p->config.ny || f.grid.z != p->config.nz ||
        f.grid.w != uint64_t(p->config.nx) * p->config.ny * p->config.nz)
        throw std::runtime_error("CUDA pressure dimensions changed");
    if (!m.precise || m.fineSolution != p->view.fallback || m.pool.bytesPerCell != sizeof(Cell) ||
        !m.pool.control || !m.counters || !buffers[Cells] || !buffers[Solid] ||
        !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0 || !std::isfinite(f.gravityDt.w) ||
        f.gravityDt.w <= 0 || !std::isfinite(f.solver.x) || f.solver.x <= 0)
        throw std::runtime_error("Invalid CUDA pressure fields/physical parameters");
    auto stream = static_cast<cudaStream_t>(rawStream);
    auto v = p->view;
    const Fields d = fields(buffers);
    const uint32_t base = (f.grid.w + 127) / 128, workers = std::min(base, 512u);
    for (uint32_t l = 0; l < v.levelCount; ++l) {
        uint32_t groups = (product(v.levels[l]) + 127) / 128;
        if (!l)
            assembleBase<<<(product(v.levels[0]) + 1) / 2, 128, 0, stream>>>(f, d, m, v);
        else
            assemble<<<groups, 128, 0, stream>>>(f, d, m, v, l);
        canonical<<<groups, 128, 0, stream>>>(m, v, l);
    }
    factor<<<1, 128, 0, stream>>>(m, v);
    initialize<<<base, 128, 0, stream>>>(f, d, m, v);
    reduce<<<1, 128, 0, stream>>>(f, m, v, p->config, 0, 0);
    dot<<<base, 128, 0, stream>>>(f, d, m, v, 4);
    reduce<<<1, 128, 0, stream>>>(f, m, v, p->config, 4, 0);
    discardWarmStart<<<base, 128, 0, stream>>>(f, d, m, v);
    auto iterationBody = [&](cudaStream_t stream, uint32_t iteration) {
        resetFine<<<workers, 128, 0, stream>>>(f, m, v);
        for (uint32_t sweep = 0; sweep < 2; ++sweep)
            fineSmooth<<<workers, 128, 0, stream>>>(f, d, m, v, sweep);
        restrictBase<<<(product(v.levels[0]) + 1) / 2, 128, 0, stream>>>(f, d, m, v);
        for (uint32_t l = 0; l + 1 < v.levelCount; ++l) {
            for (uint32_t sweep = 0; sweep < 2; ++sweep)
                coarseSmooth<<<(product(v.levels[l]) + 127) / 128, 128, 0, stream>>>(m, v, l, sweep);
            restrictCoarse<<<(product(v.levels[l + 1]) + 127) / 128, 128, 0, stream>>>(m, v, l + 1);
        }
        bottom<<<1, 128, 0, stream>>>(m, v);
        for (int l = int(v.levelCount) - 2; l >= 0; --l) {
            prolongCoarse<<<(product(v.levels[l]) + 127) / 128, 128, 0, stream>>>(m, v, l);
            for (uint32_t sweep = 0; sweep < 2; ++sweep)
                coarseSmooth<<<(product(v.levels[l]) + 127) / 128, 128, 0, stream>>>(m, v, l, sweep);
        }
        prolongFine<<<workers, 128, 0, stream>>>(f, d, m, v);
        for (uint32_t sweep = 0; sweep < 2; ++sweep)
            fineSmooth<<<workers, 128, 0, stream>>>(f, d, m, v, sweep);
        dot<<<base, 128, 0, stream>>>(f, d, m, v, 1);
        reduce<<<1, 128, 0, stream>>>(f, m, v, p->config, 1, iteration);
        direction<<<workers, 128, 0, stream>>>(f, d, m, v);
        dot<<<base, 128, 0, stream>>>(f, d, m, v, 2);
        reduce<<<1, 128, 0, stream>>>(f, m, v, p->config, 2, iteration);
        update<<<workers, 128, 0, stream>>>(f, d, m, v);
        dot<<<base, 128, 0, stream>>>(f, d, m, v, 3);
        reduce<<<1, 128, 0, stream>>>(f, m, v, p->config, 3, iteration);
    };
    cudaStreamCaptureStatus status = cudaStreamCaptureStatusNone;
    cudaGraph_t parent = nullptr;
    if (p->config.conditionalGraphs)
        check(cudaStreamGetCaptureInfo(stream, &status, nullptr, &parent), "Query CUDA pressure capture");
    if (status == cudaStreamCaptureStatusActive) {
        cudaGraphConditionalHandle handle{};
        check(cudaGraphConditionalHandleCreate(&handle, parent, 0, cudaGraphCondAssignDefault),
              "CUDA pressure conditional handle (select unrolled pressure loop if unsupported)");
        enqueuePressureLoopCondition(stream, m, v, p->config, handle, false);
        const cudaGraphNode_t *dependencies = nullptr;
        const cudaGraphEdgeData *edges = nullptr;
        size_t dependencyCount = 0;
        check(cudaStreamGetCaptureInfo(stream, &status, nullptr, &parent, &dependencies, &edges,
                                       &dependencyCount),
              "CUDA pressure loop dependencies");
        cudaGraphNodeParams params{};
        params.type = cudaGraphNodeTypeConditional;
        params.conditional.handle = handle;
        params.conditional.type = cudaGraphCondTypeWhile;
        params.conditional.size = 1;
        cudaGraphNode_t node = nullptr;
        check(cudaGraphAddNode(&node, parent, dependencies, edges, dependencyCount, &params),
              "CUDA pressure WHILE node");
        const cudaGraph_t body = params.conditional.phGraph_out[0];
        bool capturing = false;
        cudaGraph_t ended = nullptr;
        try {
            check(cudaStreamBeginCaptureToGraph(p->bodyCapture, body, nullptr, nullptr, 0,
                                                cudaStreamCaptureModeThreadLocal),
                  "Begin CUDA pressure body capture");
            capturing = true;
            iterationBody(p->bodyCapture, 0xffffffffu);
            enqueuePressureLoopCondition(p->bodyCapture, m, v, p->config, handle, true);
            const auto result = cudaStreamEndCapture(p->bodyCapture, &ended);
            capturing = false;
            check(result, "End CUDA pressure body capture");
            if (ended != body)
                throw std::runtime_error("CUDA pressure capture changed its owned body graph");
            size_t nodes = 0;
            check(cudaGraphGetNodes(body, nullptr, &nodes), "CUDA pressure body node count");
            p->capturedBodyNodes += nodes;
        } catch (...) {
            if (capturing)
                cudaStreamEndCapture(p->bodyCapture, &ended);
            // The conditional node owns its body. The enclosing capture owner
            // destroys the parent on failure; do not independently destroy it.
            throw;
        }
        check(
            cudaStreamUpdateCaptureDependencies(stream, &node, nullptr, 1, cudaStreamSetCaptureDependencies),
            "Join CUDA pressure loop to enclosing capture");
    } else {
        if (status != cudaStreamCaptureStatusNone)
            throw std::runtime_error("Invalidated CUDA pressure capture");
        for (uint32_t iteration = 0; iteration < p->config.maxIterations; ++iteration)
            iterationBody(stream, iteration);
    }
    finish<<<1, 1, 0, stream>>>(m, v);
    check(cudaGetLastError(), "CUDA mixed MGPCG");
}
} // namespace lab::cuda_fluid
