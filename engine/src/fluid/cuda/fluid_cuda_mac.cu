#include "fluid_cuda_mac.h"
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
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
__device__ float capillary(int3 wet, int3 air, Frame f, Fields d) {
    float4 a = d.material[cellIndex(wet, f)], b = d.material[cellIndex(air, f)];
    float t = fabsf(a.x - b.x) > 1e-6f ? saturate((a.x - .5f) / (a.x - b.x)) : .5f;
    return f.material.y * (a.y + (b.y - a.y) * t);
}
__global__ void clear(MacView v) {
    if (threadIdx.x || blockIdx.x || v.pool.control[BrickInvalid])
        return;
    v.pool.control[BrickCandidate] = 1 - v.pool.control[BrickFront];
    for (uint32_t i = 0; i <= MacInvalid; ++i)
        v.counters[i] = 0;
    ++v.counters[MacSolves];
}
// Evaluate each fine cell once, not once for every overlapping 6^3 halo.
// A 3^3 OR of these 2^3 blocks is exactly the original guard, including odd
// domain extents and velocity-dependent moving-solid padding.
__global__ void refinementGuards(Frame f, Fields d, MacView v, MacConfig cfg) {
    if (v.pool.control[BrickInvalid])
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.coarseCount)
        return;
    const int3 base = coarseCoord(id, v) * 2;
    uint32_t flags = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        const int3 p = base + make_int3(i & 1, (i >> 1) & 1, i >> 2);
        if (!liquid(p, f, d) || solid(p, f, d))
            flags |= RefinementSurfaceGuard;
        if (inGrid(p, f) && cfg.predictionSeconds > 0) {
            const float4 s = d.solid[cellIndex(p, f)];
            const float speed = sqrtf(s.y * s.y + s.z * s.z + s.w * s.w);
            if (speed > 0 && s.x < cfg.predictionSeconds * speed)
                flags |= RefinementMovingSolid;
        }
    }
    v.refinementGuards[id] = flags;
}
__global__ void measureRefinement(Frame f, Fields d, MacView v, MacConfig cfg) {
    if (v.pool.control[BrickInvalid])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.coarseCount)
        return;
    if (cfg.forcedFine) {
        RefinementState next{};
        next.flags = RefinementValid | RefinementSurfaceGuard;
        v.nextRefinement[id] = next;
        return;
    }
    int3 base = coarseCoord(id, v) * 2;
    RefinementState next{};
    next.flags = RefinementValid;
    const int3 center = coarseCoord(id, v);
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                const int3 p = center + make_int3(x, y, z);
                if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= int(v.cx) || p.y >= int(v.cy) || p.z >= int(v.cz))
                    next.flags |= RefinementSurfaceGuard;
                else
                    next.flags |= v.refinementGuards[coarseIndex(p, v)];
            }
    float3 lo = radiusVector(1e20f), hi = radiusVector(-1e20f);
    float3 mean{};
    uint32_t samples = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        int3 p = base + make_int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        if (!inGrid(p, f))
            continue;
        ++samples;
        if (liquid(p, f, d))
            next.liquidFraction += saturate(cellMass(cellIndex(p, f), d) * f.solver.w);
        for (int a = 0; a < 3; ++a) {
            int3 q = p;
            component(q, a)++;
            float vel = .5f * (d.faces[faceIndex(p, a, f)].x + d.faces[faceIndex(q, a, f)].x);
            component(mean, a) += vel;
            component(lo, a) = fminf(component(lo, a), vel);
            component(hi, a) = fmaxf(component(hi, a), vel);
        }
    }
    mean = mean * (1.f / samples);
    next.vx = mean.x;
    next.vy = mean.y;
    next.vz = mean.z;
    next.liquidFraction /= samples;
    const auto policy = cfg.refinement;
    next.importance = next.liquidFraction > 0 ? length(hi - lo) / policy.velocityDifference : 0;
    // Semi-Lagrangian history, sampled at coarse-cell centers. Unknown history
    // does not invent a velocity error; current safety guards still apply.
    const float3 origin = asFloat(coarseCoord(id, v)) - mean * (f.gravityDt.w / (2 * f.minimumCell.w));
    float3 predicted{};
    float fraction = 0, filtered = 0, wake = 0, validWeight = 0;
    if (isfinite(origin.x) && isfinite(origin.y) && isfinite(origin.z) && origin.x > -1 && origin.y > -1 &&
        origin.z > -1 && origin.x < v.cx && origin.y < v.cy && origin.z < v.cz) {
        const int3 first = make_int3(int(floorf(origin.x)), int(floorf(origin.y)), int(floorf(origin.z)));
        const float3 blend = origin - asFloat(first);
        for (uint32_t i = 0; i < 8; ++i) {
            int3 side = make_int3(i & 1, (i >> 1) & 1, (i >> 2) & 1), p = first + side;
            const float w = (side.x ? blend.x : 1 - blend.x) * (side.y ? blend.y : 1 - blend.y) *
                            (side.z ? blend.z : 1 - blend.z);
            if (w <= 0 || p.x < 0 || p.y < 0 || p.z < 0 || p.x >= int(v.cx) || p.y >= int(v.cy) ||
                p.z >= int(v.cz))
                continue;
            const auto previous = v.previousRefinement[(p.z * v.cy + p.y) * v.cx + p.x];
            if (!(previous.flags & RefinementValid))
                continue;
            validWeight += w;
            predicted = predicted + make_float3(previous.vx, previous.vy, previous.vz) * w;
            fraction += w * previous.liquidFraction;
            filtered += w * previous.filteredImportance;
            wake += w * previous.wakeSeconds;
        }
    }
    if (validWeight >= .9999f) {
        // Only compare velocity where liquid existed in both states. Air has
        // no transported water velocity and does not receive a gravity impulse;
        // occupancy error below still detects newly wet or vacated cells.
        if (next.liquidFraction > 0 && fraction > 0) {
            predicted = predicted + xyz(f.gravityDt) * (f.counts.w == 3 ? 0.f : f.gravityDt.w);
            next.velocityError = length(mean - predicted) / policy.velocityScale;
        }
        next.fractionError = fabsf(next.liquidFraction - fraction);
        next.importance = fmaxf(next.importance, fmaxf(next.velocityError / policy.relativeVelocityError,
                                                       next.fractionError / policy.fractionError));
    }
    const float response = -expm1f(-f.gravityDt.w / policy.responseSeconds);
    next.filteredImportance = filtered + (next.importance - filtered) * response;
    const bool disturbance =
        next.importance >= policy.promoteThreshold || (next.flags & RefinementMovingSolid);
    next.wakeSeconds = disturbance ? policy.wakeSeconds : fmaxf(0.f, wake - f.gravityDt.w);
    if (!isfinite(next.importance) || !isfinite(next.filteredImportance) || !isfinite(mean.x) ||
        !isfinite(mean.y) || !isfinite(mean.z)) {
        atomicExch(v.pool.control + BrickInvalid, 1u);
        return;
    }
    v.nextRefinement[id] = next;
}
__global__ void classify(Frame f, MacView v, MacConfig cfg) {
    if (v.pool.control[BrickInvalid])
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.coarseCount)
        return;
    const uint32_t old = v.history[id];
    const auto local = v.nextRefinement[id];
    const auto policy = cfg.refinement;
    const float threshold = old & 1 ? policy.promoteThreshold : policy.demoteThreshold;
    bool eligible = !cfg.forcedFine && !(local.flags & (RefinementSurfaceGuard | RefinementMovingSolid)) &&
                    fmaxf(local.importance, local.filteredImportance) < threshold;
    bool padded = false;
    // Pad freshly measured disturbances, not previously padded decisions; this
    // cannot recursively flood the domain by one brick on every substep.
    const auto center = coarseCoord(id, v);
    for (int z = -1; z <= 1 && eligible; ++z)
        for (int y = -1; y <= 1 && eligible; ++y)
            for (int x = -1; x <= 1 && eligible; ++x) {
                int3 p = center + make_int3(x, y, z);
                if (p.x < 0 || p.y < 0 || p.z < 0 || p.x >= int(v.cx) || p.y >= int(v.cy) || p.z >= int(v.cz))
                    continue;
                const auto &neighbor = v.nextRefinement[(p.z * v.cy + p.y) * v.cx + p.x];
                if (neighbor.importance >= policy.promoteThreshold ||
                    (neighbor.flags & RefinementMovingSolid)) {
                    eligible = false;
                    padded = true;
                }
            }
    const bool wake = local.wakeSeconds > f.gravityDt.w;
    eligible = eligible && !wake;
    const double quiet = eligible ? v.previousRefinement[id].quietSeconds + double(f.gravityDt.w) : 0;
    const uint32_t age = eligible ? min((old >> 8) + 1, 255u) : 0;
    const uint32_t next = (age << 8) | (eligible && ((old & 1) || quiet >= policy.quietSeconds) ? 1u : 0u);
    // Only these two fields are mutable in this phase; neighbor readers above
    // consume the immutable measured importance/flags, never this decision.
    v.nextRefinement[id].quietSeconds = quiet;
    v.nextRefinement[id].history = next;
    v.state[candidate(v)][id] = next;
    if ((old & 1) && !(next & 1))
        atomicAdd(v.counters + MacPromotions, 1u);
    if (!(old & 1) && (next & 1))
        atomicAdd(v.counters + MacDemotions, 1u);
    if (wake)
        atomicAdd(v.counters + MacWakeRefinements, 1u);
    if (local.velocityError >= policy.relativeVelocityError || local.fractionError >= policy.fractionError)
        atomicAdd(v.counters + MacTemporalRefinements, 1u);
    if (padded)
        atomicAdd(v.counters + MacPaddedRefinements, 1u);
}
__global__ void publishRefinement(Frame f, Fields d, MacView v) {
    if (v.pool.control[BrickInvalid])
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < v.coarseCount) {
        auto next = v.nextRefinement[id];
        // Compare next substep's forced velocity to this successfully projected
        // field. Storing the pre-projection velocity here would count gravity
        // twice and permanently refine an otherwise stationary hydrostatic pool.
        float3 velocity{};
        uint32_t samples = 0;
        const auto base = coarseCoord(id, v) * 2;
        for (uint32_t i = 0; i < 8; ++i) {
            const int3 p = base + make_int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            if (!inGrid(p, f))
                continue;
            ++samples;
            for (int a = 0; a < 3; ++a) {
                int3 q = p;
                component(q, a)++;
                component(velocity, a) +=
                    .5f * (d.faces[faceIndex(p, a, f)].x + d.faces[faceIndex(q, a, f)].x);
            }
        }
        next.vx = velocity.x / samples;
        next.vy = velocity.y / samples;
        next.vz = velocity.z / samples;
        // A capacity-deferred solve actually used fine faces, not the staged
        // coarse request. Do not give that request the relaxed retention threshold.
        if (!v.pool.control[BrickReady])
            next.history &= ~1u;
        v.previousRefinement[id] = next;
        v.history[id] = next.history;
    }
}
__global__ void leaves(Frame f, Fields d, MacView v) {
    if (v.pool.control[BrickInvalid])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    int3 p = cellFromIndex(id, f);
    uint32_t w = width(p, f, v);
    uint32_t representative = w == 2 ? cellIndex(half(p) * 2, f) : id;
    v.map[candidate(v)][id] = representative;
    if (d.cells[id].z != 1 || id != representative)
        return;
    uint32_t slot = atomicAdd(v.counters + MacLeaves, 1u);
    v.list[candidate(v)][slot] = id;
    if (w == 2)
        atomicAdd(v.counters + MacCoarse, 1u);
    atomicExch(v.pool.requests + brickKey(p, v.pool), 1u);
}
__global__ void restrictFaces(Frame f, Fields d, MacView v) {
    if (!mixed(v))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= 3 * faceStride(f))
        return;
    int a;
    int3 p;
    if (!faceCoordinate(id, f, a, p))
        return;
    int3 l = p;
    component(l, a)--;
    if (inGrid(l, f) && inGrid(p, f) && leaf(l, f, v) == leaf(p, f, v))
        return;
    uint32_t w;
    float distance;
    int3 base = patch(p, a, f, v, w, distance);
    if (p.x != base.x || p.y != base.y || p.z != base.z)
        return;
    float value = 0;
    double preciseValue = 0;
    for (uint32_t j = 0; j < w; ++j)
        for (uint32_t i = 0; i < w; ++i) {
            int3 q = base;
            component(q, (a + 1) % 3) += i;
            component(q, (a + 2) % 3) += j;
            value += d.faces[faceIndex(q, a, f)].x;
            if (v.precise)
                preciseValue += double(d.faces[faceIndex(q, a, f)].x);
        }
    d.scratch[id] = make_float4(value / (w * w), 0, 0, 0);
    if (v.precise) {
        preciseValue /= w * w;
        d.scratch[id].y = __int_as_float(__double2loint(preciseValue));
        d.scratch[id].z = __int_as_float(__double2hiint(preciseValue));
    }
    if (w == 2 && distance == 1.5f)
        atomicAdd(v.counters + MacJunctions, 1u);
}
__device__ void add(Row &r, uint32_t self, uint32_t other, float value, MacView v) {
    if (self == other) {
        r.diagonal += value;
        return;
    }
    for (uint32_t i = 0; i < r.count; ++i)
        if (r.neighbor[i] == other) {
            r.coefficient[i] += value;
            return;
        }
    if (r.count == 24) {
        atomicExch(v.counters + MacInvalid, 1u);
        return;
    }
    r.neighbor[r.count] = other;
    r.coefficient[r.count++] = value;
}
__global__ void assemble(Frame f, Fields d, MacView v) {
    if (!ready(v))
        return;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < v.counters[MacLeaves];
         i += gridDim.x * blockDim.x) {
        uint32_t id = v.list[candidate(v)][i];
        int3 c = cellFromIndex(id, f);
        Row row{};
        const uint32_t size = width(c, f, v);
        row.volumeUnits = float(size * size * size);
        if (v.precise && !v.pressureCacheState[PressureCacheDirty]) {
            // Pages are mirrored, fields are not. Read the accepted front row,
            // never the two-substep-old candidate allocation. Exact topology
            // comparison also guarantees this page was previously resident.
            const uint32_t front = v.pool.control[BrickFront];
            const uint32_t slot = v.pool.pages[front][brickKey(c, v.pool)];
            row = static_cast<const Cell *>(
                      v.pool.fields[front])[slot * 64 + (c.z & 3) * 16 + (c.y & 3) * 4 + (c.x & 3)]
                      .row;
            row.rhs = 0; // Velocity and capillary RHS is always rebuilt below.
        } else if (!mixed(v)) {
            // Preserve the scalar baseline's exact neighbor and arithmetic order.
            const float4 s = d.scratch[id];
            const uint32_t mask = uint32_t(s.z);
            const uint32_t strides[]{1, f.grid.x, f.grid.x * f.grid.y};
            row.rhs = s.x;
            row.step = s.y;
            row.diagonal = s.y ? 1 / s.y : 0;
            for (uint32_t a = 0; a < 3; ++a)
                for (uint32_t side = 0; side < 2; ++side)
                    if (mask & (1u << (a * 2 + side)))
                        add(row, id, side ? id + strides[a] : id - strides[a], -1, v);
            row.boundaryDiagonal = row.diagonal - row.count;
        } else {
            const float scale = f.solver.x * f.minimumCell.w / f.gravityDt.w;
            for (int a = 0; a < 3; ++a)
                for (uint32_t side = 0; side < 2; ++side) {
                    int3 p = c;
                    component(p, a) += side * size;
                    int3 l = p;
                    component(l, a)--;
                    uint32_t w;
                    float distance;
                    int3 base = patch(p, a, f, v, w, distance);
                    float signArea = (side ? 1.f : -1.f) * float(size * size);
                    row.rhs -= signArea * d.scratch[faceIndex(base, a, f)].x * scale;
                    if (solid(l, f, d) || solid(p, f, d))
                        continue;
                    float factor = signArea / (w * w * distance);
                    for (uint32_t j = 0; j < w; ++j)
                        for (uint32_t k = 0; k < w; ++k) {
                            int3 q = base;
                            component(q, (a + 1) % 3) += k;
                            component(q, (a + 2) % 3) += j;
                            for (uint32_t lr = 0; lr < 2; ++lr) {
                                int3 n = q;
                                component(n, a) -= 1 - lr;
                                float value = (lr ? -1.f : 1.f) * factor;
                                if (liquid(n, f, d))
                                    add(row, id, leaf(n, f, v), value, v);
                                else {
                                    row.boundaryDiagonal -= value;
                                    if (f.material.y > 0)
                                        row.rhs -= value * capillary(c, n, f, d);
                                }
                            }
                        }
                }
            float norm = row.diagonal;
            for (uint32_t k = 0; k < row.count; ++k)
                norm += fabsf(row.coefficient[k]);
            row.step = norm > 0 ? 1.8f / norm : 0;
        }
        if (!isfinite(row.rhs) || !isfinite(row.diagonal) || row.diagonal < 0 || !isfinite(row.step) ||
            row.step < 0 || !isfinite(row.boundaryDiagonal))
            atomicExch(v.counters + MacInvalid, 1u);
        for (uint32_t j = 0; j < row.count; ++j)
            if (!isfinite(row.coefficient[j]))
                atomicExch(v.counters + MacInvalid, 1u);
        auto &cell = stored(id, f, v);
        cell.row = row;
        cell.pressure[0] = cell.pressure[1] = 0;
        cell.precisePressure = 0;
        cell.preciseRhs = row.rhs;
        if (v.precise) {
            double rhs = 0, scale = double(f.solver.x) * f.minimumCell.w / f.gravityDt.w;
            for (int a = 0; a < 3; ++a)
                for (uint32_t side = 0; side < 2; ++side) {
                    int3 p = c;
                    component(p, a) += side * size;
                    int3 l = p;
                    component(l, a)--;
                    uint32_t w;
                    float distance;
                    int3 base = patch(p, a, f, v, w, distance);
                    const double area = (side ? 1. : -1.) * size * size;
                    const double velocity = mixed(v) ? restrictedVelocity(d.scratch[faceIndex(base, a, f)])
                                                     : double(d.faces[faceIndex(p, a, f)].x);
                    rhs -= area * velocity * scale;
                    if (solid(l, f, d) || solid(p, f, d) || f.material.y <= 0)
                        continue;
                    for (uint32_t j = 0; j < w; ++j)
                        for (uint32_t k = 0; k < w; ++k) {
                            int3 q = base;
                            component(q, (a + 1) % 3) += k;
                            component(q, (a + 2) % 3) += j;
                            for (uint32_t lr = 0; lr < 2; ++lr) {
                                int3 n = q;
                                component(n, a) -= 1 - lr;
                                if (!liquid(n, f, d))
                                    rhs -= (lr ? -1. : 1.) * area * double(capillary(c, n, f, d)) /
                                           (w * w * double(distance));
                            }
                        }
                }
            cell.preciseRhs = rhs;
            cell.row.rhs = float(rhs);
            if (!isfinite(rhs))
                atomicExch(v.counters + MacInvalid, 1u);
        }
    }
}
__global__ void validateAssembly(MacView v) {
    if (threadIdx.x || blockIdx.x || v.pool.control[BrickInvalid])
        return;
    // Malformed physical operators are fatal, not an excuse to hide the error
    // behind a different solve. They are checked by the deferred host audit.
    if (v.counters[MacInvalid])
        v.pool.control[BrickInvalid] = 1;
    if (!ready(v) && !v.pool.control[BrickInvalid])
        ++v.counters[MacFallbacks];
    else
        atomicMax(v.counters + MacCoarsePeak, v.counters[MacCoarse]);
}
__global__ void assembleFallback(Frame f, Fields d, MacView v) {
    if (!v.precise || v.pool.control[BrickReady] || v.pool.control[BrickInvalid])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    double rhs = 0;
    if (d.cells[id].z == 1) {
        const int3 c = cellFromIndex(id, f);
        const double scale = double(f.solver.x) * f.minimumCell.w / f.gravityDt.w;
        for (int a = 0; a < 3; ++a)
            for (uint32_t side = 0; side < 2; ++side) {
                int3 face = c, neighbor = c;
                component(face, a) += side;
                component(neighbor, a) += side ? 1 : -1;
                rhs -= (side ? 1. : -1.) * double(d.faces[faceIndex(face, a, f)].x) * scale;
                if (!solid(neighbor, f, d) && !liquid(neighbor, f, d) && f.material.y > 0)
                    rhs += double(capillary(c, neighbor, f, d));
            }
    }
    preciseRhs(id, f, v) = rhs;
    precisePressure(id, f, v) = 0;
    if (!isfinite(rhs))
        atomicExch(v.counters + MacInvalid, 1u);
}
__global__ void smooth(Frame f, Fields d, MacView v, uint32_t pi) {
    // Restriction reuses the fine-stencil scratch. A malformed mixed operator
    // cannot safely fall back after that overwrite; poison it without writes.
    if (v.pool.control[BrickInvalid])
        return;
    if (v.precise)
        return; // Both resident and capacity-deferred solves use MGPCG.
    uint32_t count = ready(v) ? v.counters[MacLeaves] : f.grid.w;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += gridDim.x * blockDim.x) {
        if (!ready(v)) {
            float4 s = d.scratch[i];
            uint32_t mask = uint32_t(s.z), stride[]{1, f.grid.x, f.grid.x * f.grid.y};
            float sum = 0;
            for (uint32_t a = 0; a < 3; ++a) {
                if (mask & (1u << (a * 2)))
                    sum += d.pressure[i - stride[a]];
                if (mask & (2u << (a * 2)))
                    sum += d.pressure[i + stride[a]];
            }
            d.output[i] = (sum + s.x) * s.y;
        } else {
            uint32_t id = v.list[candidate(v)][i];
            auto &cell = stored(id, f, v);
            const auto &r = cell.row;
            float value;
            if (!mixed(v)) {
                float sum = 0;
                for (uint32_t j = 0; j < r.count; ++j)
                    sum += stored(r.neighbor[j], f, v).pressure[pi];
                value = (sum + r.rhs) * r.step;
            } else {
                float ap = r.diagonal * cell.pressure[pi];
                for (uint32_t j = 0; j < r.count; ++j)
                    ap += r.coefficient[j] * stored(r.neighbor[j], f, v).pressure[pi];
                value = cell.pressure[pi] + (r.rhs - ap) * r.step;
            }
            cell.pressure[1 - pi] = value;
        }
    }
}
__global__ void validateSolution(Frame f, Fields d, MacView v, uint32_t pi) {
    if (v.pool.control[BrickInvalid])
        return;
    const uint32_t count = ready(v) ? v.counters[MacLeaves] : f.grid.w;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += gridDim.x * blockDim.x) {
        double value = 0;
        if (ready(v)) {
            const auto &s = stored(v.list[candidate(v)][i], f, v);
            value = v.precise ? s.precisePressure : double(s.pressure[pi]);
        } else
            value = v.precise ? precisePressure(i, f, v) : double(d.pressure[i]);
        if (!isfinite(value))
            atomicExch(v.counters + MacInvalid, 1u);
    }
}
__global__ void rejectInvalid(MacView v) {
    if (!blockIdx.x && !threadIdx.x && v.counters[MacInvalid])
        v.pool.control[BrickInvalid] = 1;
}
__global__ void exportPressure(Frame f, Fields d, MacView v) {
    if (v.pool.control[BrickInvalid] || (!v.precise && !ready(v)))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    // The caller supplies physical bank zero, regardless of the iteration parity.
    float a = 0, b = 0;
    if (solveActive(id, f, d, v)) {
        if (v.precise)
            a = b = float(precisePressure(id, f, v));
        else {
            const auto &c = stored(id, f, v);
            a = c.pressure[0];
            b = c.pressure[1];
        }
    }
    d.pressure[id] = a;
    d.output[id] = b;
}
__global__ void project(Frame f, Fields d, MacView v) {
    if (v.pool.control[BrickInvalid])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= 3 * faceStride(f))
        return;
    int a;
    int3 p;
    if (!faceCoordinate(id, f, a, p))
        return;
    int3 l = p;
    component(l, a)--;
    const bool m = mixed(v);
    if (m && inGrid(l, f) && inGrid(p, f) && leaf(l, f, v) == leaf(p, f, v))
        return;
    float4 value = d.faces[id];
    if (solid(l, f, d) || solid(p, f, d)) {
        value.x = boundaryVelocity(l, p, a, f, d);
        value.w = 1;
    } else if (liquid(l, f, d) || liquid(p, f, d)) {
        if (v.precise) {
            uint32_t w;
            float distance;
            int3 base = solvePatch(p, a, f, v, w, distance);
            double dp = 0;
            for (uint32_t j = 0; j < w; ++j)
                for (uint32_t i = 0; i < w; ++i) {
                    int3 q = base;
                    component(q, (a + 1) % 3) += i;
                    component(q, (a + 2) % 3) += j;
                    int3 n = q;
                    component(n, a)--;
                    const double pl = liquid(n, f, d)
                                          ? precisePressure(solveLeaf(n, f, v), f, v)
                                          : (f.material.y > 0 ? double(capillary(q, n, f, d)) : 0.);
                    const double pr = liquid(q, f, d)
                                          ? precisePressure(solveLeaf(q, f, v), f, v)
                                          : (f.material.y > 0 ? double(capillary(n, q, f, d)) : 0.);
                    dp += pr - pl;
                }
            const double original =
                m ? restrictedVelocity(d.scratch[faceIndex(base, a, f)]) : double(value.x);
            value.x = float(original - double(f.gravityDt.w) / (double(f.solver.x) * f.minimumCell.w) * dp /
                                           (w * w * double(distance)));
        } else if (!m) {
            float pl = d.pressure[cellIndex(l, f)], pr = d.pressure[cellIndex(p, f)];
            if (f.material.y > 0) {
                if (!liquid(l, f, d))
                    pl = capillary(p, l, f, d);
                if (!liquid(p, f, d))
                    pr = capillary(l, p, f, d);
            }
            value.x -= f.gravityDt.w / (f.solver.x * f.minimumCell.w) * (pr - pl);
        } else {
            uint32_t w;
            float distance;
            int3 base = patch(p, a, f, v, w, distance);
            float dp = 0;
            for (uint32_t j = 0; j < w; ++j)
                for (uint32_t i = 0; i < w; ++i) {
                    int3 q = base;
                    component(q, (a + 1) % 3) += i;
                    component(q, (a + 2) % 3) += j;
                    for (uint32_t lr = 0; lr < 2; ++lr) {
                        int3 n = q;
                        component(n, a) -= 1 - lr;
                        float pressure = liquid(n, f, d) ? d.pressure[leaf(n, f, v)] : 0;
                        if (!liquid(n, f, d) && f.material.y > 0)
                            pressure = capillary(liquid(l, f, d) ? l : p, n, f, d);
                        dp += (lr ? 1.f : -1.f) * pressure;
                    }
                }
            value.x = d.scratch[faceIndex(base, a, f)].x -
                      f.gravityDt.w / (f.solver.x * f.minimumCell.w) * dp / (w * w * distance);
        }
        value.w = 1;
    } else
        value.w = 0;
    d.faces[id] = value;
}
__device__ float parity(uint32_t i) {
    return (__popc(i) & 1) ? -1.f : 1.f;
}
__global__ void prolong(Frame f, Fields d, MacView v) {
    if (!mixed(v))
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= v.coarseCount || !(v.state[candidate(v)][id] & 1))
        return;
    int3 base = coarseCoord(id, v) * 2;
    float external[24], internal[12], div[8], phi[8]{};
    for (int a = 0; a < 3; ++a)
        for (uint32_t j = 0; j < 4; ++j) {
            int3 p = base;
            component(p, (a + 1) % 3) += j & 1;
            component(p, (a + 2) % 3) += j >> 1;
            external[a * 8 + j] = d.faces[faceIndex(p, a, f)].x;
            component(p, a) += 2;
            external[a * 8 + 4 + j] = d.faces[faceIndex(p, a, f)].x;
            internal[a * 4 + j] = .5f * (external[a * 8 + j] + external[a * 8 + 4 + j]);
        }
    float mean = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        int3 p = make_int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        div[i] = 0;
        for (int a = 0; a < 3; ++a) {
            uint32_t j = component(p, (a + 1) % 3) + 2 * component(p, (a + 2) % 3);
            div[i] += component(p, a) ? external[a * 8 + 4 + j] - internal[a * 4 + j]
                                      : internal[a * 4 + j] - external[a * 8 + j];
        }
        mean += div[i] / 8;
    }
    for (uint32_t mode = 1; mode < 8; ++mode) {
        float coefficient = 0;
        for (uint32_t i = 0; i < 8; ++i)
            coefficient += parity(i & mode) * (mean - div[i]) / (16 * __popc(mode));
        for (uint32_t i = 0; i < 8; ++i)
            phi[i] += parity(i & mode) * coefficient;
    }
    for (int a = 0; a < 3; ++a)
        for (uint32_t j = 0; j < 4; ++j) {
            int3 p = make_int3(0, 0, 0);
            component(p, (a + 1) % 3) = j & 1;
            component(p, (a + 2) % 3) = j >> 1;
            uint32_t left = p.x + 2 * p.y + 4 * p.z;
            float velocity = internal[a * 4 + j] - phi[left + (1u << a)] + phi[left];
            p = p + base;
            component(p, a)++;
            auto &face = d.faces[faceIndex(p, a, f)];
            face.x = velocity;
            face.w = 1;
        }
}
__global__ void measure(Frame f, Fields d, MacView v) {
    if (v.pool.control[BrickInvalid])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.grid.w)
        return;
    int3 p = cellFromIndex(id, f);
    float divergence = 0;
    double preciseDivergence = 0;
    for (int a = 0; a < 3; ++a) {
        int3 q = p;
        component(q, a)++;
        divergence += d.faces[faceIndex(q, a, f)].x - d.faces[faceIndex(p, a, f)].x;
        if (v.precise)
            preciseDivergence += double(d.faces[faceIndex(q, a, f)].x) - d.faces[faceIndex(p, a, f)].x;
    }
    d.cells[id].y = d.cells[id].z == 1 ? (v.precise ? float(preciseDivergence / f.minimumCell.w)
                                                    : divergence / f.minimumCell.w)
                                       : 0;
    d.cells[id].w = d.pressure[mixed(v) ? leaf(p, f, v) : id];
}
__global__ void validatePublishedFlux(Frame f, Fields d, MacView v, PressureView pressure) {
    __shared__ float maxima[128];
    __shared__ uint32_t invalidAtEntry;
    if (!threadIdx.x)
        invalidAtEntry = atomicAdd(v.pool.control + BrickInvalid, 0u);
    __syncthreads();
    // Another block may reject this field while we run. Keep the branch uniform
    // within this block so no lane skips the reduction's synchronization.
    if (invalidAtEntry)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    float value = id < f.grid.w ? fabsf(d.cells[id].y) : 0;
    if (!isfinite(value))
        value = __int_as_float(0x7f800000); // +infinity; always rejects the field
    maxima[threadIdx.x] = value;
    __syncthreads();
    for (uint32_t stride = 64; stride; stride >>= 1) {
        if (threadIdx.x < stride)
            maxima[threadIdx.x] = fmaxf(maxima[threadIdx.x], maxima[threadIdx.x + stride]);
        __syncthreads();
    }
    if (!threadIdx.x) {
        // Nonnegative IEEE double bits have the same ordering as uint64.
        atomicMax(reinterpret_cast<unsigned long long *>(pressure.scalars + 7),
                  static_cast<unsigned long long>(__double_as_longlong(double(maxima[0]))));
        if (maxima[0] > 1e-4f) {
            atomicExch(v.counters + MacInvalid, 1u);
            atomicExch(v.pool.control + BrickInvalid, 1u);
        }
    }
}
__global__ void resetHistory(MacView v) {
    if (v.pool.control[BrickInvalid])
        return;
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < v.coarseCount)
        v.history[id] = 0;
    if (id < v.coarseCount) {
        v.previousRefinement[id] = {};
        v.nextRefinement[id] = {};
    }
}
} // namespace
struct Mac {
    MacConfig config;
    BrickPool *pool = nullptr;
    Pressure *pressure = nullptr;
    MacView view{};
    size_t bytes = 0;
    ~Mac() {
        destroyPressure(pressure);
        for (auto p : view.map)
            if (p)
                cudaFree(p);
        for (auto p : view.state)
            if (p)
                cudaFree(p);
        for (auto p : view.list)
            if (p)
                cudaFree(p);
        if (view.history)
            cudaFree(view.history);
        if (view.counters)
            cudaFree(view.counters);
        if (view.previousRefinement)
            cudaFree(view.previousRefinement);
        if (view.nextRefinement)
            cudaFree(view.nextRefinement);
        if (view.refinementGuards)
            cudaFree(view.refinementGuards);
        destroyBricks(pool);
    }
};
Mac *createMac(const MacConfig &c) {
    const auto policy = c.refinement;
    for (const float value : {policy.velocityScale, policy.velocityDifference, policy.relativeVelocityError,
                              policy.fractionError, policy.responseSeconds, policy.wakeSeconds,
                              policy.quietSeconds, policy.demoteThreshold, policy.promoteThreshold})
        if (!std::isfinite(value) || value <= 0)
            throw std::runtime_error("Invalid CUDA refinement policy");
    if (policy.demoteThreshold >= policy.promoteThreshold || policy.wakeSeconds > 10 ||
        policy.quietSeconds > 10)
        throw std::runtime_error("Invalid CUDA refinement hysteresis");
    if (!c.iterations || c.iterations > 1000 || !std::isfinite(c.predictionSeconds) ||
        c.predictionSeconds < 0 || c.predictionSeconds > 1)
        throw std::runtime_error("Invalid CUDA mixed MAC parameters");
    auto p = std::make_unique<Mac>();
    p->config = c;
    p->pool = createBricks({c.nx, c.ny, c.nz, c.brickCapacity, c.changesPerFrame, sizeof(Cell)});
    auto &v = p->view;
    v.precise = c.multigrid;
    v.pool = brickView(p->pool);
    v.cx = (c.nx + 1) / 2;
    v.cy = (c.ny + 1) / 2;
    v.cz = (c.nz + 1) / 2;
    v.coarseCount = v.cx * v.cy * v.cz;
    auto allocate = [&](uint32_t **dst, size_t count) {
        check(cudaMalloc(dst, count * 4), "CUDA MAC metadata");
        p->bytes += count * 4;
        check(cudaMemset(*dst, 0, count * 4), "CUDA MAC metadata initialization");
    };
    const size_t cells = size_t(c.nx) * c.ny * c.nz;
    for (uint32_t i = 0; i < 2; ++i) {
        allocate(&v.map[i], cells);
        allocate(&v.list[i], cells);
        allocate(&v.state[i], v.coarseCount);
    }
    allocate(&v.history, v.coarseCount);
    allocate(&v.refinementGuards, v.coarseCount);
    allocate(&v.counters, MacCounterCount);
    for (auto *destination : {&v.previousRefinement, &v.nextRefinement}) {
        const size_t bytes = size_t(v.coarseCount) * sizeof(RefinementState);
        check(cudaMalloc(destination, bytes), "CUDA refinement history");
        p->bytes += bytes;
        check(cudaMemset(*destination, 0, bytes), "CUDA refinement initialization");
    }
    check(cudaStreamSynchronize(nullptr), "CUDA MAC initialization completion");
    if (c.multigrid) {
        p->pressure = createPressure({c.nx, c.ny, c.nz, c.cgIterations, 1e-5f, 1e-4f, c.conditionalGraphs});
        v.fineSolution = pressureView(p->pressure).fallback;
        v.pressureCacheState = pressureView(p->pressure).counts;
    }
    return p.release();
}
void destroyMac(Mac *p) noexcept {
    delete p;
}
MacView macView(const Mac *p) {
    if (!p)
        throw std::runtime_error("Missing CUDA MAC");
    return p->view;
}
size_t macBytes(const Mac *p) {
    return p ? p->bytes + brickBytes(p->pool) + pressureBytes(p->pressure) : 0;
}
PressureView macPressureView(const Mac *p) {
    return p ? pressureView(p->pressure) : PressureView{};
}
uint64_t macPressureCapturedBodyNodes(const Mac *p) {
    return p ? pressureCapturedBodyNodes(p->pressure) : 0;
}
void beginMacFrame(Mac *p, void *stream, bool reset) {
    if (!p || !stream)
        throw std::runtime_error("Invalid CUDA MAC frame");
    beginBrickFrame(p->pool, stream);
    if (reset) {
        resetPressureHistory(p->pressure, stream, p->view);
        resetHistory<<<(p->view.coarseCount + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(
            p->view);
        check(cudaGetLastError(), "Reset CUDA MAC hysteresis");
    }
}
uint32_t enqueueMac(Mac *p, void *rawStream, void *const (&buffers)[BufferCount], const void *rawFrame,
                    const void *mass) {
    if (!p || !rawStream || !rawFrame)
        throw std::runtime_error("Invalid CUDA MAC projection");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto &c = p->config;
    if (f.grid.x != c.nx || f.grid.y != c.ny || f.grid.z != c.nz || f.grid.w != uint64_t(c.nx) * c.ny * c.nz)
        throw std::runtime_error("CUDA MAC frame dimensions changed");
    for (auto i : {Faces, Scratch, Cells, Solid, Material, Pressure0, Pressure1})
        if (!buffers[i])
            throw std::runtime_error("Missing CUDA MAC field");
    if (buffers[Faces] == buffers[Scratch] || buffers[Pressure0] == buffers[Pressure1] ||
        !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0 || !std::isfinite(f.gravityDt.w) ||
        f.gravityDt.w <= 0 || !std::isfinite(f.solver.x) || f.solver.x <= 0 || !std::isfinite(f.solver.w) ||
        f.solver.w <= 0 || !buffers[Counts] || f.display.z || f.display.w != (mass ? 2u : 0u))
        throw std::runtime_error("Invalid or unsupported CUDA MAC frame");
    const auto stream = static_cast<cudaStream_t>(rawStream);
    const auto v = p->view;
    const Fields d = fields(buffers, 0, nullptr, mass);
    const uint32_t faces = 3 * (c.nx + 1) * (c.ny + 1) * (c.nz + 1);
    const uint32_t cg = (f.grid.w + 127) / 128, fg = (faces + 127) / 128,
                   coarseGroups = (v.coarseCount + 63) / 64, workers = std::min(cg, 512u);
    clear<<<1, 1, 0, stream>>>(v);
    clearBrickRequests(p->pool, stream);
    if (!c.forcedFine)
        refinementGuards<<<coarseGroups, 64, 0, stream>>>(f, d, v, c);
    measureRefinement<<<coarseGroups, 64, 0, stream>>>(f, d, v, c);
    classify<<<coarseGroups, 64, 0, stream>>>(f, v, c);
    leaves<<<cg, 128, 0, stream>>>(f, d, v);
    stageBricks(p->pool, stream);
    if (p->pressure)
        preparePressure(p->pressure, stream, buffers, &f, v);
    restrictFaces<<<fg, 128, 0, stream>>>(f, d, v);
    assemble<<<workers, 128, 0, stream>>>(f, d, v);
    if (p->pressure)
        assembleFallback<<<cg, 128, 0, stream>>>(f, d, v);
    validateAssembly<<<1, 1, 0, stream>>>(v);
    if (p->pressure)
        enqueuePressure(p->pressure, stream, buffers, &f, v);
    uint32_t pi = c.iterations & 1;
    if (!p->pressure) {
        pi = 0;
        for (uint32_t i = 0; i < c.iterations; ++i) {
            smooth<<<workers, 128, 0, stream>>>(f, fields(buffers, pi), v, pi);
            pi = 1 - pi;
        }
    }
    validateSolution<<<workers, 128, 0, stream>>>(f, fields(buffers, pi), v, pi);
    rejectInvalid<<<1, 1, 0, stream>>>(v);
    exportPressure<<<cg, 128, 0, stream>>>(f, d, v);
    project<<<fg, 128, 0, stream>>>(f, fields(buffers, pi), v);
    prolong<<<coarseGroups, 64, 0, stream>>>(f, d, v);
    measure<<<cg, 128, 0, stream>>>(f, fields(buffers, pi), v);
    if (p->pressure)
        validatePublishedFlux<<<cg, 128, 0, stream>>>(f, d, v, pressureView(p->pressure));
    if (p->pressure)
        publishPressureHistory(p->pressure, stream, buffers, &f, v);
    publishRefinement<<<coarseGroups, 64, 0, stream>>>(f, d, v);
    publishBricks(p->pool, stream);
    check(cudaGetLastError(), "CUDA coupled MAC projection");
    return pi;
}
} // namespace lab::cuda_fluid
