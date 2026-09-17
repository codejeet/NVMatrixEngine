#include "fluid_cuda_geometric_transport.h"
#include "fluid_cuda_geometry.cuh"
#include "fluid_cuda_device.cuh"
#include "fluid_cuda_loop_condition.cuh"
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
struct alignas(16) Q {
    double v[4];
};
struct Control {
    GeometricTransportMetrics metrics;
    uint32_t capacityRunning, capacityIteration;
    double excess;
};
struct Device {
    uint3 grid, fine;
    uint32_t cells, faces, stride, maximum;
    double h, dt;
    const Q *source;
    const double2 *sourcePhase;
    const double4 *sourcePlanes;
    const double *rates;
    Q *q[2], *accumulated, *output, *transfers;
    double2 *phase[2], *candidate;
    double4 *planes, *limits;
    double *flux;
    Control *control;
    uint32_t *failure;
};
void check(cudaError_t e, const char *what) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(e));
}
__device__ bool valid(Q q) {
    return isfinite(q.v[0]) && isfinite(q.v[1]) && isfinite(q.v[2]) && isfinite(q.v[3]) && q.v[3] >= 0 &&
           (q.v[3] > 0 || (q.v[0] == 0 && q.v[1] == 0 && q.v[2] == 0));
}
__device__ void reject(Device d, uint32_t stage, uint32_t index) {
    atomicAdd(&d.control->metrics.invalid, 1u);
    if (atomicCAS(&d.control->metrics.failureStage, 0u, stage) == 0)
        d.control->metrics.failureIndex = index;
    atomicExch(d.failure, 1u);
}
__device__ void maximum(double *p, double x) {
    atomicMax(reinterpret_cast<unsigned long long *>(p),
              static_cast<unsigned long long>(__double_as_longlong(x)));
}
__device__ uint32_t face(uint3 p, uint32_t a, Device d) {
    return a * d.stride + (p.z * (d.grid.y + 1) + p.y) * (d.grid.x + 1) + p.x;
}
__device__ bool faceCells(uint32_t i, Device d, uint32_t &a, uint32_t &left, uint32_t &right) {
    a = i / d.stride;
    const uint32_t k = i % d.stride;
    uint3 p = make_uint3(k % (d.grid.x + 1), (k / (d.grid.x + 1)) % (d.grid.y + 1),
                         k / ((d.grid.x + 1) * (d.grid.y + 1)));
    uint3 extent = d.grid;
    (&extent.x)[a]++;
    if (p.x >= extent.x || p.y >= extent.y || p.z >= extent.z || !(&p.x)[a] || (&p.x)[a] >= (&d.grid.x)[a])
        return false;
    right = geometry::index(p, d.grid);
    (&p.x)[a]--;
    left = geometry::index(p, d.grid);
    return true;
}
__device__ bool active(Device d, uint32_t step) {
    return !*d.failure && step < d.control->metrics.substeps;
}
__global__ void clear(Device d) {
    if (!*d.failure)
        *d.control = {};
}
__global__ void initialize(Device d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < d.cells) {
        const auto phase = d.sourcePhase[id];
        const auto q = d.source[id];
        const auto p = geometry::coordinate(id, d.grid);
        const auto w = geometry::widths(p, d.fine, d.h);
        const double full = w.x * w.y * w.z;
        if (!valid(q) || !isfinite(phase.x) || !isfinite(phase.y) || phase.x < 0 ||
            fabs(phase.y - full) > full * 2e-13 || phase.x > phase.y * (1 + 2e-13) ||
            q.v[3] > phase.x * (1 + 2e-13)) {
            reject(d, GeometricInput, id);
            return;
        }
        d.q[0][id] = q;
        d.phase[0][id] = phase;
        double incoming = 0, outgoing = 0;
        for (uint32_t a = 0; a < 3; ++a)
            for (uint32_t side = 0; side < 2; ++side) {
                auto fp = p;
                if (side)
                    (&fp.x)[a]++;
                const double rate = d.rates[face(fp, a, d)] * (side ? 1 : -1);
                outgoing += fmax(0., rate);
                incoming += fmax(0., -rate);
            }
        const double courant = fmax(incoming, outgoing) * d.dt / phase.y;
        if (!isfinite(courant)) {
            reject(d, GeometricCourant, id);
            return;
        }
        maximum(&d.control->metrics.maximumCourant, courant);
    }
    if (id < d.faces) {
        uint32_t a, l, r;
        const bool internal = faceCells(id, d, a, l, r);
        if (!isfinite(d.rates[id]) || (!internal && d.rates[id] != 0)) {
            reject(d, GeometricRate, id);
            return;
        }
        d.accumulated[id] = {};
    }
}
__global__ void schedule(Device d) {
    if (*d.failure)
        return;
    const double n = ceil(d.control->metrics.maximumCourant / .45);
    if (n > d.maximum) {
        d.control->metrics.capped = 1;
        atomicExch(d.failure, 1u);
        return;
    }
    d.control->metrics.substeps = static_cast<uint32_t>(fmax(1., n));
}
__global__ void planes(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto p = step == 0 && d.sourcePlanes
                       ? d.sourcePlanes[id]
                       : geometry::reconstruct(id, d.grid, d.fine, d.h, d.phase[step & 1]);
    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) || !isfinite(p.w)) {
        reject(d, GeometricPlaneFinite, id);
        return;
    }
    const auto phase = d.phase[step & 1][id];
    const double normal = fabs(p.x) + fabs(p.y) + fabs(p.z);
    const double represented = geometry::fraction(make_double3(p.x, p.y, p.z), p.w) * phase.y;
    if (!isfinite(normal) || (normal > 0 && fabs(normal - 1) > 2e-13) ||
        (normal == 0 && (p.w < 0 || p.w > 1)) || fabs(represented - phase.x) > phase.x * 2e-12) {
        reject(d, GeometricPlaneVolume, id);
        return;
    }
    d.planes[id] = p;
}
__global__ void candidates(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.faces)
        return;
    uint32_t a, l, r;
    if (!faceCells(id, d, a, l, r) || d.rates[id] == 0) {
        d.candidate[id] = make_double2(0, 0);
        return;
    }
    const double swept = d.rates[id] * (d.dt / d.control->metrics.substeps);
    const uint32_t donor = swept > 0 ? l : r;
    const auto phase = d.phase[step & 1][donor];
    const double width = fabs(swept) / phase.y;
    if (!isfinite(width) || width > .450000000001) {
        reject(d, GeometricSweep, id);
        return;
    }
    const double wet = geometry::slabFraction(d.planes[donor], a, width, swept > 0);
    const bool contained = geometry::slabContains(d.planes[donor], a, width, swept > 0);
    // Geometric containment, not an epsilon-volume deletion: if the entire wet
    // region leaves, transport its authoritative quantity exactly. Subtracting
    // separately rounded integrals otherwise leaves a one-ulp ghost owner.
    const double low = swept * (phase.x / phase.y);
    const double high = contained ? (swept > 0 ? phase.x : -phase.x) : swept * wet;
    if (!isfinite(low) || !isfinite(high)) {
        reject(d, GeometricCandidate, id);
        return;
    }
    d.candidate[id] = make_double2(low, high);
}
__device__ double ratio(double budget, double requested) {
    return requested > 0 ? fmin(1., fmax(0., budget) / requested) : 1;
}
__global__ void capacityBegin(Device d, uint32_t step) {
    d.control->capacityRunning = active(d, step);
    d.control->capacityIteration = 0;
    d.control->excess = 0;
}
__global__ void capacityInitialize(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto p = geometry::coordinate(id, d.grid);
    double incoming = 0;
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            auto fp = p;
            if (side)
                (&fp.x)[a]++;
            incoming += fmax(0., d.candidate[face(fp, a, d)].x * (side ? -1 : 1));
        }
    const auto phase = d.phase[step & 1][id];
    // Two receiver-admission banks, original incoming flux and available space.
    // Reuse the later FCT scratch; no additional per-cell allocation.
    d.limits[id] = make_double4(1, 1, incoming, fmax(0., phase.y - phase.x));
}
__device__ double admittedOutflow(Device d, uint3 p, uint32_t bank) {
    double outgoing = 0;
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            auto fp = p;
            if (side)
                (&fp.x)[a]++;
            const double value = d.candidate[face(fp, a, d)].x * (side ? 1 : -1);
            if (value <= 0)
                continue;
            auto q = p;
            if (side)
                (&q.x)[a]++;
            else
                (&q.x)[a]--;
            // Candidate generation already makes all domain-boundary flux zero.
            outgoing += value * (&d.limits[geometry::index(q, d.grid)].x)[bank];
        }
    return outgoing;
}
__global__ void capacityMeasure(Device d, uint32_t step) {
    if (!active(d, step) || !d.control->capacityRunning)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const uint32_t bank = d.control->capacityIteration & 1;
    const double outgoing = admittedOutflow(d, geometry::coordinate(id, d.grid), bank);
    const auto phase = d.phase[step & 1][id];
    const auto limits = d.limits[id];
    const double incoming = (&limits.x)[bank] * limits.z;
    const double excess = fmax(0., ((phase.x - outgoing) + incoming - phase.y) / phase.y);
    maximum(&d.control->excess, excess);
}
__global__ void capacityDecide(Device d, uint32_t step, uint32_t maximumIterations) {
    if (!active(d, step) || !d.control->capacityRunning)
        return;
    d.control->metrics.maximumAdmissionError =
        fmax(d.control->metrics.maximumAdmissionError, d.control->excess);
    // Reserve most of the external 2e-13 bound for subsequent FCT arithmetic.
    if (d.control->excess <= 2e-14) {
        d.control->capacityRunning = 0;
    } else if (d.control->capacityIteration >= maximumIterations) {
        d.control->metrics.capacityCapped = 1;
        reject(d, GeometricCapacityConvergence, 0xffffffffu);
        d.control->capacityRunning = 0;
    }
}
__global__ void capacityNext(Device d, uint32_t step) {
    if (!active(d, step) || !d.control->capacityRunning)
        return;
    ++d.control->capacityIteration;
    ++d.control->metrics.capacityIterations;
    d.control->excess = 0;
}
__global__ void capacityIterate(Device d, uint32_t step) {
    if (!active(d, step) || !d.control->capacityRunning)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const uint32_t bank = d.control->capacityIteration & 1, previous = 1 - bank;
    const double outgoing = admittedOutflow(d, geometry::coordinate(id, d.grid), previous);
    const auto limits = d.limits[id];
    // A receiver may admit its free space plus what its neighbors accept from
    // it. Monotone iteration propagates downstream capacity upstream, including
    // full-cell cycles. Only shared face fluxes change, never owned quantities.
    (&d.limits[id].x)[bank] = fmin((&limits.x)[previous], ratio(limits.w + outgoing, limits.z));
}
__global__ void capacityFluxes(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.faces)
        return;
    uint32_t a, left, right;
    if (!faceCells(id, d, a, left, right))
        return;
    const double original = d.candidate[id].x;
    const uint32_t receiver = original >= 0 ? right : left;
    const double adjusted = original * (&d.limits[receiver].x)[d.control->capacityIteration & 1];
    maximum(&d.control->metrics.maximumFluxReduction, fabs(original - adjusted));
    d.candidate[id].x = adjusted;
}
__global__ void limits(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto p = geometry::coordinate(id, d.grid);
    const auto phase = d.phase[step & 1][id];
    double low = phase.x, plus = 0, minus = 0, lowOut = 0, extraOut = 0;
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            auto fp = p;
            if (side)
                (&fp.x)[a]++;
            const auto c = d.candidate[face(fp, a, d)];
            const double sign = side ? -1 : 1;
            low += sign * c.x;
            const double change = sign * (c.y - c.x);
            plus += fmax(0., change);
            minus += fmax(0., -change);
            lowOut += fmax(0., -sign * c.x);
            if (sign * c.x < 0)
                extraOut += fmax(0., sign * (c.x - c.y));
        }
    if (!isfinite(low) || low < 0 || low > phase.y * (1 + 2e-13)) {
        reject(d, GeometricLowOrderBounds, id);
        return;
    }
    // Net positivity alone does not bound gross donor loss. The third budget
    // retains a nonnegative old-fluid coefficient, making momentum a convex
    // mixture transported by exactly the same accepted liquid-volume fluxes.
    d.limits[id] =
        make_double4(ratio(phase.y - low, plus), ratio(low, minus), ratio(phase.x - lowOut, extraOut), 1);
}
__global__ void fluxes(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.faces)
        return;
    uint32_t a, l, r;
    if (!faceCells(id, d, a, l, r)) {
        d.flux[id] = 0;
        return;
    }
    const auto c = d.candidate[id];
    const double delta = c.y - c.x;
    const auto left = d.limits[l], right = d.limits[r];
    double factor = delta > 0 ? fmin(left.y, right.x) : fmin(left.x, right.y);
    if (fabs(c.y) > fabs(c.x))
        factor = fmin(factor, c.x > 0 ? left.z : right.z);
    d.flux[id] = factor >= 1 ? c.y : factor <= 0 ? c.x : c.x + factor * delta;
}
__global__ void donorRoundoff(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto p = geometry::coordinate(id, d.grid);
    double out = 0;
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            auto fp = p;
            if (side)
                (&fp.x)[a]++;
            out += fmax(0., d.flux[face(fp, a, d)] * (side ? 1 : -1));
        }
    const double volume = d.phase[step & 1][id].x;
    if (out > volume * (1 + 2e-13)) {
        reject(d, GeometricDonorBounds, id);
        return;
    }
    double scale = out > volume ? volume / out : 1;
    if (scale < 1)
        for (uint32_t i = 0; i < 4; ++i)
            scale = nextafter(scale, 0.);
    d.limits[id].w = scale;
}
__global__ void adjustedFluxes(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.faces)
        return;
    uint32_t a, l, r;
    if (!faceCells(id, d, a, l, r))
        return;
    const double value = d.flux[id] * d.limits[d.flux[id] > 0 ? l : r].w;
    d.flux[id] = value;
    const uint32_t donor = value > 0 ? l : r;
    const double volume = d.phase[step & 1][donor].x;
    const double weight = volume > 0 ? value / volume : 0;
    const auto q = d.q[step & 1][donor];
    for (uint32_t k = 0; k < 4; ++k)
        d.accumulated[id].v[k] += q.v[k] * weight;
}
__global__ void update(Device d, uint32_t step) {
    if (!active(d, step))
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto p = geometry::coordinate(id, d.grid);
    const auto phase = d.phase[step & 1][id];
    double out = 0, incoming = 0;
    Q incomingQ{};
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t side = 0; side < 2; ++side) {
            auto fp = p;
            if (side)
                (&fp.x)[a]++;
            const double value = d.flux[face(fp, a, d)] * (side ? 1 : -1);
            out += fmax(0., value);
            if (value >= 0)
                continue;
            auto neighbor = p;
            if (side)
                (&neighbor.x)[a]++;
            else
                (&neighbor.x)[a]--;
            if ((&neighbor.x)[a] >= (&d.grid.x)[a]) {
                reject(d, GeometricBoundary, id);
                return;
            }
            const uint32_t donor = geometry::index(neighbor, d.grid);
            const double v = d.phase[step & 1][donor].x;
            if (v <= 0) {
                reject(d, GeometricEmptyDonor, id);
                return;
            }
            const auto q = d.q[step & 1][donor];
            for (uint32_t k = 0; k < 4; ++k)
                incomingQ.v[k] += q.v[k] * (-value / v);
            incoming -= value;
        }
    const double retained = phase.x > 0 ? 1 - out / phase.x : 1;
    const double liquid = phase.x * retained + incoming;
    Q q = d.q[step & 1][id];
    for (uint32_t k = 0; k < 4; ++k)
        q.v[k] = q.v[k] * retained + incomingQ.v[k];
    if (retained < 0 || !valid(q) || !isfinite(liquid) || liquid < 0 || liquid > phase.y * (1 + 2e-13) ||
        q.v[3] > liquid * (1 + 2e-13)) {
        reject(d, GeometricUpdateBounds, id);
        return;
    }
    maximum(&d.control->metrics.maximumExcess, fmax(0., liquid - phase.y));
    d.q[1 - (step & 1)][id] = q;
    d.phase[1 - (step & 1)][id] = make_double2(liquid, phase.y);
    if (!id)
        ++d.control->metrics.completed;
}
__global__ void publish(Device d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < d.cells)
        d.output[id] = d.q[d.control->metrics.substeps & 1][id];
    if (id < d.faces)
        d.transfers[id] = d.accumulated[id];
}
} // namespace
struct GeometricTransport {
    GeometricTransportConfig config;
    Device d{};
    size_t bytes = 0;
    cudaStream_t bodyCapture = nullptr;
    uint64_t capturedBodyNodes = 0;
    ~GeometricTransport() {
        if (bodyCapture)
            cudaStreamDestroy(bodyCapture);
        for (void *p :
             {static_cast<void *>(d.q[0]), static_cast<void *>(d.q[1]), static_cast<void *>(d.phase[0]),
              static_cast<void *>(d.phase[1]), static_cast<void *>(d.candidate),
              static_cast<void *>(d.planes), static_cast<void *>(d.limits), static_cast<void *>(d.flux),
              static_cast<void *>(d.accumulated), static_cast<void *>(d.control)})
            if (p)
                cudaFree(p);
    }
};
GeometricTransport *createGeometricTransport(const GeometricTransportConfig &c) {
    if (!c.nx || !c.ny || !c.nz || c.nx > 1048576 || c.ny > 1048576 || c.nz > 1048576 ||
        uint64_t(c.nx) * c.ny * c.nz > 1048576 || !c.maxSubsteps || c.maxSubsteps > 32 ||
        !c.maxCapacityIterations || c.maxCapacityIterations > 512)
        throw std::runtime_error("Invalid CUDA geometric transport configuration");
    auto p = std::make_unique<GeometricTransport>();
    p->config = c;
    if (c.conditionalGraphs)
        check(cudaStreamCreateWithFlags(&p->bodyCapture, cudaStreamNonBlocking),
              "CUDA capacity body capture stream");
    auto &d = p->d;
    d.fine = make_uint3(c.nx, c.ny, c.nz);
    d.grid = make_uint3((c.nx + 1) / 2, (c.ny + 1) / 2, (c.nz + 1) / 2);
    d.cells = d.grid.x * d.grid.y * d.grid.z;
    d.stride = (d.grid.x + 1) * (d.grid.y + 1) * (d.grid.z + 1);
    d.faces = 3 * d.stride;
    d.maximum = c.maxSubsteps;
    for (uint32_t k = 0; k < 2; ++k) {
        check(cudaMalloc(&d.q[k], size_t(d.cells) * 32), "CUDA geometric quantity ping/pong");
        check(cudaMalloc(&d.phase[k], size_t(d.cells) * 16), "CUDA geometric phase ping/pong");
    }
    check(cudaMalloc(&d.candidate, size_t(d.faces) * 16), "CUDA geometric flux candidates");
    check(cudaMalloc(&d.planes, size_t(d.cells) * 32), "CUDA geometric planes");
    check(cudaMalloc(&d.limits, size_t(d.cells) * 32), "CUDA geometric flux budgets");
    check(cudaMalloc(&d.flux, size_t(d.faces) * 8), "CUDA geometric accepted fluxes");
    check(cudaMalloc(&d.accumulated, size_t(d.faces) * 32), "CUDA geometric accumulated transfers");
    check(cudaMalloc(&d.control, sizeof(Control)), "CUDA geometric control");
    check(cudaMemset(d.control, 0, sizeof(Control)), "CUDA geometric diagnostic initialization");
    p->bytes = size_t(d.cells) * 160 + size_t(d.faces) * 56 + sizeof(Control);
    return p.release();
}
void destroyGeometricTransport(GeometricTransport *p) noexcept {
    delete p;
}
size_t geometricTransportBytes(const GeometricTransport *p) {
    return p ? p->bytes : 0;
}
uint64_t geometricTransportCapturedBodyNodes(const GeometricTransport *p) {
    return p ? p->capturedBodyNodes : 0;
}
const GeometricTransportMetrics *geometricTransportMetrics(const GeometricTransport *p) {
    return p ? &p->d.control->metrics : nullptr;
}
static void enqueueCapacityLoop(GeometricTransport *p, cudaStream_t stream, Device d, uint32_t step,
                                uint32_t cells) {
    auto iteration = [&](cudaStream_t s) {
        capacityNext<<<1, 1, 0, s>>>(d, step);
        capacityIterate<<<cells, 128, 0, s>>>(d, step);
        capacityMeasure<<<cells, 128, 0, s>>>(d, step);
        capacityDecide<<<1, 1, 0, s>>>(d, step, p->config.maxCapacityIterations);
    };
    cudaStreamCaptureStatus status = cudaStreamCaptureStatusNone;
    cudaGraph_t parent = nullptr;
    if (p->config.conditionalGraphs)
        check(cudaStreamGetCaptureInfo(stream, &status, nullptr, &parent), "Query CUDA capacity capture");
    if (status == cudaStreamCaptureStatusActive) {
        cudaGraphConditionalHandle handle{};
        check(cudaGraphConditionalHandleCreate(&handle, parent, 0, cudaGraphCondAssignDefault),
              "CUDA capacity conditional handle (select unrolled loops if unsupported)");
        enqueueCudaLoopCondition(stream, d.failure, &d.control->capacityRunning, handle);
        const cudaGraphNode_t *dependencies = nullptr;
        const cudaGraphEdgeData *edges = nullptr;
        size_t count = 0;
        check(cudaStreamGetCaptureInfo(stream, &status, nullptr, &parent, &dependencies, &edges, &count),
              "CUDA capacity loop dependencies");
        cudaGraphNodeParams params{};
        params.type = cudaGraphNodeTypeConditional;
        params.conditional.handle = handle;
        params.conditional.type = cudaGraphCondTypeWhile;
        params.conditional.size = 1;
        cudaGraphNode_t node = nullptr;
        check(cudaGraphAddNode(&node, parent, dependencies, edges, count, &params),
              "CUDA capacity WHILE node");
        const auto body = params.conditional.phGraph_out[0];
        bool capturing = false;
        cudaGraph_t ended = nullptr;
        try {
            check(cudaStreamBeginCaptureToGraph(p->bodyCapture, body, nullptr, nullptr, 0,
                                                cudaStreamCaptureModeThreadLocal),
                  "Begin CUDA capacity body capture");
            capturing = true;
            iteration(p->bodyCapture);
            enqueueCudaLoopCondition(p->bodyCapture, d.failure, &d.control->capacityRunning, handle);
            const auto result = cudaStreamEndCapture(p->bodyCapture, &ended);
            capturing = false;
            check(result, "End CUDA capacity body capture");
            if (ended != body)
                throw std::runtime_error("CUDA capacity capture changed its owned body graph");
            size_t nodes = 0;
            check(cudaGraphGetNodes(body, nullptr, &nodes), "CUDA capacity body node count");
            p->capturedBodyNodes += nodes;
        } catch (...) {
            if (capturing)
                cudaStreamEndCapture(p->bodyCapture, &ended);
            throw; // The enclosing graph owns the conditional body.
        }
        check(
            cudaStreamUpdateCaptureDependencies(stream, &node, nullptr, 1, cudaStreamSetCaptureDependencies),
            "Join CUDA capacity loop to enclosing capture");
    } else {
        if (status != cudaStreamCaptureStatusNone)
            throw std::runtime_error("Invalidated CUDA capacity capture");
        for (uint32_t i = 0; i < p->config.maxCapacityIterations; ++i)
            iteration(stream);
    }
}
void enqueueGeometricTransport(GeometricTransport *p, void *stream, const void *rawFrame,
                               const GeometricTransportInputs &v) {
    if (!p || !stream || !rawFrame)
        throw std::runtime_error("Invalid CUDA geometric transport invocation");
    const void *views[]{v.quantity, v.phase, v.rates, v.output, v.transfers, v.failure};
    for (uint32_t i = 0; i < 6; ++i) {
        if (!views[i])
            throw std::runtime_error("Missing CUDA geometric transport view");
        for (uint32_t j = 0; j < i; ++j)
            if (views[i] == views[j])
                throw std::runtime_error("Aliased CUDA geometric transport views");
        if (v.planes == views[i])
            throw std::runtime_error("Aliased CUDA geometric plane cache");
    }
    detail::Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto &c = p->config;
    if (f.grid.x != c.nx || f.grid.y != c.ny || f.grid.z != c.nz || f.grid.w != c.nx * c.ny * c.nz ||
        !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0 || !std::isfinite(f.gravityDt.w) ||
        f.gravityDt.w <= 0)
        throw std::runtime_error("CUDA geometric transport frame mismatch");
    auto d = p->d;
    d.h = f.minimumCell.w;
    d.dt = f.gravityDt.w;
    d.source = static_cast<const Q *>(v.quantity);
    d.sourcePhase = static_cast<const double2 *>(v.phase);
    d.sourcePlanes = static_cast<const double4 *>(v.planes);
    d.rates = static_cast<const double *>(v.rates);
    d.output = static_cast<Q *>(v.output);
    d.transfers = static_cast<Q *>(v.transfers);
    d.failure = v.failure;
    const auto s = static_cast<cudaStream_t>(stream);
    const uint32_t cells = (d.cells + 127) / 128, faces = (d.faces + 127) / 128;
    clear<<<1, 1, 0, s>>>(d);
    initialize<<<std::max(cells, faces), 128, 0, s>>>(d);
    schedule<<<1, 1, 0, s>>>(d);
    for (uint32_t i = 0; i < c.maxSubsteps; ++i) {
        planes<<<cells, 128, 0, s>>>(d, i);
        candidates<<<faces, 128, 0, s>>>(d, i);
        capacityBegin<<<1, 1, 0, s>>>(d, i);
        capacityInitialize<<<cells, 128, 0, s>>>(d, i);
        capacityMeasure<<<cells, 128, 0, s>>>(d, i);
        capacityDecide<<<1, 1, 0, s>>>(d, i, c.maxCapacityIterations);
        enqueueCapacityLoop(p, s, d, i, cells);
        capacityFluxes<<<faces, 128, 0, s>>>(d, i);
        limits<<<cells, 128, 0, s>>>(d, i);
        fluxes<<<faces, 128, 0, s>>>(d, i);
        donorRoundoff<<<cells, 128, 0, s>>>(d, i);
        adjustedFluxes<<<faces, 128, 0, s>>>(d, i);
        update<<<cells, 128, 0, s>>>(d, i);
    }
    publish<<<std::max(cells, faces), 128, 0, s>>>(d);
    check(cudaGetLastError(), "CUDA conservative geometric transport");
}
} // namespace lab::cuda_fluid
