#pragma once
#include "fluid_cuda_kernels.h"
#include <cuda_runtime.h>
namespace lab::cuda_fluid::detail {
// CUDA-local mirror of FluidSimulationConstants/FluidParticle. No DirectXMath
// dependency in device code. Cross-backend fixtures consume the real host ABI.
struct Frame {
    float4 minimumCell, maximumRadius, gravityDt;
    uint4 counts;
    float4 viewProjection[4];
    float4 cameraRight, cameraUp, cameraPosition, cameraForward;
    uint4 grid;
    float4 solver;
    uint4 display, collision;
    float4 material, initialMinimum, initialMaximum;
    uint4 initialLattice, emission;
    float4 emitterOriginRadius, emitterVelocity;
};
struct Particle {
    float4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
static_assert(sizeof(Frame) == 368 && sizeof(Particle) == 80);
__device__ inline float3 xyz(float4 a) {
    return make_float3(a.x, a.y, a.z);
}
__device__ inline float &component(float3 &v, int a) {
    return a == 0 ? v.x : (a == 1 ? v.y : v.z);
}
__device__ inline float component(float4 v, int a) {
    return a == 0 ? v.x : (a == 1 ? v.y : v.z);
}
__device__ inline int &component(int3 &v, int a) {
    return a == 0 ? v.x : (a == 1 ? v.y : v.z);
}
__device__ inline float3 operator+(float3 a, float3 b) {
    return make_float3(a.x + b.x, a.y + b.y, a.z + b.z);
}
__device__ inline float3 operator-(float3 a, float3 b) {
    return make_float3(a.x - b.x, a.y - b.y, a.z - b.z);
}
__device__ inline float3 operator*(float3 a, float b) {
    return make_float3(a.x * b, a.y * b, a.z * b);
}
__device__ inline float dot(float3 a, float3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
__device__ inline void setXYZ(float4 &v, float3 a) {
    v.x = a.x;
    v.y = a.y;
    v.z = a.z;
}
__device__ inline float quadratic(float x) {
    x = fabsf(x);
    return x < .5f ? .75f - x * x : (x < 1.5f ? .5f * (1.5f - x) * (1.5f - x) : 0.f);
}
__device__ inline uint32_t cellIndex(int3 p, const Frame &f) {
    return (p.z * f.grid.y + p.y) * f.grid.x + p.x;
}
__device__ inline int3 cellCoord(float3 p, const Frame &f) {
    float3 q = (p - xyz(f.minimumCell)) * (1.f / f.minimumCell.w);
    return make_int3(min(max(int(fmaxf(q.x, 0)), 0), int(f.grid.x) - 1),
                     min(max(int(fmaxf(q.y, 0)), 0), int(f.grid.y) - 1),
                     min(max(int(fmaxf(q.z, 0)), 0), int(f.grid.z) - 1));
}
__device__ inline uint32_t faceStride(const Frame &f) {
    return (f.grid.x + 1) * (f.grid.y + 1) * (f.grid.z + 1);
}
__device__ inline uint32_t faceIndex(int3 p, int axis, const Frame &f) {
    return axis * faceStride(f) + (p.z * (f.grid.y + 1) + p.y) * (f.grid.x + 1) + p.x;
}
__device__ inline float3 faceOffset(int axis) {
    float3 p = make_float3(.5f, .5f, .5f);
    component(p, axis) = 0;
    return p;
}

__device__ inline int3 operator+(int3 a, int3 b) {
    return make_int3(a.x + b.x, a.y + b.y, a.z + b.z);
}
__device__ inline int3 operator-(int3 a, int3 b) {
    return make_int3(a.x - b.x, a.y - b.y, a.z - b.z);
}
__device__ inline int3 operator*(int3 a, int b) {
    return make_int3(a.x * b, a.y * b, a.z * b);
}
__device__ inline int3 cellFromIndex(uint32_t id, const Frame &f) {
    return make_int3(id % f.grid.x, (id / f.grid.x) % f.grid.y, id / (f.grid.x * f.grid.y));
}
__device__ inline bool inGrid(int3 c, const Frame &f) {
    return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < int(f.grid.x) && c.y < int(f.grid.y) &&
           c.z < int(f.grid.z);
}
__device__ inline int3 boundedCell(int3 c, const Frame &f) {
    return make_int3(min(max(c.x, 0), int(f.grid.x) - 1), min(max(c.y, 0), int(f.grid.y) - 1),
                     min(max(c.z, 0), int(f.grid.z) - 1));
}
__device__ inline float3 asFloat(int3 p) {
    return make_float3(float(p.x), float(p.y), float(p.z));
}
__device__ inline float3 cross(float3 a, float3 b) {
    return make_float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
__device__ inline float length(float3 a) {
    return sqrtf(dot(a, a));
}
__device__ inline float saturate(float a) {
    return fminf(fmaxf(a, 0.f), 1.f);
}
__device__ inline float3 clampPosition(float3 p, float3 lo, float3 hi) {
    return make_float3(fminf(fmaxf(p.x, lo.x), hi.x), fminf(fmaxf(p.y, lo.y), hi.y),
                       fminf(fmaxf(p.z, lo.z), hi.z));
}
__device__ inline float3 radiusVector(float r) {
    return make_float3(r, r, r);
}
__device__ inline bool outside(float3 p, float3 lo, float3 hi) {
    return p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z;
}
struct Collider {
    float4 worldToLocal[4], centerRestitution, extentType, velocityFriction, angularSlip, meshMinimumSpacing;
    uint4 meshDimensions;
};
static_assert(sizeof(Collider) == 160);
struct Fields {
    Particle *particles;
    uint32_t *counts, *offsets, *cursors, *indices;
    float4 *faces;
    float *pressure, *output;
    float4 *cells, *scratch, *density, *solid, *material;
    uint32_t *arguments;
    const uint32_t *failure = nullptr;
    const float *cellMass = nullptr;
    const double4 *narrowGrid = nullptr;
};
inline Fields fields(void *const (&p)[BufferCount], uint32_t pressureIndex = 0,
                     const uint32_t *failure = nullptr, const void *cellMass = nullptr) {
    return {static_cast<Particle *>(p[Particles]),
            static_cast<uint32_t *>(p[Counts]),
            static_cast<uint32_t *>(p[Offsets]),
            static_cast<uint32_t *>(p[Cursors]),
            static_cast<uint32_t *>(p[Indices]),
            static_cast<float4 *>(p[Faces]),
            static_cast<float *>(p[pressureIndex ? Pressure1 : Pressure0]),
            static_cast<float *>(p[pressureIndex ? Pressure0 : Pressure1]),
            static_cast<float4 *>(p[Cells]),
            static_cast<float4 *>(p[Scratch]),
            static_cast<float4 *>(p[Density]),
            static_cast<float4 *>(p[Solid]),
            static_cast<float4 *>(p[Material]),
            static_cast<uint32_t *>(p[DensityArguments]),
            failure,
            static_cast<const float *>(cellMass)};
}
__device__ inline float cellMass(uint32_t id, const Fields &d) {
    return d.cellMass ? d.cellMass[id] : float(d.counts[id]);
}
__device__ inline bool failed(Fields d) {
    return d.failure && *d.failure;
}
__device__ inline bool solid(int3 c, const Frame &f, const Fields &d) {
    return !inGrid(c, f) || d.solid[cellIndex(c, f)].x < 0;
}
__device__ inline bool liquid(int3 c, const Frame &f, const Fields &d) {
    return inGrid(c, f) && d.cells[cellIndex(c, f)].z == 1;
}
__device__ inline float boundaryVelocity(int3 l, int3 r, int axis, const Frame &f, const Fields &d) {
    if (!inGrid(l, f) || !inGrid(r, f))
        return 0;
    float4 v = d.solid[cellIndex(solid(l, f, d) ? l : r, f)];
    return axis == 0 ? v.y : (axis == 1 ? v.z : v.w);
}
__device__ inline bool faceCoordinate(uint32_t id, const Frame &f, int &axis, int3 &c) {
    const uint32_t stride = faceStride(f), k = id % stride;
    axis = int(id / stride);
    c = make_int3(k % (f.grid.x + 1), (k / (f.grid.x + 1)) % (f.grid.y + 1),
                  k / ((f.grid.x + 1) * (f.grid.y + 1)));
    int3 extent = make_int3(f.grid.x, f.grid.y, f.grid.z);
    component(extent, axis)++;
    return c.x < extent.x && c.y < extent.y && c.z < extent.z;
}

} // namespace lab::cuda_fluid::detail
