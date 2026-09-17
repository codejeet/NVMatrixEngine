#include "fluid_cuda_transfer.h"
#include "fluid_cuda_geometry.cuh"
#include "fluid_cuda_device.cuh"
#include <cuda_runtime.h>
#include <cub/device/device_scan.cuh>
#include <cub/device/device_radix_sort.cuh>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
void check(cudaError_t result, const char *where) {
    if (result != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(result));
}
using namespace detail;
struct alignas(16) GridQuantity {
    double v[4];
};
static_assert(sizeof(GridQuantity) == 32);
struct DeviceData {
    Particle *particles;
    uint32_t *counts, *offsets, *cursors, *indices;
    float4 *faces;
    const uint32_t *failure = nullptr;
    const double4 *quantities = nullptr;
    float *cellMass = nullptr;
    const GridQuantity *gridQuantity = nullptr;
    const double *fineCapacity = nullptr;
    GridQuantity *fineQuantity = nullptr, *gridResult = nullptr;
    uint32_t *gridFailure = nullptr;
    double2 *phase = nullptr, *phaseInput = nullptr;
    double4 *planes = nullptr;
    bool narrowBand = false;
};
__device__ uint3 coarseExtent(Frame f) {
    return make_uint3((f.grid.x + 1) / 2, (f.grid.y + 1) / 2, (f.grid.z + 1) / 2);
}
__device__ int3 coarseBase(uint32_t id, Frame f) {
    const auto c = coarseExtent(f);
    return make_int3(2 * (id % c.x), 2 * ((id / c.x) % c.y), 2 * (id / (c.x * c.y)));
}
__device__ bool validGridQuantity(GridQuantity q) {
    return isfinite(q.v[0]) && isfinite(q.v[1]) && isfinite(q.v[2]) && isfinite(q.v[3]) && q.v[3] >= 0 &&
           (q.v[3] > 0 || (q.v[0] == 0 && q.v[1] == 0 && q.v[2] == 0));
}
__global__ void gatherGridPhase(DeviceData d, Frame f) {
    if (*d.gridFailure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    const auto c = coarseExtent(f);
    if (id >= c.x * c.y * c.z)
        return;
    const auto base = coarseBase(id, f);
    double volume = d.gridQuantity[id].v[3], capacity = 0;
    const double full = double(f.minimumCell.w) * f.minimumCell.w * f.minimumCell.w;
    for (int j = 0; j < 8; ++j) {
        const auto p = base + make_int3(j & 1, (j >> 1) & 1, j >> 2);
        if (!inGrid(p, f))
            continue;
        const uint32_t cell = cellIndex(p, f);
        const double open = d.fineCapacity[cell];
        if (!isfinite(open) || fabs(open - full) > full * 2e-13) {
            atomicExch(d.gridFailure, 1u);
            return;
        }
        capacity += open;
        for (uint32_t k = d.offsets[cell]; k < d.offsets[cell + 1]; ++k)
            volume += d.quantities[d.indices[k]].w;
    }
    if (!isfinite(volume) || volume < 0 || (!d.narrowBand && volume > capacity * (1 + 2e-13))) {
        atomicExch(d.gridFailure, 1u);
        return;
    }
    // In the narrow-band solver this is only a transport-support guide. Point
    // bin occupancy is NOT an exact cut-volume measurement. Do not reject or
    // clip authoritative quantities because particles straddle a cell boundary.
    // Grid owners remain capacity-bounded; the particle band handles the actual
    // free surface and positional density correction.
    (d.phaseInput ? d.phaseInput : d.phase)[id] = make_double2(volume, capacity);
}
__global__ void filterNarrowPhase(DeviceData d, Frame f) {
    if (*d.gridFailure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    const auto c = coarseExtent(f);
    if (id >= c.x * c.y * c.z)
        return;
    const int3 center = make_int3(id % c.x, (id / c.x) % c.y, id / (c.x * c.y));
    double fraction = 0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const int3 p = center + make_int3(x, y, z);
                if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= int(c.x) || p.y >= int(c.y) || p.z >= int(c.z))
                    continue; // Outside the domain is air, not a renormalized neighbor.
                const auto q = d.phaseInput[(p.z * c.y + p.y) * c.x + p.x];
                const double weight = (x ? .25 : .5) * (y ? .25 : .5) * (z ? .25 : .5);
                fraction += weight * q.x / q.y;
            }
    // Filter a NON-OWNING occupancy measurement before clamping it. Clamping
    // raw bins first biases an incommensurate particle lattice toward air.
    // Neither particle nor grid mass/momentum is modified by this guide.
    const double capacity = d.phaseInput[id].y;
    d.phase[id] =
        make_double2(fmax(d.gridQuantity[id].v[3], capacity * fmin(1., fmax(0., fraction))), capacity);
}
__global__ void reconstructGridPhase(DeviceData d, Frame f) {
    if (*d.gridFailure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    const auto c = coarseExtent(f);
    if (id >= c.x * c.y * c.z)
        return;
    d.planes[id] =
        geometry::reconstruct(id, c, make_uint3(f.grid.x, f.grid.y, f.grid.z), f.minimumCell.w, d.phase);
}
__device__ double childWet(DeviceData d, Frame f, uint32_t owner, int3 p, int3 base) {
    if (!d.phase || d.narrowBand)
        return 1;
    const auto extent = geometry::widths(make_uint3(base.x / 2, base.y / 2, base.z / 2),
                                         make_uint3(f.grid.x, f.grid.y, f.grid.z), 1.);
    const double3 lo =
        make_double3((p.x - base.x) / extent.x, (p.y - base.y) / extent.y, (p.z - base.z) / extent.z);
    const double3 hi = make_double3(lo.x + 1 / extent.x, lo.y + 1 / extent.y, lo.z + 1 / extent.z);
    return geometry::boxFraction(d.planes[owner], lo, hi);
}
__global__ void prepareGridQuantities(DeviceData d, Frame f) {
    if (*d.gridFailure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    const auto c = coarseExtent(f);
    if (id >= c.x * c.y * c.z)
        return;
    const auto q = d.gridQuantity[id];
    const int3 base = coarseBase(id, f);
    const double maximum = double(f.minimumCell.w) * double(f.minimumCell.w) * double(f.minimumCell.w);
    double capacity = 0, wetCapacity = 0;
    for (int j = 0; j < 8; ++j) {
        const int3 p = base + make_int3(j & 1, (j >> 1) & 1, j >> 2);
        if (!inGrid(p, f))
            continue;
        const double v = d.fineCapacity[cellIndex(p, f)];
        if (!isfinite(v) || v < 0 || v > maximum * (1 + 2e-13)) {
            atomicExch(d.gridFailure, 1u);
            return;
        }
        capacity += v;
        wetCapacity += v * childWet(d, f, id, p, base);
    }
    if (!validGridQuantity(q) || q.v[3] > capacity * (1 + 2e-13) || !isfinite(wetCapacity) ||
        (q.v[3] > 0 && wetCapacity <= 0)) {
        atomicExch(d.gridFailure, 1u);
        return;
    }
    if (!isfinite(float(q.v[3] / double(f.initialMinimum.w)))) {
        atomicExch(d.gridFailure, 1u);
        return;
    }
    for (uint32_t a = 0; a < 3; ++a)
        if (q.v[3] > 0 && !isfinite(float(q.v[a] / q.v[3]))) {
            atomicExch(d.gridFailure, 1u);
            return;
        }
    for (int j = 0; j < 8; ++j) {
        const int3 p = base + make_int3(j & 1, (j >> 1) & 1, j >> 2);
        if (!inGrid(p, f))
            continue;
        const uint32_t fine = cellIndex(p, f);
        const double fraction =
            wetCapacity > 0 ? d.fineCapacity[fine] * childWet(d, f, id, p, base) / wetCapacity : 0;
        GridQuantity value{};
        for (uint32_t a = 0; a < 4; ++a)
            value.v[a] = q.v[a] * fraction;
        d.fineQuantity[fine] = value; // Private transfer cache, not another owner.
    }
}
__device__ double volumeKernel(int3 cell, int3 face, int axis, Frame f) {
    // Exact quadratic B-spline integrals over a unit fine cell. Normal faces
    // use four cells (1,23,23,1)/48; transverse faces use (1,4,1)/6.
    double w = 1;
    for (int a = 0; a < 3; ++a) {
        const int delta = component(cell, a) - component(face, a);
        // Normalize the clipped finite-domain basis in both directions. Without
        // this, PIC return damps even a constant tangential flow near a domain
        // edge solely because some basis functions have no allocated face.
        const double missing = a == axis ? 1. / 48 : 1. / 6;
        const double support = 1 - missing * ((component(cell, a) == 0 ? 1 : 0) +
                                              (component(cell, a) + 1 == (&f.grid.x)[a] ? 1 : 0));
        if (a == axis) {
            if (delta < -2 || delta > 1)
                return 0;
            w *= (delta == -2 || delta == 1) ? 1. / 48 : 23. / 48;
        } else {
            if (delta < -1 || delta > 1)
                return 0;
            w *= delta == 0 ? 2. / 3 : 1. / 6;
        }
        w /= support;
    }
    return w;
}
__global__ void countParticles(DeviceData d, Frame f, uint32_t *keys, uint32_t *values) {
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    Particle p = d.particles[id];
    uint32_t key = 0xffffffffu;
    if ((!d.failure || !*d.failure) && p.velocityFlags.w != 0) {
        key = cellIndex(cellCoord(xyz(p.positionRadius), f), f);
        atomicAdd(d.counts + key, 1u);
    }
    if (keys) {
        keys[id] = key;
        values[id] = id;
    }
}
__global__ void finishOffsets(DeviceData d, uint32_t cells) {
    d.offsets[cells] = d.offsets[cells - 1] + d.counts[cells - 1];
}
__global__ void scatter(DeviceData d, Frame f) {
    if (d.failure && *d.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    Particle p = d.particles[id];
    if (!p.velocityFlags.w)
        return;
    uint32_t key = cellIndex(cellCoord(xyz(p.positionRadius), f), f);
    d.indices[d.offsets[key] + atomicAdd(d.cursors + key, 1u)] = id;
}
__global__ void gatherMass(DeviceData d, Frame f) {
    if (d.failure && *d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    double volume = 0;
    if (d.fineQuantity)
        volume = d.fineQuantity[id].v[3];
    for (uint32_t j = d.offsets[id]; j < d.offsets[id + 1]; ++j)
        volume += d.quantities[d.indices[j]].w;
    d.cellMass[id] = float(volume / double(f.initialMinimum.w));
}
template <bool Joint> __global__ void toGrid(DeviceData d, Frame f) {
    if (d.failure && *d.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x, stride = faceStride(f);
    if (id >= stride * 3)
        return;
    int axis = int(id / stride);
    uint32_t k = id % stride;
    int3 cell = make_int3(k % (f.grid.x + 1), (k / (f.grid.x + 1)) % (f.grid.y + 1),
                          k / ((f.grid.x + 1) * (f.grid.y + 1)));
    int3 extent = make_int3(f.grid.x, f.grid.y, f.grid.z);
    component(extent, axis)++;
    if (cell.x >= extent.x || cell.y >= extent.y || cell.z >= extent.z) {
        d.faces[id] = make_float4(0, 0, 0, 0);
        return;
    }
    float3 gp = make_float3(float(cell.x), float(cell.y), float(cell.z)) + faceOffset(axis);
    int3 lo = make_int3(max(int(floorf(gp.x - 1.5f)), 0), max(int(floorf(gp.y - 1.5f)), 0),
                        max(int(floorf(gp.z - 1.5f)), 0));
    int3 hi = make_int3(min(int(ceilf(gp.x + 1.5f)) - 1, int(f.grid.x) - 1),
                        min(int(ceilf(gp.y + 1.5f)) - 1, int(f.grid.y) - 1),
                        min(int(ceilf(gp.z + 1.5f)) - 1, int(f.grid.z) - 1));
    float momentum = 0, mass = 0;
    for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
            for (int x = lo.x; x <= hi.x; ++x) {
                uint32_t c = cellIndex(make_int3(x, y, z), f), end = d.offsets[c + 1];
                for (uint32_t j = d.offsets[c]; j < end; ++j) {
                    const uint32_t pid = d.indices[j];
                    float3 q = (xyz(d.particles[pid].positionRadius) - xyz(f.minimumCell)) *
                                   (1.f / f.minimumCell.w) -
                               gp;
                    const float kernel = quadratic(q.x) * quadratic(q.y) * quadratic(q.z);
                    if (kernel == 0)
                        continue; // No APIC/velocity/ownership loads outside support.
                    const Particle p = d.particles[pid];
                    float massWeight = p.apic0.w, particleVelocity = component(p.velocityFlags, axis);
                    if (d.quantities) {
                        const auto physical = d.quantities[pid];
                        massWeight = float(physical.w / double(f.initialMinimum.w));
                        particleVelocity = physical.w > 0 ? float((axis == 0   ? physical.x
                                                                   : axis == 1 ? physical.y
                                                                               : physical.z) /
                                                                  physical.w)
                                                          : 0;
                    }
                    float w = kernel * massWeight;
                    float3 row = xyz(axis == 0 ? p.apic0 : (axis == 1 ? p.apic1 : p.apic2));
                    if (f.solver.z > 0)
                        row = make_float3(0, 0, 0);
                    momentum += w * (particleVelocity - dot(row, q * f.minimumCell.w));
                    mass += w;
                }
            }
    float velocity = mass > 1e-8f ? momentum / mass : 0;
    if constexpr (Joint) {
        double jointMass = mass, jointMomentum = momentum;
        int3 low = cell - make_int3(1, 1, 1), high = cell + make_int3(1, 1, 1);
        component(low, axis)--;
        for (int z = low.z; z <= high.z; ++z)
            for (int y = low.y; y <= high.y; ++y)
                for (int x = low.x; x <= high.x; ++x) {
                    const int3 p = make_int3(x, y, z);
                    if (!inGrid(p, f))
                        continue;
                    const auto q = d.fineQuantity[cellIndex(p, f)];
                    if (q.v[3] == 0)
                        continue;
                    const double w = volumeKernel(p, cell, axis, f) / double(f.initialMinimum.w);
                    jointMass += w * q.v[3];
                    jointMomentum += w * q.v[axis];
                }
        mass = float(jointMass);
        velocity = jointMass > 0 ? float(jointMomentum / jointMass) : 0;
    }
    d.faces[id] = make_float4(velocity, velocity, mass, 0);
}
__global__ void gridReturn(DeviceData d, Frame f) {
    if (*d.gridFailure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    const auto c = coarseExtent(f);
    if (id >= c.x * c.y * c.z)
        return;
    auto result = d.gridQuantity[id];
    const int3 base = coarseBase(id, f);
    const double flip = f.solver.z > 0 ? double(f.solver.y) : 0;
    for (int axis = 0; axis < 3; ++axis) {
        double pic = 0, delta = 0;
        for (int j = 0; j < 8; ++j) {
            const int3 p = base + make_int3(j & 1, (j >> 1) & 1, j >> 2);
            if (!inGrid(p, f))
                continue;
            const double volume = d.fineQuantity[cellIndex(p, f)].v[3];
            if (volume == 0)
                continue;
            int3 low = p - make_int3(1, 1, 1), high = p + make_int3(1, 1, 1);
            component(high, axis)++;
            int3 extent = make_int3(f.grid.x, f.grid.y, f.grid.z);
            component(extent, axis)++;
            for (int z = low.z; z <= high.z; ++z)
                for (int y = low.y; y <= high.y; ++y)
                    for (int x = low.x; x <= high.x; ++x) {
                        const int3 face = make_int3(x, y, z);
                        if (x < 0 || y < 0 || z < 0 || x >= extent.x || y >= extent.y || z >= extent.z)
                            continue; // Same bounded basis as forward transfer.
                        const auto value = d.faces[faceIndex(face, axis, f)];
                        if (!isfinite(value.x) || !isfinite(value.y)) {
                            atomicExch(d.gridFailure, 1u);
                            return;
                        }
                        const double w = volume * volumeKernel(p, face, axis, f);
                        pic += w * double(value.x);
                        delta += w * (double(value.x) - double(value.y));
                    }
        }
        result.v[axis] += (1 - flip) * (pic - result.v[axis]) + flip * delta;
    }
    if (!validGridQuantity(result)) {
        atomicExch(d.gridFailure, 1u);
        return;
    }
    d.gridResult[id] = result;
}
__global__ void publishGridReturn(DeviceData d, Frame f, GridQuantity *output) {
    if (*d.gridFailure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    const auto c = coarseExtent(f);
    if (id < c.x * c.y * c.z)
        output[id] = d.gridResult[id];
}
__global__ void toParticles(DeviceData d, Frame f) {
    if (d.failure && *d.failure)
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    Particle p = d.particles[id];
    if (!p.velocityFlags.w)
        return;
    float3 pic = make_float3(0, 0, 0), delta = pic, rows[3];
    float h = f.minimumCell.w;
    for (int axis = 0; axis < 3; ++axis) {
        float3 gp = (xyz(p.positionRadius) - xyz(f.minimumCell)) * (1.f / h) - faceOffset(axis);
        int3 base = make_int3(int(floorf(gp.x - .5f)), int(floorf(gp.y - .5f)), int(floorf(gp.z - .5f)));
        float3 moment = make_float3(0, 0, 0);
        float3 weights[3];
        for (int k = 0; k < 3; ++k)
            weights[k] = make_float3(quadratic(float(base.x + k) - gp.x), quadratic(float(base.y + k) - gp.y),
                                     quadratic(float(base.z + k) - gp.z));
        for (int z = 0; z < 3; ++z)
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    int3 cell = make_int3(base.x + x, base.y + y, base.z + z);
                    float3 q = make_float3(float(cell.x), float(cell.y), float(cell.z)) - gp;
                    float w = weights[x].x * weights[y].y * weights[z].z;
                    int3 last = make_int3(f.grid.x - 1, f.grid.y - 1, f.grid.z - 1);
                    component(last, axis)++;
                    int3 bounded = make_int3(min(max(cell.x, 0), last.x), min(max(cell.y, 0), last.y),
                                             min(max(cell.z, 0), last.z));
                    float4 face = d.faces[faceIndex(bounded, axis, f)];
                    component(pic, axis) += w * face.x;
                    component(delta, axis) += w * (face.x - face.y);
                    moment = moment + q * (w * (face.x - component(p.velocityFlags, axis)));
                }
        rows[axis] = moment * (4 / h);
    }
    float3 velocity = f.solver.z > 0 ? pic + (xyz(p.velocityFlags) + delta - pic) * f.solver.y : pic;
    setXYZ(p.velocityFlags, velocity);
    setXYZ(p.apic0, rows[0]);
    setXYZ(p.apic1, rows[1]);
    setXYZ(p.apic2, rows[2]);
    if (f.counts.w != 4) {
        float3 pos = xyz(p.positionRadius) + pic * f.gravityDt.w;
        for (int axis = 0; axis < 3; ++axis) {
            float lo = component(f.minimumCell, axis) + p.positionRadius.w,
                  hi = component(f.maximumRadius, axis) - p.positionRadius.w;
            float &v = component(velocity, axis), &a = component(pos, axis);
            if (a < lo) {
                a = lo;
                v = fmaxf(v, 0);
            }
            if (a > hi) {
                a = hi;
                v = fminf(v, 0);
            }
        }
        setXYZ(p.positionRadius, pos);
        setXYZ(p.velocityFlags, velocity);
    }
    d.particles[id] = p;
}
} // namespace
struct Transfer {
    Config config{};
    DeviceData d{};
    uint32_t cells = 0, faces = 0;
    uint32_t *keys = nullptr, *sortedKeys = nullptr, *values = nullptr;
    void *scratch = nullptr;
    size_t scratchBytes = 0;
    ~Transfer() {
        cudaFree(d.fineQuantity);
        cudaFree(d.gridResult);
        cudaFree(d.phase);
        cudaFree(d.phaseInput);
        cudaFree(d.planes);
        if (scratch)
            cudaFree(scratch);
        if (keys)
            cudaFree(keys);
        if (sortedKeys)
            cudaFree(sortedKeys);
        if (values)
            cudaFree(values);
    }
};
Transfer *createTransfer(const Config &c, void *const (&buffers)[BufferCount], const Ownership &owned,
                         const GridOwnership &grid) {
    validateOwnership(c, owned);
    if (c.nx > 1048576 || c.ny > 1048576 || c.nz > 1048576)
        throw std::runtime_error("CUDA transfer grid dimension exceeds capacity");
    const uint64_t cells = uint64_t(c.nx) * c.ny * c.nz;
    if (!c.nx || !c.ny || !c.nz || cells > 1048576 || !c.capacity || c.capacity > 1048576)
        throw std::runtime_error("Invalid CUDA transfer grid/capacity");
    for (auto i : {Particles, Counts, Offsets, Cursors, Indices, Faces})
        if (!buffers[i])
            throw std::runtime_error("Missing CUDA transfer buffer");
    auto p = std::make_unique<Transfer>();
    p->config = c;
    p->cells = uint32_t(cells);
    p->faces = (c.nx + 1) * (c.ny + 1) * (c.nz + 1) * 3;
    p->d = {static_cast<Particle *>(buffers[Particles]), static_cast<uint32_t *>(buffers[Counts]),
            static_cast<uint32_t *>(buffers[Offsets]),   static_cast<uint32_t *>(buffers[Cursors]),
            static_cast<uint32_t *>(buffers[Indices]),   static_cast<float4 *>(buffers[Faces])};
    p->d.quantities = static_cast<const double4 *>(owned[OwnedQuantity]);
    p->d.cellMass = static_cast<float *>(owned[OwnedCellMass]);
    if (grid.quantity || grid.fineCapacity || grid.failure || grid.geometric) {
        if (!c.ownedParticles || !grid.quantity || !grid.fineCapacity || !grid.failure)
            throw std::runtime_error("Incomplete joint CUDA particle/grid ownership");
        const void *views[]{grid.quantity, grid.fineCapacity, grid.failure};
        for (uint32_t i = 0; i < 3; ++i) {
            for (auto source : buffers)
                if (views[i] == source)
                    throw std::runtime_error("Grid ownership aliases simulation fields");
            for (auto source : owned)
                if (views[i] == source)
                    throw std::runtime_error("Grid ownership aliases particle ledger");
            for (uint32_t j = 0; j < i; ++j)
                if (views[i] == views[j])
                    throw std::runtime_error("Aliased grid ownership view");
        }
        p->d.gridQuantity = static_cast<const GridQuantity *>(grid.quantity);
        p->d.fineCapacity = static_cast<const double *>(grid.fineCapacity);
        p->d.gridFailure = grid.failure;
        p->d.narrowBand = c.narrowBand;
        check(cudaMalloc(&p->d.fineQuantity, size_t(p->cells) * 32), "CUDA joint fine quantity cache");
        check(cudaMalloc(&p->d.gridResult, size_t((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2) * 32),
              "CUDA joint grid return staging");
        if (grid.geometric) {
            const size_t coarse = size_t((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2);
            check(cudaMalloc(&p->d.phase, coarse * 16), "CUDA joint total phase");
            if (c.narrowBand)
                check(cudaMalloc(&p->d.phaseInput, coarse * 16), "CUDA narrow-band raw occupancy");
            check(cudaMalloc(&p->d.planes, coarse * 32), "CUDA joint geometric support");
        }
    }
    check(cub::DeviceScan::ExclusiveSum(nullptr, p->scratchBytes, p->d.counts, p->d.offsets, p->cells),
          "CUDA scan scratch size");
    if (c.deterministic) {
        check(cudaMalloc(&p->keys, size_t(c.capacity) * 4), "CUDA bin keys");
        check(cudaMalloc(&p->sortedKeys, size_t(c.capacity) * 4), "CUDA sorted keys");
        check(cudaMalloc(&p->values, size_t(c.capacity) * 4), "CUDA bin values");
        size_t sortBytes = 0;
        check(cub::DeviceRadixSort::SortPairs(nullptr, sortBytes, p->keys, p->sortedKeys, p->values,
                                              p->d.indices, c.capacity),
              "CUDA sort scratch size");
        p->scratchBytes = std::max(p->scratchBytes, sortBytes);
    }
    check(cudaMalloc(&p->scratch, p->scratchBytes), "CUDA persistent bin scratch");
    return p.release();
}
void destroyTransfer(Transfer *p) noexcept {
    delete p;
}
void enqueueTransfer(Transfer *p, void *rawStream, const void *rawFrame, TransferStage stage,
                     void *facesOverride, const uint32_t *failure) {
    if (!p || !rawStream || !rawFrame)
        throw std::runtime_error("Invalid CUDA transfer invocation");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto &c = p->config;
    if (f.grid.x != c.nx || f.grid.y != c.ny || f.grid.z != c.nz || f.grid.w != p->cells ||
        f.counts.x != c.capacity || !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0 || f.display.z ||
        f.display.w != (c.ownedParticles ? 2u : 0u) ||
        (c.ownedParticles && (!std::isfinite(f.initialMinimum.w) || f.initialMinimum.w <= 0)))
        throw std::runtime_error("CUDA baseline transfer grid/ownership mismatch");
    auto stream = static_cast<cudaStream_t>(rawStream);
    auto d = p->d;
    if (d.gridFailure && failure && failure != d.gridFailure)
        throw std::runtime_error("Joint transfer requires one shared failure latch");
    d.failure = failure;
    if (d.gridFailure)
        d.failure = d.gridFailure;
    if (facesOverride)
        d.faces = static_cast<float4 *>(facesOverride);
    switch (stage) {
    case TransferStage::Bin: {
        if (d.gridQuantity && !d.phase) {
            const uint32_t coarse = ((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2);
            prepareGridQuantities<<<(coarse + 127) / 128, 128, 0, stream>>>(d, f);
            check(cudaGetLastError(), "CUDA grid-owned prediction preparation");
        }
        check(cudaMemsetAsync(d.counts, 0, size_t(p->cells) * 4, stream), "CUDA clear cell counts");
        countParticles<<<(c.capacity + 127) / 128, 128, 0, stream>>>(d, f, p->keys, p->values);
        check(cudaGetLastError(), "CUDA count particles");
        size_t temporaryBytes = p->scratchBytes;
        check(
            cub::DeviceScan::ExclusiveSum(p->scratch, temporaryBytes, d.counts, d.offsets, p->cells, stream),
            "CUDA particle scan");
        finishOffsets<<<1, 1, 0, stream>>>(d, p->cells);
        check(cudaGetLastError(), "CUDA finish offsets");
        if (c.deterministic) {
            temporaryBytes = p->scratchBytes;
            check(cub::DeviceRadixSort::SortPairs(p->scratch, temporaryBytes, p->keys, p->sortedKeys,
                                                  p->values, d.indices, c.capacity, 0, 32, stream),
                  "CUDA deterministic bin sort");
        } else {
            check(cudaMemsetAsync(d.cursors, 0, size_t(p->cells) * 4, stream), "CUDA clear cell cursors");
            scatter<<<(c.capacity + 127) / 128, 128, 0, stream>>>(d, f);
            check(cudaGetLastError(), "CUDA scatter particles");
        }
        if (d.phase) {
            const uint32_t coarse = ((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2);
            gatherGridPhase<<<(coarse + 127) / 128, 128, 0, stream>>>(d, f);
            if (d.phaseInput)
                filterNarrowPhase<<<(coarse + 127) / 128, 128, 0, stream>>>(d, f);
            reconstructGridPhase<<<(coarse + 127) / 128, 128, 0, stream>>>(d, f);
            prepareGridQuantities<<<(coarse + 127) / 128, 128, 0, stream>>>(d, f);
            check(cudaGetLastError(), "CUDA geometric grid-owned support");
        }
        if (c.ownedParticles) {
            gatherMass<<<(p->cells + 127) / 128, 128, 0, stream>>>(d, f);
            check(cudaGetLastError(), "CUDA authoritative cell mass");
        }
        break;
    }
    case TransferStage::ToGrid:
        // Do not impose the joint FP64 integration's register/loop footprint
        // on the faster existing all-particle kernel.
        if (d.gridQuantity)
            toGrid<true><<<(p->faces + 127) / 128, 128, 0, stream>>>(d, f);
        else
            toGrid<false><<<(p->faces + 127) / 128, 128, 0, stream>>>(d, f);
        check(cudaGetLastError(), "CUDA APIC P2G");
        break;
    case TransferStage::ToParticles:
        toParticles<<<(c.capacity + 127) / 128, 128, 0, stream>>>(d, f);
        check(cudaGetLastError(), "CUDA APIC/FLIP G2P");
        break;
    default:
        throw std::runtime_error("Unknown CUDA transfer stage");
    }
}
size_t gridOwnershipTransferBytes(const Transfer *p) {
    if (!p || !p->d.gridQuantity)
        return 0;
    const auto &c = p->config;
    const size_t coarse = size_t((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2);
    return 32 * (size_t(p->cells) + coarse) + (p->d.phase ? 48 * coarse : 0) +
           (p->d.phaseInput ? 16 * coarse : 0);
}
const void *gridOwnershipPhase(const Transfer *p) {
    return p ? p->d.phase : nullptr;
}
const void *gridOwnershipPlanes(const Transfer *p) {
    return p ? p->d.planes : nullptr;
}
void enqueueGridOwnershipReturn(Transfer *p, void *stream, const void *rawFrame, void *output,
                                void *facesOverride) {
    if (!p || !stream || !rawFrame || !output || !p->d.gridQuantity)
        throw std::runtime_error("Missing joint grid return view");
    auto d = p->d;
    if (facesOverride)
        d.faces = static_cast<float4 *>(facesOverride);
    const void *sources[]{d.gridQuantity, d.fineCapacity, d.gridFailure, d.faces,   d.particles,
                          d.quantities,   d.fineQuantity, d.gridResult,  d.cellMass};
    for (auto source : sources)
        if (output == source)
            throw std::runtime_error("Aliased joint grid return destination");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto &c = p->config;
    if (f.grid.x != c.nx || f.grid.y != c.ny || f.grid.z != c.nz || f.grid.w != p->cells ||
        f.display.w != 2 || f.display.z || !std::isfinite(f.solver.y) || f.solver.y < 0 || f.solver.y > 1 ||
        !std::isfinite(f.solver.z))
        throw std::runtime_error("Invalid joint grid return frame");
    const uint32_t coarse = ((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2);
    const auto s = static_cast<cudaStream_t>(stream);
    gridReturn<<<(coarse + 127) / 128, 128, 0, s>>>(d, f);
    publishGridReturn<<<(coarse + 127) / 128, 128, 0, s>>>(d, f, static_cast<GridQuantity *>(output));
    check(cudaGetLastError(), "CUDA grid-owned PIC/FLIP return");
}
} // namespace lab::cuda_fluid
