#include "fluid_cuda_ownership.h"
#include "fluid_cuda_device.cuh"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
struct Ledger {
    Particle *particles;
    double4 *quantities;
    float4 *references;
    uint32_t *control, *failure;
};
__device__ bool finite(float4 v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z) && isfinite(v.w);
}
__device__ bool quantity(double4 q) {
    return isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w) && q.w >= 0 &&
           (q.w > 0 || (q.x == 0 && q.y == 0 && q.z == 0));
}
__device__ void reject(Ledger d) {
    atomicAdd(d.control + 7, 1u);
    atomicAdd(d.control + 16, 1u);
    atomicExch(d.failure, 1u);
}
__global__ void update(Ledger d, Frame f, bool delta) {
    if (*d.failure)
        return;
    if (d.control[16]) {
        atomicExch(d.failure, 1u);
        return;
    }
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    const auto p = d.particles[id];
    auto q = d.quantities[id];
    if (!p.velocityFlags.w) {
        if (!quantity(q) || q.w != 0)
            reject(d);
        return;
    }
    const auto ref = d.references[id];
    if (!quantity(q) || q.w <= 0 || !finite(ref) || !finite(p.positionRadius) || !finite(p.velocityFlags) ||
        !finite(p.apic0) || !finite(p.apic1) || !finite(p.apic2) || p.positionRadius.w <= 0 ||
        p.apic0.w <= 0) {
        reject(d);
        return;
    }
    const float weight = float(q.w / double(f.initialMinimum.w));
    const float4 physical = make_float4(float(q.x / q.w), float(q.y / q.w), float(q.z / q.w), weight);
    if (!finite(physical) || weight <= 0 || fabsf(p.apic0.w - weight) > 2e-7f * fmaxf(1.f, weight)) {
        reject(d);
        return;
    }
    if (!delta)
        return;
    q.x += q.w * (double(p.velocityFlags.x) - double(ref.x));
    q.y += q.w * (double(p.velocityFlags.y) - double(ref.y));
    q.z += q.w * (double(p.velocityFlags.z) - double(ref.z));
    if (!quantity(q)) {
        reject(d);
        return;
    }
    d.quantities[id] = q;
    d.references[id] = make_float4(p.velocityFlags.x, p.velocityFlags.y, p.velocityFlags.z, 0);
}
} // namespace
size_t ownershipBytes(const Config &c, OwnershipBuffer b) {
    bufferBytes(c, Particles); // shared bounds contract, before size arithmetic
    switch (b) {
    case OwnedQuantity:
        return size_t(c.capacity) * 32;
    case OwnedReference:
        return size_t(c.capacity) * 16;
    case OwnedCellMass:
        return size_t(c.nx) * c.ny * c.nz * 4;
    case OwnedControl:
        return 17 * sizeof(uint32_t); // Only initialized/public exchange counters.
    default:
        throw std::runtime_error("Unknown CUDA ownership buffer");
    }
}
void validateOwnership(const Config &c, const Ownership &v) {
    for (uint32_t i = 0; i < v.size(); ++i) {
        if (bool(v[i]) != c.ownedParticles)
            throw std::runtime_error("CUDA ownership selection/view mismatch");
        if (v[i])
            for (uint32_t j = 0; j < i; ++j)
                if (v[i] == v[j])
                    throw std::runtime_error("Aliased CUDA ownership buffers");
    }
}
void enqueueOwnership(void *stream, void *particles, const Ownership &v, const void *rawFrame,
                      OwnershipStage stage, uint32_t *failure) {
    if (!stream || !particles || !rawFrame || !failure)
        throw std::runtime_error("Missing CUDA ownership invocation");
    for (auto p : v)
        if (!p)
            throw std::runtime_error("Incomplete CUDA ownership view");
    if (stage != OwnershipStage::Validate && stage != OwnershipStage::VelocityDelta)
        throw std::runtime_error("Invalid CUDA ownership stage");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    if (!f.counts.x || f.counts.x > 1048576 || f.display.z || f.display.w != 2 ||
        !std::isfinite(f.initialMinimum.w) || f.initialMinimum.w <= 0)
        throw std::runtime_error("Invalid CUDA ownership frame");
    Ledger d{static_cast<Particle *>(particles), static_cast<double4 *>(v[OwnedQuantity]),
             static_cast<float4 *>(v[OwnedReference]), static_cast<uint32_t *>(v[OwnedControl]), failure};
    update<<<(f.counts.x + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(
        d, f, stage == OwnershipStage::VelocityDelta);
    const auto e = cudaGetLastError();
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("CUDA ownership kernel: ") + cudaGetErrorString(e));
}
} // namespace lab::cuda_fluid
