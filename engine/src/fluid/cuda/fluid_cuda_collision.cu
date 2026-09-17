#include "fluid_cuda_collision.h"
#include "fluid_cuda_device.cuh"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
__device__ float colliderPhi(Collider c, float3 world, const float *mesh) {
    float3 p = xyz(c.worldToLocal[0]) * world.x + xyz(c.worldToLocal[1]) * world.y +
               xyz(c.worldToLocal[2]) * world.z + xyz(c.worldToLocal[3]);
    float3 e = xyz(c.extentType);
    uint32_t type = uint32_t(c.extentType.w);
    if (type == 0)
        return length(p) - e.x;
    if (type == 1) {
        float3 q = make_float3(fabsf(p.x) - e.x, fabsf(p.y) - e.y, fabsf(p.z) - e.z);
        return length(make_float3(fmaxf(q.x, 0), fmaxf(q.y, 0), fmaxf(q.z, 0))) +
               fminf(fmaxf(q.x, fmaxf(q.y, q.z)), 0);
    }
    if (type == 2) {
        p.y -= fminf(fmaxf(p.y, -e.y), e.y);
        return length(p) - e.x;
    }
    if (type == 3) {
        float x = sqrtf(p.x * p.x + p.z * p.z) - e.x, y = fabsf(p.y) - e.y;
        float a = fmaxf(x, 0), b = fmaxf(y, 0);
        return sqrtf(a * a + b * b) + fminf(fmaxf(x, y), 0);
    }
    if (type == 4)
        return p.y;
    float3 g = (p - xyz(c.meshMinimumSpacing)) * (1 / c.meshMinimumSpacing.w);
    float3 q = clampPosition(g, make_float3(0, 0, 0),
                             make_float3(float(c.meshDimensions.x - 1), float(c.meshDimensions.y - 1),
                                         float(c.meshDimensions.z - 1)));
    if (g.x != q.x || g.y != q.y || g.z != q.z)
        return (2 + length(g - q)) * c.meshMinimumSpacing.w;
    int3 cell =
        make_int3(min(int(q.x), int(c.meshDimensions.x) - 2), min(int(q.y), int(c.meshDimensions.y) - 2),
                  min(int(q.z), int(c.meshDimensions.z) - 2));
    float3 f = q - asFloat(cell);
    float phi = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        int3 a = make_int3(i & 1, (i >> 1) & 1, i >> 2), v = cell + a;
        float3 w = make_float3(a.x ? f.x : 1 - f.x, a.y ? f.y : 1 - f.y, a.z ? f.z : 1 - f.z);
        phi += mesh[c.meshDimensions.w + (v.z * c.meshDimensions.y + v.y) * c.meshDimensions.x + v.x] * w.x *
               w.y * w.z;
    }
    return phi + length(g - q) * c.meshMinimumSpacing.w;
}
__device__ float3 wallVelocity(Collider c, float3 p) {
    return xyz(c.velocityFriction) + cross(xyz(c.angularSlip), p - xyz(c.centerRestitution));
}
__global__ void bake(Frame f, Fields d, const Collider *colliders, const float *mesh) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    float3 p = xyz(f.minimumCell) + (asFloat(cellFromIndex(id, f)) + radiusVector(.5f)) * f.minimumCell.w;
    float4 s = make_float4(1e6f, 0, 0, 0);
    for (uint32_t i = 0; i < f.collision.x; ++i) {
        Collider c = colliders[i];
        float phi = colliderPhi(c, p, mesh);
        if (phi < s.x) {
            float3 v = wallVelocity(c, p);
            s = make_float4(phi, v.x, v.y, v.z);
        }
    }
    d.solid[id] = s;
}
__global__ void collide(Frame f, Fields d, const Collider *colliders, const float *mesh, bool conditional) {
    if (failed(d))
        return;
    if (conditional && !d.arguments[24])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    Particle p = d.particles[id];
    if (!p.velocityFlags.w)
        return;
    float3 lo = xyz(f.minimumCell) + radiusVector(p.positionRadius.w),
           hi = xyz(f.maximumRadius) - radiusVector(p.positionRadius.w);
    for (uint32_t sweep = 0; sweep < 2; ++sweep)
        for (uint32_t i = 0; i < f.collision.x; ++i) {
            Collider c = colliders[i];
            float3 position = xyz(p.positionRadius);
            float phi = colliderPhi(c, position, mesh);
            // Match collision.hlsl: roundoff after projection must not cause a
            // second application of wall slip/restitution at the same contact.
            float contactTolerance = fmaxf(1e-7f, f.minimumCell.w * 1e-5f);
            if (phi >= p.positionRadius.w - contactTolerance)
                continue;
            float e = f.minimumCell.w * .01f;
            float3 n = make_float3(0, 0, 0);
            for (int a = 0; a < 3; ++a) {
                float3 v = make_float3(0, 0, 0);
                component(v, a) = e;
                component(n, a) = colliderPhi(c, position + v, mesh) - colliderPhi(c, position - v, mesh);
            }
            n = dot(n, n) > 1e-15f ? n * (1 / sqrtf(dot(n, n))) : make_float3(0, 1, 0);
            float3 contact = position + n * (p.positionRadius.w - phi);
            bool blocked = outside(contact, lo, hi);
            for (uint32_t other = 0; other < f.collision.x && !blocked; ++other)
                blocked = colliderPhi(colliders[other], contact, mesh) < p.positionRadius.w - 1e-5f;
            if (blocked) {
                float best = 1e20f;
                for (uint32_t direction = 0; direction < 6; ++direction) {
                    float3 axis = make_float3(0, 0, 0);
                    component(axis, int(direction / 2)) = (direction & 1) ? 1.f : -1.f;
                    float travel = 0;
                    for (uint32_t step = 0; step < 24; ++step) {
                        float3 q = position + axis * travel;
                        if (outside(q, lo, hi))
                            break;
                        float distance = 1e20f;
                        for (uint32_t s = 0; s < f.collision.x; ++s)
                            distance = fminf(distance, colliderPhi(colliders[s], q, mesh));
                        float gap = p.positionRadius.w - distance;
                        if (gap < 1e-5f) {
                            if (travel < best) {
                                best = travel;
                                contact = q;
                                n = axis;
                            }
                            break;
                        }
                        travel += fmaxf(gap, 1e-5f);
                    }
                }
            }
            setXYZ(p.positionRadius, contact);
            float3 wall = wallVelocity(c, contact), relative = xyz(p.velocityFlags) - wall;
            float vn = dot(relative, n);
            if (vn < 0) {
                relative = relative - n * ((1 + saturate(c.centerRestitution.w)) * vn);
                float3 tangent = relative - n * dot(relative, n);
                relative = relative - tangent * fminf(1.f, fmaxf(0.f, c.velocityFriction.w) * (-vn) /
                                                               fmaxf(length(tangent), 1e-6f));
            }
            relative = relative + (n * dot(relative, n) - relative) * saturate(1 - c.angularSlip.w);
            setXYZ(p.velocityFlags, relative + wall);
            setXYZ(p.apic0, make_float3(0, 0, 0));
            setXYZ(p.apic1, make_float3(0, 0, 0));
            setXYZ(p.apic2, make_float3(0, 0, 0));
        }
    setXYZ(p.positionRadius, clampPosition(xyz(p.positionRadius), lo, hi));
    d.particles[id] = p;
}
} // namespace
void enqueueCollision(void *rawStream, void *const (&buffers)[BufferCount], const void *rawFrame,
                      CollisionStage stage, const void *deviceColliders, const float *deviceMesh,
                      bool conditional, const uint32_t *failure) {
    if (!rawStream || !rawFrame)
        throw std::runtime_error("Invalid CUDA collision invocation");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    if (f.collision.x > 16 || (f.collision.x && !deviceColliders) || !f.grid.x || !f.grid.y || !f.grid.z ||
        f.grid.x > 1048576 || f.grid.y > 1048576 || f.grid.z > 1048576 ||
        uint64_t(f.grid.x) * f.grid.y * f.grid.z != f.grid.w || f.grid.w > 1048576 || !f.counts.x ||
        f.counts.x > 1048576 || !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0 ||
        !buffers[Particles] || !buffers[Solid] || (conditional && !buffers[DensityArguments]))
        throw std::runtime_error("Invalid CUDA collision dimensions/buffers");
    auto stream = static_cast<cudaStream_t>(rawStream);
    Fields d = fields(buffers, 0, failure);
    auto *colliders = static_cast<const Collider *>(deviceColliders);
    if (stage == CollisionStage::BakeSolids)
        bake<<<(f.grid.w + 127) / 128, 128, 0, stream>>>(f, d, colliders, deviceMesh);
    else if (stage == CollisionStage::Collide)
        collide<<<(f.counts.x + 127) / 128, 128, 0, stream>>>(f, d, colliders, deviceMesh, conditional);
    else
        throw std::runtime_error("Unknown CUDA collision stage");
    auto error = cudaGetLastError();
    if (error != cudaSuccess)
        throw std::runtime_error(std::string("CUDA collision kernel: ") + cudaGetErrorString(error));
}
} // namespace lab::cuda_fluid
