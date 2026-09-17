#include "fluid_cuda_narrow_band.h"
#include "fluid_cuda_device.cuh"
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
struct alignas(16) Quantity {
    double x, y, z, w;
};
static_assert(sizeof(Quantity) == 32);
struct View {
    uint3 coarse;
    uint32_t cells, capacity;
    Particle *particles;
    Quantity *quantity, *grid;
    float4 *reference, *previous;
    const uint32_t *offsets, *indices;
    const float4 *solid;
    const double2 *phase;
    double *open;
    Quantity *combined;
    uint32_t *candidate, *request, *age, *freeIds, *allocation;
    NarrowBandMetrics *metrics;
    uint32_t *failure;
};
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
__device__ bool valid(Quantity q) {
    return isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w) && q.w >= 0 &&
           (q.w > 0 || (q.x == 0 && q.y == 0 && q.z == 0));
}
__device__ Quantity add(Quantity a, Quantity b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
__device__ Quantity scale(Quantity a, double s) {
    return {a.x * s, a.y * s, a.z * s, a.w * s};
}
__device__ uint32_t index(int3 p, View v) {
    return (p.z * v.coarse.y + p.y) * v.coarse.x + p.x;
}
__device__ int3 coord(uint32_t i, View v) {
    return make_int3(i % v.coarse.x, i / v.coarse.x % v.coarse.y, i / (v.coarse.x * v.coarse.y));
}
__device__ bool inside(int3 p, View v) {
    return p.x >= 0 && p.y >= 0 && p.z >= 0 && p.x < int(v.coarse.x) && p.y < int(v.coarse.y) &&
           p.z < int(v.coarse.z);
}
__device__ void reject(View v, uint32_t stage) {
    atomicAdd(&v.metrics->invalid, 1u);
    atomicCAS(&v.metrics->reserved, 0u, stage);
    atomicExch(v.failure, 1u);
}
__global__ void reset(Frame f, View v) {
    if (*v.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < v.cells) {
        v.age[id] = v.request[id] = v.candidate[id] = 0;
    }
    if (id < f.grid.w)
        v.open[id] = double(f.minimumCell.w) * f.minimumCell.w * f.minimumCell.w;
    if (!id) {
        *v.metrics = {};
        v.allocation[0] = v.allocation[1] = 0;
        v.allocation[2] = f.counts.y;
    }
}
__global__ void begin(Frame f, View v) {
    if (!*v.failure)
        v.allocation[2] = max(v.allocation[2], max(f.counts.y, f.emission.x + f.emission.y));
}
__global__ void gather(Frame f, View v) {
    if (*v.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.cells)
        return;
    auto sum = v.grid[id];
    int3 base = coord(id, v) * 2;
    if (!valid(sum)) {
        reject(v, 1);
        return;
    }
    for (uint32_t child = 0; child < 8; ++child) {
        int3 p = base + make_int3(child & 1, (child >> 1) & 1, child >> 2);
        if (!inGrid(p, f))
            continue;
        uint32_t c = cellIndex(p, f);
        for (uint32_t j = v.offsets[c]; j < v.offsets[c + 1]; ++j) {
            const uint32_t pid = v.indices[j];
            if (pid >= v.capacity || !v.particles[pid].velocityFlags.w || !valid(v.quantity[pid]) ||
                v.quantity[pid].w <= 0) {
                reject(v, 2);
                return;
            }
            sum = add(sum, v.quantity[pid]);
        }
    }
    if (!valid(sum)) {
        reject(v, 3);
        return;
    }
    v.combined[id] = sum;
}
__global__ void classify(Frame f, View v, bool force) {
    if (*v.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.cells)
        return;
    const int3 center = coord(id, v), base = center * 2;
    const double h = f.minimumCell.w, full = 8 * h * h * h;
    const auto total = v.combined[id];
    const auto phase = v.phase[id];
    bool eligible =
        !force && inGrid(base + make_int3(1, 1, 1), f) && total.w > .5 * full && phase.x > .75 * phase.y;
    const float3 mean = total.w > 0 ? make_float3(float(total.x / total.w), float(total.y / total.w),
                                                  float(total.z / total.w))
                                    : make_float3(0, 0, 0);
    // Three fine cells, plus one coarse-cell padding below. Current occupancy
    // and a swept collider margin always override previous quiet decisions.
    // Unlike the old dormant-lattice path, uniform translation is admissible.
    for (int z = -1; z <= 1 && eligible; ++z)
        for (int y = -1; y <= 1 && eligible; ++y)
            for (int x = -1; x <= 1 && eligible; ++x) {
                const int3 n = center + make_int3(x, y, z);
                if (!inside(n, v)) {
                    eligible = false;
                    break;
                }
                const auto ni = index(n, v);
                const auto q = v.combined[ni];
                const auto support = v.phase[ni];
                if (q.w < .5 * full || support.x < .75 * support.y) {
                    eligible = false;
                    break;
                }
                const float3 velocity = make_float3(float(q.x / q.w), float(q.y / q.w), float(q.z / q.w));
                // Bound unresolved displacement over the prediction horizon,
                // rather than using one m/s cutoff at every simulation scale.
                if (length(velocity - mean) > float(.05 * h / .1)) {
                    eligible = false;
                    break;
                }
                for (uint32_t child = 0; child < 8 && eligible; ++child) {
                    const int3 p = n * 2 + make_int3(child & 1, (child >> 1) & 1, child >> 2);
                    if (!inGrid(p, f)) {
                        eligible = false;
                        break;
                    }
                    const auto s = v.solid[cellIndex(p, f)];
                    const float speed = length(make_float3(s.y, s.z, s.w));
                    // At least a whole coarse-cell travel margin before restoration.
                    if (!isfinite(s.x) || s.x < float(h * 2) + speed * fmaxf(.15f, f.gravityDt.w))
                        eligible = false;
                }
            }
    double variance = 0, px = 0, py = 0, pz = 0;
    const float3 cellCenter = xyz(f.minimumCell) + (asFloat(base) + radiusVector(1.f)) * float(h);
    for (uint32_t child = 0; child < 8 && eligible; ++child) {
        const int3 p = base + make_int3(child & 1, (child >> 1) & 1, child >> 2);
        uint32_t c = cellIndex(p, f);
        for (uint32_t j = v.offsets[c]; j < v.offsets[c + 1]; ++j) {
            uint32_t pid = v.indices[j];
            const auto p = v.particles[pid];
            const auto q = v.quantity[pid];
            const double vx = q.x / q.w - mean.x, vy = q.y / q.w - mean.y, vz = q.z / q.w - mean.z;
            const double affine = double(dot(xyz(p.apic0), xyz(p.apic0))) + dot(xyz(p.apic1), xyz(p.apic1)) +
                                  dot(xyz(p.apic2), xyz(p.apic2));
            variance += q.w * (vx * vx + vy * vy + vz * vz + .25 * h * h * affine);
            const auto d = xyz(p.positionRadius) - cellCenter;
            px += q.w * d.x;
            py += q.w * d.y;
            pz += q.w * d.z;
        }
    }
    // The grid stores a mean, not arbitrary APIC detail. Admission bounds the
    // discarded velocity variance; it does not claim exact angular conservation.
    const double detailVelocity = .025 * h / .1;
    if (eligible && variance > total.w * detailVelocity * detailVelocity)
        eligible = false;
    const bool protect = !eligible;
    if (eligible && px * px + py * py + pz * pz > total.w * total.w * h * h * .04)
        eligible = false;
    // Centroid admission is local to an exchange, NOT a free-surface or wake
    // refinement flag. Padding its failure used to propagate lattice sampling
    // jitter across the whole pool, preventing any real-scene retirement.
    v.candidate[id] = (eligible ? 1u : 0u) | (protect ? 2u : 0u);
}
__global__ void decide(Frame f, View v, bool advanceAge) {
    if (*v.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.cells)
        return;
    const int3 c = coord(id, v);
    bool eligible = (v.candidate[id] & 1u) != 0;
    for (int z = -1; z <= 1 && eligible; ++z)
        for (int y = -1; y <= 1 && eligible; ++y)
            for (int x = -1; x <= 1 && eligible; ++x) {
                int3 p = c + make_int3(x, y, z);
                if (!inside(p, v) || (v.candidate[index(p, v)] & 2u))
                    eligible = false;
            }
    // Dwell in physical time, quantized once to substeps. Immediate promotion
    // back to particles, delayed demotion to the grid.
    const uint32_t dwell = uint32_t(ceilf(.1f / f.gravityDt.w));
    uint32_t age = eligible ? min(v.age[id] + uint32_t(advanceAge), dwell) : 0;
    v.age[id] = age;
    v.request[id] = eligible && age >= dwell;
}
__global__ void deposit(Frame f, View v) {
    if (*v.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.cells || !v.request[id])
        return;
    auto sum = v.grid[id];
    const double h = f.minimumCell.w, capacity = 8 * h * h * h;
    if (!valid(sum) || sum.w > capacity * (1 + 2e-13)) {
        reject(v, 4);
        return;
    }
    const int3 base = coord(id, v) * 2;
    uint32_t removed = 0;
    for (uint32_t child = 0; child < 8; ++child) {
        const int3 p = base + make_int3(child & 1, (child >> 1) & 1, child >> 2);
        if (!inGrid(p, f))
            continue;
        const uint32_t c = cellIndex(p, f);
        for (uint32_t j = v.offsets[c]; j < v.offsets[c + 1]; ++j) {
            const uint32_t pid = v.indices[j];
            const auto q = v.quantity[pid];
            const double room = fmax(0., capacity - sum.w);
            if (room <= 0)
                break;
            if (q.w <= room) {
                sum = add(sum, q);
                v.particles[pid] = {};
                v.quantity[pid] = {};
                v.reference[pid] = {};
                ++removed;
            } else {
                // Point bins can exceed geometric capacity. Retire only the
                // admissible fraction, retaining the excess as a real particle.
                // Never clip the physical ledger to make the capacity audit pass.
                auto take = scale(q, room / q.w);
                take.w = room;
                const auto remain = add(q, scale(take, -1.));
                sum = add(sum, take);
                v.quantity[pid] = remain;
                v.particles[pid].apic0.w = float(remain.w / double(f.initialMinimum.w));
                const float3 velocity = make_float3(float(remain.x / remain.w), float(remain.y / remain.w),
                                                    float(remain.z / remain.w));
                v.particles[pid].velocityFlags = make_float4(velocity.x, velocity.y, velocity.z, 1);
                v.reference[pid] = make_float4(velocity.x, velocity.y, velocity.z, 0);
            }
        }
    }
    v.grid[id] = sum;
    if (removed) {
        atomicAdd(&v.metrics->retired, removed);
        atomicExch(&v.metrics->changed, 1u);
    }
}
__global__ void clearFree(View v) {
    if (!*v.failure) {
        v.allocation[0] = v.allocation[1] = 0;
    }
}
__global__ void freeIds(View v, uint32_t prefix) {
    if (*v.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= min(prefix, v.allocation[2]) || v.particles[id].velocityFlags.w)
        return;
    if (!valid(v.quantity[id]) || v.quantity[id].w != 0) {
        reject(v, 5);
        return;
    }
    v.freeIds[atomicAdd(v.allocation, 1u)] = id;
}
__device__ bool reserve(View v, uint32_t count, uint32_t &first) {
    const uint32_t available = v.allocation[0];
    uint32_t old = v.allocation[1];
    for (;;) {
        if (old > available || count > available - old)
            return false;
        uint32_t actual = atomicCAS(v.allocation + 1, old, old + count);
        if (actual == old) {
            first = old;
            return true;
        }
        old = actual;
    }
}
__device__ bool site(Frame f, View v, float3 p) {
    if (outside(p, xyz(f.minimumCell) + radiusVector(f.maximumRadius.w),
                xyz(f.maximumRadius) - radiusVector(f.maximumRadius.w)))
        return false;
    // Conservative lower bound from the nearest SDF sample. Every restoration
    // point must be collision-safe BEFORE IDs or either owner are modified.
    int3 c = cellCoord(p, f);
    const float3 center = xyz(f.minimumCell) + (asFloat(c) + radiusVector(.5f)) * f.minimumCell.w;
    const float phi = v.solid[cellIndex(c, f)].x - length(p - center);
    return isfinite(phi) && phi >= f.maximumRadius.w;
}
__global__ void restore(Frame f, View v) {
    if (*v.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.cells || v.request[id])
        return;
    const auto q = v.grid[id];
    if (q.w == 0)
        return;
    if (!valid(q)) {
        reject(v, 6);
        return;
    }
    // Rebirth has a physical sampling granularity. Spawning eight infinitesimal
    // particles for every advected tail consumes the entire free pool while
    // restoring almost no volume. Accumulate at least one useful 2^3 stencil;
    // smaller quantities remain actual flowing grid owners, never a sink.
    if (q.w < 8 * double(f.initialMinimum.w)) {
        atomicAdd(&v.metrics->deferred, 1u);
        return;
    }
    const uint32_t wanted = uint32_t(fmin(64., ceil(q.w / double(f.initialMinimum.w))));
    uint32_t sites[64], count = 0;
    const float3 base = xyz(f.minimumCell) + asFloat(coord(id, v) * 2) * f.minimumCell.w;
    // A fixed permutation provides spatially distributed prefixes. Site order
    // depends on the cell, never on free-list reservation races.
    for (uint32_t k = 0; k < 64 && count < wanted; ++k) {
        uint32_t i = (37 * k + (id * 13)) & 63;
        float3 p =
            base + (make_float3(float(i & 3), float((i >> 2) & 3), float(i >> 4)) + radiusVector(.5f)) *
                       (.5f * f.minimumCell.w);
        if (site(f, v, p))
            sites[count++] = i;
    }
    if (!count) {
        atomicAdd(&v.metrics->deferred, 1u);
        return;
    }
    const auto each = scale(q, 1. / count);
    const float weight = float(each.w / double(f.initialMinimum.w));
    const float3 velocity = make_float3(float(q.x / q.w), float(q.y / q.w), float(q.z / q.w));
    if (weight >= 0 && weight < 1.1754943508222875e-38f) {
        // Conservative transport can leave a real FP64 tail smaller than a
        // normal FP32 particle weight. It still belongs to the grid. Do not
        // delete it, poison the frame, or publish a zero-weight live particle.
        atomicAdd(&v.metrics->deferred, 1u);
        return;
    }
    if (!(weight > 0) || !isfinite(weight) || !isfinite(velocity.x) || !isfinite(velocity.y) ||
        !isfinite(velocity.z)) {
        reject(v, 7);
        return;
    }
    uint32_t first;
    if (!reserve(v, count, first)) {
        atomicAdd(&v.metrics->deferred, 1u);
        return;
    }
    auto remaining = q;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t pid = v.freeIds[first + k], i = sites[k];
        const auto owned = k + 1 == count ? remaining : each;
        remaining = add(remaining, scale(owned, -1.));
        const float3 position =
            base + (make_float3(float(i & 3), float((i >> 2) & 3), float(i >> 4)) + radiusVector(.5f)) *
                       (.5f * f.minimumCell.w);
        Particle p{};
        p.positionRadius = make_float4(position.x, position.y, position.z, f.maximumRadius.w);
        p.velocityFlags = make_float4(velocity.x, velocity.y, velocity.z, 1);
        p.apic0.w = float(owned.w / double(f.initialMinimum.w));
        v.particles[pid] = p;
        v.quantity[pid] = owned;
        v.reference[pid] = make_float4(velocity.x, velocity.y, velocity.z, 0);
        v.previous[pid] = p.positionRadius;
    }
    v.grid[id] = {};
    atomicAdd(&v.metrics->restored, count);
    atomicExch(&v.metrics->changed, 1u);
}
__global__ void clearMetrics(View v) {
    if (*v.failure)
        return;
    v.metrics->activeParticles = v.metrics->gridCells = 0;
    v.metrics->gridVolume = v.metrics->particleVolume = 0;
}
__global__ void measure(View v) {
    if (*v.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < v.capacity && v.particles[id].velocityFlags.w) {
        atomicAdd(&v.metrics->activeParticles, 1u);
        atomicAdd(&v.metrics->particleVolume, v.quantity[id].w);
    }
    if (id < v.cells && v.grid[id].w > 0) {
        atomicAdd(&v.metrics->gridCells, 1u);
        atomicAdd(&v.metrics->gridVolume, v.grid[id].w);
    }
}
} // namespace
struct NarrowBand {
    View v{};
    size_t bytes = 0;
    ~NarrowBand() {
        for (void *p :
             {static_cast<void *>(v.combined), static_cast<void *>(v.candidate),
              static_cast<void *>(v.request), static_cast<void *>(v.age), static_cast<void *>(v.freeIds),
              static_cast<void *>(v.allocation), static_cast<void *>(v.metrics)})
            if (p)
                cudaFree(p);
    }
};
NarrowBand *createNarrowBand(const Config &c, void *const (&buffers)[BufferCount], const Ownership &owned,
                             const GridInventory &grid, const void *phase, void *previous,
                             uint32_t *failure) {
    if (!validateGridInventory(c, grid) || !phase || !previous || !failure)
        throw std::runtime_error("Incomplete narrow-band ownership bindings");
    auto p = std::make_unique<NarrowBand>();
    auto &v = p->v;
    v.coarse = make_uint3((c.nx + 1) / 2, (c.ny + 1) / 2, (c.nz + 1) / 2);
    v.cells = v.coarse.x * v.coarse.y * v.coarse.z;
    v.capacity = c.capacity;
    v.particles = static_cast<Particle *>(buffers[Particles]);
    v.quantity = static_cast<Quantity *>(owned[OwnedQuantity]);
    v.reference = static_cast<float4 *>(owned[OwnedReference]);
    v.grid = static_cast<Quantity *>(grid[GridOwnedQuantity]);
    v.open = static_cast<double *>(grid[GridFineCapacity]);
    v.previous = static_cast<float4 *>(previous);
    v.offsets = static_cast<const uint32_t *>(buffers[Offsets]);
    v.indices = static_cast<const uint32_t *>(buffers[Indices]);
    v.solid = static_cast<const float4 *>(buffers[Solid]);
    v.phase = static_cast<const double2 *>(phase);
    v.failure = failure;
    auto allocate = [&](auto &dst, size_t bytes) {
        check(cudaMalloc(reinterpret_cast<void **>(&dst), bytes), "CUDA narrow-band allocation");
        p->bytes += bytes;
        check(cudaMemset(dst, 0, bytes), "CUDA narrow-band initialization");
    };
    allocate(v.combined, size_t(v.cells) * 32);
    allocate(v.candidate, size_t(v.cells) * 4);
    allocate(v.request, size_t(v.cells) * 4);
    allocate(v.age, size_t(v.cells) * 4);
    allocate(v.freeIds, size_t(v.capacity) * 4);
    allocate(v.allocation, 12);
    allocate(v.metrics, sizeof(NarrowBandMetrics));
    check(cudaStreamSynchronize(nullptr), "CUDA narrow-band initialization completion");
    return p.release();
}
void destroyNarrowBand(NarrowBand *p) noexcept {
    delete p;
}
size_t narrowBandBytes(const NarrowBand *p) {
    return p ? p->bytes : 0;
}
const NarrowBandMetrics *narrowBandMetrics(const NarrowBand *p) {
    return p ? p->v.metrics : nullptr;
}
void resetNarrowBand(NarrowBand *p, void *rawStream, const void *rawFrame) {
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    reset<<<(std::max(f.grid.w, p->v.cells) + 127) / 128, 128, 0, static_cast<cudaStream_t>(rawStream)>>>(
        f, p->v);
    check(cudaGetLastError(), "CUDA narrow-band reset");
}
void beginNarrowBandFrame(NarrowBand *p, void *rawStream, const void *rawFrame) {
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    begin<<<1, 1, 0, static_cast<cudaStream_t>(rawStream)>>>(f, p->v);
    check(cudaGetLastError(), "CUDA narrow-band source prefix");
}
void exchangeNarrowBand(NarrowBand *p, void *rawStream, const void *rawFrame, const void *, const float *,
                        uint32_t prefix, bool force, bool advanceAge) {
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto s = static_cast<cudaStream_t>(rawStream);
    const auto v = p->v;
    if (prefix > v.capacity)
        throw std::runtime_error("Narrow-band reusable prefix exceeds particle pool");
    const uint32_t groups = (v.cells + 127) / 128;
    gather<<<groups, 128, 0, s>>>(f, v);
    classify<<<groups, 128, 0, s>>>(f, v, force);
    decide<<<groups, 128, 0, s>>>(f, v, advanceAge);
    deposit<<<groups, 128, 0, s>>>(f, v);
    clearFree<<<1, 1, 0, s>>>(v);
    if (prefix)
        freeIds<<<(prefix + 127) / 128, 128, 0, s>>>(v, prefix);
    restore<<<groups, 128, 0, s>>>(f, v);
    check(cudaGetLastError(), "CUDA narrow-band retire/restore");
}
void measureNarrowBand(NarrowBand *p, void *rawStream, const void *) {
    const auto s = static_cast<cudaStream_t>(rawStream);
    clearMetrics<<<1, 1, 0, s>>>(p->v);
    measure<<<(std::max(p->v.capacity, p->v.cells) + 127) / 128, 128, 0, s>>>(p->v);
    check(cudaGetLastError(), "CUDA narrow-band ownership metrics");
}
} // namespace lab::cuda_fluid
