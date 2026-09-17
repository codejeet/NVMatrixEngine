#include "fluid_cuda_grid.h"
#include "fluid_cuda_device.cuh"
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
__device__ float capillary(int3 wet, int3 air, Frame f, Fields d) {
    float4 a = d.material[cellIndex(wet, f)], b = d.material[cellIndex(air, f)];
    float t = fabsf(a.x - b.x) > 1e-6f ? saturate((a.x - .5f) / (a.x - b.x)) : .5f;
    return f.material.y * (a.y + (b.y - a.y) * t);
}
__device__ float divergenceAt(int3 c, Frame f, Fields d) {
    float sum = 0;
    for (int axis = 0; axis < 3; ++axis) {
        int3 next = c;
        component(next, axis)++;
        sum += d.faces[faceIndex(next, axis, f)].x - d.faces[faceIndex(c, axis, f)].x;
    }
    return sum / f.minimumCell.w;
}
__device__ float stencil(uint32_t id, Frame f, Fields d) {
    float4 s = d.scratch[id];
    uint32_t mask = uint32_t(s.z), strides[3]{1, f.grid.x, f.grid.x * f.grid.y};
    float sum = 0;
    for (uint32_t a = 0; a < 3; ++a) {
        if (mask & (1u << (a * 2)))
            sum += d.pressure[id - strides[a]];
        if (mask & (2u << (a * 2)))
            sum += d.pressure[id + strides[a]];
    }
    return (sum + s.x) * s.y;
}
__global__ void classify(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    d.cells[id] =
        make_float4(0, 0, solid(cellFromIndex(id, f), f, d) ? 2.f : (cellMass(id, d) > 0 ? 1.f : 0.f), 0);
    d.pressure[id] = d.output[id] = 0;
    d.density[id] = d.material[id] = make_float4(0, 0, 0, 0);
}
__global__ void forces(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= faceStride(f) * 3)
        return;
    int axis;
    int3 c;
    if (!faceCoordinate(id, f, axis, c))
        return;
    float4 v = d.faces[id];
    int3 left = c;
    component(left, axis)--;
    if (solid(left, f, d) || solid(c, f, d))
        v.x = boundaryVelocity(left, c, axis, f, d);
    else if (v.z > 1e-8f && f.counts.w != 3)
        v.x += component(f.gravityDt, axis) * f.gravityDt.w;
    d.faces[id] = v;
}
__global__ void viscosity(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= faceStride(f) * 3)
        return;
    int axis;
    int3 p;
    bool valid = faceCoordinate(id, f, axis, p);
    float4 v = d.faces[id];
    if (!valid || v.z <= 1e-8f) {
        d.scratch[id] = v;
        return;
    }
    int3 left = p;
    component(left, axis)--;
    if (solid(left, f, d) || solid(p, f, d)) {
        v.x = boundaryVelocity(left, p, axis, f, d);
        d.scratch[id] = v;
        return;
    }
    int3 extent = make_int3(f.grid.x, f.grid.y, f.grid.z);
    component(extent, axis)++;
    float lap = 0;
    for (int a = 0; a < 3; ++a)
        for (int side = -1; side <= 1; side += 2) {
            int3 n = p;
            component(n, a) += side;
            if (n.x < 0 || n.y < 0 || n.z < 0 || n.x >= extent.x || n.y >= extent.y || n.z >= extent.z)
                continue;
            int3 l = n;
            component(l, axis)--;
            float4 neighbor = d.faces[faceIndex(n, axis, f)];
            if (solid(l, f, d) || solid(n, f, d))
                lap += boundaryVelocity(l, n, axis, f, d) - v.x;
            else if (neighbor.z > 1e-8f)
                lap += neighbor.x - v.x;
        }
    v.x += f.material.x * lap;
    d.scratch[id] = v;
}
__device__ float occupancy(int3 c, Frame f, Fields d) {
    return inGrid(c, f) ? saturate(cellMass(cellIndex(c, f), d) * f.solver.w) : 0;
}
__global__ void surfaceColor(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    int3 c = cellFromIndex(id, f);
    float value = 0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                value += occupancy(c + make_int3(x, y, z), f, d) * (x == 0 ? 2.f / 3 : 1.f / 6) *
                         (y == 0 ? 2.f / 3 : 1.f / 6) * (z == 0 ? 2.f / 3 : 1.f / 6);
    if (f.material.w == 2) {
        float3 p = xyz(f.minimumCell) + (asFloat(c) + radiusVector(.5f)) * f.minimumCell.w;
        value = saturate(.5f - (length(p - (xyz(f.minimumCell) + xyz(f.maximumRadius)) * .5f) - .75f) /
                                   (6 * f.minimumCell.w));
    }
    d.density[id] = make_float4(value, 0, 0, 0);
}
__device__ float colorAt(int3 c, Frame f, Fields d) {
    return d.density[cellIndex(boundedCell(c, f), f)].x;
}
__global__ void surfaceCurvature(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    int3 p = cellFromIndex(id, f);
    float h = f.minimumCell.w, c = colorAt(p, f, d), H[3][3]{};
    float3 g = make_float3(0, 0, 0);
    bool contact = false;
    for (int a = 0; a < 3; ++a) {
        int3 e = make_int3(0, 0, 0);
        component(e, a) = 1;
        float lo = colorAt(p - e, f, d), hi = colorAt(p + e, f, d);
        component(g, a) = (hi - lo) / (2 * h);
        H[a][a] = (hi - 2 * c + lo) / (h * h);
        contact = contact || solid(p - e, f, d) || solid(p + e, f, d) || solid(p - e * 2, f, d) ||
                  solid(p + e * 2, f, d);
        for (int b = a + 1; b < 3; ++b) {
            int3 v = make_int3(0, 0, 0);
            component(v, b) = 1;
            float value = (colorAt(p + e + v, f, d) - colorAt(p + e - v, f, d) - colorAt(p - e + v, f, d) +
                           colorAt(p - e - v, f, d)) /
                          (4 * h * h);
            H[a][b] = H[b][a] = value;
        }
    }
    float gg = dot(g, g), kappa = 0;
    if (gg > 1e-6f && !contact) {
        float3 Hg = make_float3(H[0][0] * g.x + H[0][1] * g.y + H[0][2] * g.z,
                                H[1][0] * g.x + H[1][1] * g.y + H[1][2] * g.z,
                                H[2][0] * g.x + H[2][1] * g.y + H[2][2] * g.z);
        kappa = -(gg * (H[0][0] + H[1][1] + H[2][2]) - dot(g, Hg)) / powf(gg, 1.5f);
    }
    d.material[id] = make_float4(c, fminf(fmaxf(kappa, -2 / h), 2 / h), sqrtf(gg), 0);
}
__global__ void materialFixture(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= faceStride(f) * 3)
        return;
    int axis;
    int3 p;
    bool valid = faceCoordinate(id, f, axis, p);
    float v = axis == 0 ? sinf((float(p.z) + .5f) * .4f) : 0;
    d.faces[id] = valid ? make_float4(v, v, 1, 0) : make_float4(0, 0, 0, 0);
}
__global__ void divergence(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    if (d.cells[id].z != 1) {
        d.cells[id].x = 0;
        d.scratch[id] = make_float4(0, 0, 0, 0);
        return;
    }
    int3 c = cellFromIndex(id, f);
    float value = divergenceAt(c, f, d);
    d.cells[id].x = value;
    uint32_t mask = 0, diagonal = 0;
    float ghost = 0;
    for (int a = 0; a < 3; ++a)
        for (int side = -1; side <= 1; side += 2) {
            int3 n = c;
            component(n, a) += side;
            if (!solid(n, f, d)) {
                ++diagonal;
                if (liquid(n, f, d))
                    mask |= 1u << (a * 2 + (side > 0 ? 1 : 0));
                else if (f.material.y > 0)
                    ghost += capillary(c, n, f, d);
            }
        }
    float rhs = f.solver.x * f.minimumCell.w * f.minimumCell.w / f.gravityDt.w * value;
    d.scratch[id] = make_float4(ghost - rhs, diagonal ? 1.f / diagonal : 0, float(mask), 0);
}
__global__ void jacobi(Frame f, Fields d, bool conditional) {
    if (failed(d))
        return;
    if (conditional && !d.arguments[24])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < f.grid.w)
        d.output[id] = stencil(id, f, d);
}
__global__ void project(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= faceStride(f) * 3)
        return;
    int a;
    int3 r;
    if (!faceCoordinate(id, f, a, r))
        return;
    int3 l = r;
    component(l, a)--;
    float4 v = d.faces[id];
    if (solid(l, f, d) || solid(r, f, d)) {
        v.x = boundaryVelocity(l, r, a, f, d);
        v.w = 1;
    } else if (liquid(l, f, d) || liquid(r, f, d)) {
        float pl = d.pressure[cellIndex(l, f)], pr = d.pressure[cellIndex(r, f)];
        if (f.material.y > 0) {
            if (!liquid(l, f, d))
                pl = capillary(r, l, f, d);
            if (!liquid(r, f, d))
                pr = capillary(l, r, f, d);
        }
        v.x -= f.gravityDt.w / (f.solver.x * f.minimumCell.w) * (pr - pl);
        v.w = 1;
    } else
        v.w = 0;
    d.faces[id] = v;
}
__global__ void measure(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    d.cells[id].y = d.cells[id].z == 1 ? divergenceAt(cellFromIndex(id, f), f, d) : 0;
    d.cells[id].w = d.pressure[id];
}
__global__ void extrapolate(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= faceStride(f) * 3)
        return;
    int axis;
    int3 c;
    bool valid = faceCoordinate(id, f, axis, c);
    float4 v = d.faces[id];
    if (!valid || v.w > 0) {
        d.scratch[id] = v;
        return;
    }
    int3 extent = make_int3(f.grid.x, f.grid.y, f.grid.z);
    component(extent, axis)++;
    float x = 0, y = 0, count = 0;
    for (int a = 0; a < 3; ++a)
        for (int side = -1; side <= 1; side += 2) {
            int3 n = c;
            component(n, a) += side;
            if (n.x < 0 || n.y < 0 || n.z < 0 || n.x >= extent.x || n.y >= extent.y || n.z >= extent.z)
                continue;
            float4 neighbor = d.faces[faceIndex(n, axis, f)];
            if (neighbor.w > 0) {
                x += neighbor.x;
                y += neighbor.y;
                count++;
            }
        }
    if (count > 0) {
        v.x = x / count;
        v.y = y / count;
        v.w = 1;
    }
    d.scratch[id] = v;
}
__device__ float particleDensity(int3 c, Frame f, Fields d) {
    float3 center = asFloat(c) + radiusVector(.5f);
    float mass = 0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                int3 n = c + make_int3(x, y, z);
                if (!inGrid(n, f))
                    continue;
                uint32_t b = cellIndex(n, f);
                if (d.narrowGrid) {
                    const int3 base = make_int3(n.x / 2, n.y / 2, n.z / 2) * 2;
                    const uint32_t owner =
                        ((n.z / 2) * ((f.grid.y + 1) / 2) + n.y / 2) * ((f.grid.x + 1) / 2) + n.x / 2;
                    const uint32_t children = min(2, int(f.grid.x) - base.x) *
                                              min(2, int(f.grid.y) - base.y) * min(2, int(f.grid.z) - base.z);
                    const float weight = (x == 0 ? 2.f / 3 : 1.f / 6) * (y == 0 ? 2.f / 3 : 1.f / 6) *
                                         (z == 0 ? 2.f / 3 : 1.f / 6);
                    mass += float(d.narrowGrid[owner].w / (double(children) * f.initialMinimum.w)) * weight;
                }
                for (uint32_t j = d.offsets[b]; j < d.offsets[b + 1]; ++j) {
                    Particle p = d.particles[d.indices[j]];
                    float3 q = (xyz(p.positionRadius) - xyz(f.minimumCell)) * (1 / f.minimumCell.w) - center;
                    mass += quadratic(q.x) * quadratic(q.y) * quadratic(q.z) * p.apic0.w;
                }
            }
    return mass * f.solver.w;
}
__global__ void densityMeasure(Frame f, Fields d) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < f.grid.w)
        d.density[id].w = particleDensity(cellFromIndex(id, f), f, d);
}
__global__ void densityGather(Frame f, Fields d, bool adaptive) {
    if (failed(d))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    int3 c = cellFromIndex(id, f);
    float mass = particleDensity(c, f, d), solidMass = 0;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                if (solid(c + make_int3(x, y, z), f, d))
                    solidMass += (x == 0 ? 2.f / 3 : 1.f / 6) * (y == 0 ? 2.f / 3 : 1.f / 6) *
                                 (z == 0 ? 2.f / 3 : 1.f / 6);
    float rho = mass + solidMass, rhs = fminf(fmaxf(rho - 1, 0.f), .5f);
    bool active = !solid(c, f, d) && d.counts[id] > 0;
    if (!adaptive)
        d.density[id] = make_float4(mass, rhs, active ? 1.f : 0.f, 0);
    if (adaptive && active && rho > 1.02f)
        atomicOr(d.arguments + 24, 1u);
    d.pressure[id] = d.output[id] = 0;
    uint32_t mask = 0, diagonal = 0;
    if (active)
        for (int a = 0; a < 3; ++a)
            for (int side = -1; side <= 1; side += 2) {
                int3 n = c;
                component(n, a) += side;
                if (!solid(n, f, d)) {
                    ++diagonal;
                    if (d.counts[cellIndex(n, f)])
                        mask |= 1u << (a * 2 + (side > 0 ? 1 : 0));
                }
            }
    d.scratch[id] =
        make_float4(f.minimumCell.w * f.minimumCell.w * rhs, diagonal ? 1.f / diagonal : 0, float(mask), 0);
}
__device__ float displacementFace(int3 r, int a, Frame f, Fields d) {
    int3 l = r;
    component(l, a)--;
    return solid(l, f, d) || solid(r, f, d)
               ? 0
               : -(d.pressure[cellIndex(r, f)] - d.pressure[cellIndex(l, f)]) / f.minimumCell.w;
}
__global__ void densityDisplace(Frame f, Fields d, bool conditional) {
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
    float3 delta = make_float3(0, 0, 0);
    for (int a = 0; a < 3; ++a) {
        float3 gp = (xyz(p.positionRadius) - xyz(f.minimumCell)) * (1 / f.minimumCell.w) - faceOffset(a);
        int3 base = make_int3(int(floorf(gp.x - .5f)), int(floorf(gp.y - .5f)), int(floorf(gp.z - .5f)));
        for (int z = 0; z < 3; ++z)
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    int3 c = base + make_int3(x, y, z);
                    float3 q = asFloat(c) - gp;
                    component(delta, a) +=
                        quadratic(q.x) * quadratic(q.y) * quadratic(q.z) * displacementFace(c, a, f, d);
                }
    }
    delta = delta * fminf(1.f, .2f * f.minimumCell.w / fmaxf(length(delta), 1e-10f));
    setXYZ(p.positionRadius,
           clampPosition(xyz(p.positionRadius) + delta, xyz(f.minimumCell) + radiusVector(p.positionRadius.w),
                         xyz(f.maximumRadius) - radiusVector(p.positionRadius.w)));
    d.particles[id] = p;
}
__global__ void densityArguments(Frame f, Fields d, int mode) {
    if (failed(d))
        return;
    uint32_t id = threadIdx.x;
    if (mode < 2) {
        if (mode == 0 || id != 25)
            d.arguments[id] = 0;
        return;
    }
    if (id)
        return;
    uint32_t flag = d.arguments[24];
    d.arguments[25] += flag ? 1u : 0u;
    uint32_t groups[8]{
        (f.grid.w + 255) / 256, (f.counts.x + 255) / 256, (f.grid.w + 255) / 256, 1,
        (f.grid.w + 255) / 256, (f.counts.x + 255) / 256, (f.grid.w + 255) / 256, (f.counts.x + 127) / 128};
    for (uint32_t i = 0; i < 8; ++i) {
        d.arguments[3 * i] = flag ? groups[i] : 0;
        d.arguments[3 * i + 1] = d.arguments[3 * i + 2] = 1;
    }
    uint32_t tiles = ((f.grid.x + 7) / 8) * ((f.grid.y + 3) / 4) * ((f.grid.z + 3) / 4);
    d.arguments[30] = flag ? min(tiles, 65535u) : 0;
    d.arguments[31] = (tiles + 65534) / 65535;
    d.arguments[32] = 1;
}
} // namespace
void enqueueGrid(void *rawStream, void *const (&buffers)[BufferCount], const void *rawFrame, GridStage stage,
                 uint32_t pressureIndex, bool conditional, const uint32_t *failure, const void *mass,
                 const void *narrowGrid) {
    if (!rawStream || !rawFrame || pressureIndex > 1)
        throw std::runtime_error("Invalid CUDA grid invocation");
    for (void *p : buffers)
        if (!p)
            throw std::runtime_error("Missing CUDA grid field");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    if (!f.grid.x || !f.grid.y || !f.grid.z || f.grid.x > 1048576 || f.grid.y > 1048576 ||
        f.grid.z > 1048576 || uint64_t(f.grid.x) * f.grid.y * f.grid.z != f.grid.w || f.grid.w > 1048576 ||
        !f.counts.x || f.counts.x > 1048576 || !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0 ||
        !std::isfinite(f.gravityDt.w) || f.gravityDt.w <= 0 || !std::isfinite(f.solver.x) ||
        f.solver.x <= 0 || f.display.z || f.display.w != (mass ? 2u : 0u))
        throw std::runtime_error("CUDA baseline grid dimensions/ownership mismatch");
    auto stream = static_cast<cudaStream_t>(rawStream);
    Fields d = fields(buffers, pressureIndex, failure, mass);
    d.narrowGrid = static_cast<const double4 *>(narrowGrid);
    uint32_t cg = (f.grid.w + 127) / 128,
             fg = ((f.grid.x + 1) * (f.grid.y + 1) * (f.grid.z + 1) * 3 + 127) / 128;
    switch (stage) {
    case GridStage::Classify:
        classify<<<cg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::Forces:
        forces<<<fg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::Viscosity:
        viscosity<<<fg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::SurfaceColor:
        surfaceColor<<<cg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::SurfaceCurvature:
        surfaceCurvature<<<cg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::MaterialFixture:
        materialFixture<<<fg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::Divergence:
        divergence<<<cg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::Jacobi:
        jacobi<<<cg, 128, 0, stream>>>(f, d, conditional);
        break;
    case GridStage::Project:
        project<<<fg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::Measure:
        measure<<<cg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::Extrapolate:
        extrapolate<<<fg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::DensityGather:
        densityGather<<<cg, 128, 0, stream>>>(f, d, false);
        break;
    case GridStage::DensityGatherAdaptive:
        densityGather<<<cg, 128, 0, stream>>>(f, d, true);
        break;
    case GridStage::DensityDisplace:
        densityDisplace<<<(f.counts.x + 127) / 128, 128, 0, stream>>>(f, d, conditional);
        break;
    case GridStage::DensityMeasure:
        densityMeasure<<<cg, 128, 0, stream>>>(f, d);
        break;
    case GridStage::DensityClearArguments:
        densityArguments<<<1, 64, 0, stream>>>(f, d, 0);
        break;
    case GridStage::DensityContinueArguments:
        densityArguments<<<1, 64, 0, stream>>>(f, d, 1);
        break;
    case GridStage::DensityPrepareArguments:
        densityArguments<<<1, 64, 0, stream>>>(f, d, 2);
        break;
    default:
        throw std::runtime_error("Unknown CUDA grid stage");
    }
    auto error = cudaGetLastError();
    if (error != cudaSuccess)
        throw std::runtime_error(std::string("CUDA grid kernel: ") + cudaGetErrorString(error));
}
} // namespace lab::cuda_fluid
