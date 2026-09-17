#pragma once
#include "fluid_cuda_mac.h"
#include "fluid_cuda_device.cuh"
namespace lab::cuda_fluid::mac_detail {
using namespace detail;
// The FP32 matrix is a preconditioner/legacy relaxation representation. Physical
// MGPCG residuals use the adjoint face operator below, not rounded row sums.
struct Row {
    uint32_t count;
    float diagonal, rhs, step;
    uint32_t neighbor[24];
    float coefficient[24], boundaryDiagonal, volumeUnits;
};
struct Cell {
    Row row;
    float pressure[2];
    double preciseRhs, precisePressure;
};
static_assert(sizeof(Row) == 216 && sizeof(Cell) == 240);
// The idle scratch y/z channels carry the precise shared-face average as bits.
// No floating arithmetic is performed on those channels; x retains the legacy
// FP32 value. Only precise mixed projection uses this temporary representation.
__device__ inline double restrictedVelocity(float4 scratch) {
    return __hiloint2double(__float_as_int(scratch.z), __float_as_int(scratch.y));
}
__device__ inline uint32_t candidate(MacView v) {
    return v.pool.control[BrickCandidate];
}
__device__ inline bool ready(MacView v) {
    return v.pool.control[BrickReady] && !v.pool.control[BrickInvalid];
}
__device__ inline bool mixed(MacView v) {
    return ready(v) && v.counters[MacCoarse];
}
__device__ inline uint32_t coarseIndex(int3 p, MacView v) {
    return (p.z * v.cy + p.y) * v.cx + p.x;
}
__device__ inline int3 coarseCoord(uint32_t i, MacView v) {
    return make_int3(i % v.cx, i / v.cx % v.cy, i / (v.cx * v.cy));
}
__device__ inline uint32_t brickKey(int3 p, BrickView v) {
    return ((p.z / 4) * v.by + p.y / 4) * v.bx + p.x / 4;
}
__device__ inline int3 half(int3 p) {
    return make_int3(p.x / 2, p.y / 2, p.z / 2);
}
__device__ inline uint32_t width(int3 p, Frame f, MacView v) {
    return inGrid(p, f) && (v.state[candidate(v)][coarseIndex(half(p), v)] & 1) ? 2 : 1;
}
__device__ inline uint32_t leaf(int3 p, Frame f, MacView v) {
    return v.map[candidate(v)][cellIndex(p, f)];
}
__device__ inline Cell &stored(uint32_t id, Frame f, MacView v) {
    int3 p = cellFromIndex(id, f);
    uint32_t slot = v.pool.pages[candidate(v)][brickKey(p, v.pool)];
    return static_cast<Cell *>(
        v.pool.fields[candidate(v)])[slot * 64 + (p.z & 3) * 16 + (p.y & 3) * 4 + (p.x & 3)];
}
__device__ inline int3 patch(int3 p, int a, Frame f, MacView v, uint32_t &w, float &distance) {
    int3 l = p;
    component(l, a)--;
    uint32_t wl = width(l, f, v), wr = width(p, f, v);
    w = max(wl, wr);
    distance = .5f * (wl + wr);
    if (w == 2) {
        component(p, (a + 1) % 3) &= ~1;
        component(p, (a + 2) % 3) &= ~1;
    }
    return p;
}
// Proposal topology is still needed by leaves()/page staging when residency is
// incomplete. Keep those helpers separate from the accepted solve topology.
// Never reinterpret an invalid resident operator as a fine fallback.
__device__ inline uint32_t solveWidth(int3 p, Frame f, MacView v) {
    return v.pool.control[BrickReady] ? width(p, f, v) : 1;
}
__device__ inline uint32_t solveLeaf(int3 p, Frame f, MacView v) {
    return v.pool.control[BrickReady] ? leaf(p, f, v) : cellIndex(p, f);
}
__device__ inline int3 solvePatch(int3 p, int a, Frame f, MacView v, uint32_t &w, float &distance) {
    if (v.pool.control[BrickReady])
        return patch(p, a, f, v, w, distance);
    w = 1;
    distance = 1;
    return p;
}
__device__ inline uint32_t solveCount(Frame f, MacView v) {
    return v.pool.control[BrickReady] ? v.counters[MacLeaves] : f.grid.w;
}
__device__ inline uint32_t solveId(uint32_t i, MacView v) {
    return v.pool.control[BrickReady] ? v.list[candidate(v)][i] : i;
}
__device__ inline bool solveActive(uint32_t id, Frame f, Fields d, MacView v) {
    return id < f.grid.w && d.cells[id].z == 1 && solveLeaf(cellFromIndex(id, f), f, v) == id;
}
__device__ inline double &preciseRhs(uint32_t id, Frame f, MacView v) {
    return v.pool.control[BrickReady] ? stored(id, f, v).preciseRhs : v.fineSolution[2 * id];
}
__device__ inline double &precisePressure(uint32_t id, Frame f, MacView v) {
    return v.pool.control[BrickReady] ? stored(id, f, v).precisePressure : v.fineSolution[2 * id + 1];
}
__device__ inline Row solveRow(uint32_t id, Frame f, Fields d, MacView v) {
    if (v.pool.control[BrickReady])
        return stored(id, f, v).row;
    Row r{};
    r.volumeUnits = 1;
    int3 p = cellFromIndex(id, f);
    for (int a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            int3 q = p;
            component(q, a) += side ? 1 : -1;
            if (solid(q, f, d))
                continue;
            ++r.diagonal;
            if (liquid(q, f, d)) {
                r.neighbor[r.count] = cellIndex(q, f);
                r.coefficient[r.count++] = -1;
            } else
                ++r.boundaryDiagonal;
        }
    r.step = r.diagonal ? 1 / r.diagonal : 0;
    return r;
}
// Applying the canonical face operator in FP64 preserves closed-domain null
// modes and large nearly cancelling pressures. The scalar boundary coefficient
// is known from geometry, so no rounded diagonal cancellation becomes a leak.
template <class Sample>
__device__ double physicalApply(uint32_t id, Frame f, Fields d, MacView v, Sample sample) {
    int3 c = cellFromIndex(id, f);
    double ap = 0, self = sample(id);
    // With no coarse leaves the adjoint face operator is exactly the six-face
    // stencil. Avoid patch/leaf gathers and repeated self samples, but retain
    // FP64 pressure differences (not diagonal * p minus rounded row sums).
    // This includes the qualified matrix-free fallback during page deferral.
    if (!mixed(v)) {
        for (int a = 0; a < 3; ++a)
            for (uint32_t side = 0; side < 2; ++side) {
                int3 q = c;
                component(q, a) += side ? 1 : -1;
                if (!solid(q, f, d))
                    ap += self - (liquid(q, f, d) ? sample(cellIndex(q, f)) : 0.);
            }
        return ap;
    }
    uint32_t size = solveWidth(c, f, v);
    for (int a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            int3 p = c;
            component(p, a) += side * size;
            int3 l = p;
            component(l, a)--;
            if (solid(l, f, d) || solid(p, f, d))
                continue;
            // Only T-junction faces require the adjoint patch gather. Fine/fine
            // and coarse/coarse faces retain their exact finite-volume stencil
            // even when some other part of the domain is mixed. A width-2 leaf
            // is fully wet (including the refinement halo), so its four face
            // samples all resolve to the same pressure unknown.
            const uint32_t wl = solveWidth(l, f, v), wr = solveWidth(p, f, v);
            if (wl == wr) {
                const int3 neighbor = side ? p : l;
                ap +=
                    double(size) * (self - (liquid(neighbor, f, d) ? sample(solveLeaf(neighbor, f, v)) : 0.));
                continue;
            }
            uint32_t w;
            float distance;
            int3 base = solvePatch(p, a, f, v, w, distance);
            double difference = 0;
            for (uint32_t j = 0; j < w; ++j)
                for (uint32_t k = 0; k < w; ++k) {
                    int3 q = base;
                    component(q, (a + 1) % 3) += k;
                    component(q, (a + 2) % 3) += j;
                    for (uint32_t lr = 0; lr < 2; ++lr) {
                        int3 n = q;
                        component(n, a) -= 1 - lr;
                        difference +=
                            (lr ? -1. : 1.) * ((liquid(n, f, d) ? sample(solveLeaf(n, f, v)) : 0.) - self);
                    }
                }
            ap += (side ? 1. : -1.) * double(size * size) * difference / (w * w * double(distance));
        }
    return ap;
}
} // namespace lab::cuda_fluid::mac_detail
