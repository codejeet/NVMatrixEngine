#include "fluid_cuda_owned_transport.h"
#include "fluid_cuda_device.cuh"
#include <cuda_runtime.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
struct alignas(16) Quantity {
    double x, y, z, w;
};
static_assert(sizeof(Quantity) == 32 && alignof(Quantity) == 16);
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
struct Control {
    OwnedTransportMetrics metrics;
    double workingResidual;
    uint32_t active;
};
struct Device {
    uint3 grid;
    uint32_t cells, stride;
    double dt, tolerance;
    const Quantity *quantity;
    const double2 *capacity;
    const double *rates;
    Quantity *ping, *pong, *output, *transfers;
    double *diagonal;
    Control *control;
    uint32_t *failure;
};
__device__ uint32_t index(uint3 p, Device d) {
    return (p.z * d.grid.y + p.y) * d.grid.x + p.x;
}
__device__ uint3 coord(uint32_t id, Device d) {
    return make_uint3(id % d.grid.x, (id / d.grid.x) % d.grid.y, id / (d.grid.x * d.grid.y));
}
__device__ uint32_t face(uint3 p, uint32_t a, Device d) {
    return a * d.stride + (p.z * (d.grid.y + 1) + p.y) * (d.grid.x + 1) + p.x;
}
__device__ double flow(uint3 p, uint32_t a, Device d) {
    return d.dt * d.rates[face(p, a, d)];
}
__device__ bool valid(Quantity q) {
    return isfinite(q.x) && isfinite(q.y) && isfinite(q.z) && isfinite(q.w) && q.w >= 0 &&
           (q.w > 0 || (q.x == 0 && q.y == 0 && q.z == 0));
}
__device__ Quantity multiply(Quantity q, double scale) {
    return {q.x * scale, q.y * scale, q.z * scale, q.w * scale};
}
__device__ void add(Quantity &q, Quantity a, double scale) {
    q.x += a.x * scale;
    q.y += a.y * scale;
    q.z += a.z * scale;
    q.w += a.w * scale;
}
__device__ void reject(Device d) {
    atomicAdd(&d.control->metrics.invalid, 1u);
    atomicExch(d.failure, 1u);
}
__device__ void maximum(double *out, double value) {
    // All reduced values are finite and nonnegative; IEEE bit ordering agrees.
    atomicMax(reinterpret_cast<unsigned long long *>(out),
              static_cast<unsigned long long>(__double_as_longlong(value)));
}
__device__ Quantity source(uint32_t id, Device d, const Quantity *solution) {
    uint3 p = coord(id, d);
    Quantity q = d.quantity[id];
    for (uint32_t a = 0; a < 3; ++a) {
        uint3 lo = p, hi = p;
        (&lo.x)[a]--;
        (&hi.x)[a]++;
        const double l = flow(p, a, d), r = flow(hi, a, d);
        if ((&p.x)[a] > 0 && l > 0)
            add(q, solution[index(lo, d)], l);
        if ((&hi.x)[a] < (&d.grid.x)[a] && r < 0)
            add(q, solution[index(hi, d)], -r);
    }
    return q;
}
__global__ void clear(Device d) {
    *d.control = {};
    d.control->active = !*d.failure;
}
__global__ void initialize(Device d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto c = d.capacity[id];
    const auto q = d.quantity[id];
    if (!isfinite(c.x) || !isfinite(c.y) || c.x < 0 || c.y < 0 || !valid(q) ||
        q.w > c.y + d.tolerance * c.y) {
        reject(d);
        return;
    }
    uint3 p = coord(id, d);
    double outflow = 0;
    for (uint32_t a = 0; a < 3; ++a) {
        uint3 hi = p;
        (&hi.x)[a]++;
        outflow += fmax(0., -flow(p, a, d)) + fmax(0., flow(hi, a, d));
    }
    const double diagonal = c.x + outflow;
    if (!isfinite(diagonal) || diagonal < 0 || (diagonal == 0 && q.w > 0)) {
        reject(d);
        return;
    }
    d.diagonal[id] = diagonal;
    d.ping[id] = d.pong[id] = {};
}
__device__ bool internalFace(uint32_t id, Device d, uint32_t &a, uint3 &p) {
    a = id / d.stride;
    uint32_t k = id % d.stride;
    p = make_uint3(k % (d.grid.x + 1), (k / (d.grid.x + 1)) % (d.grid.y + 1),
                   k / ((d.grid.x + 1) * (d.grid.y + 1)));
    uint3 extent = d.grid;
    (&extent.x)[a]++;
    return p.x < extent.x && p.y < extent.y && p.z < extent.z && (&p.x)[a] > 0 && (&p.x)[a] < (&d.grid.x)[a];
}
__global__ void validateRates(Device d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.stride * 3)
        return;
    uint32_t a;
    uint3 p;
    const bool internal = internalFace(id, d, a, p);
    const double rate = d.rates[id];
    if (!isfinite(rate) || !isfinite(rate * d.dt) || (!internal && rate != 0))
        reject(d);
}
__global__ void iterate(Device d, bool reverse) {
    if (*d.failure || !d.control->active)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    if (!id)
        ++d.control->metrics.iterations;
    const double diagonal = d.diagonal[id];
    const auto value =
        diagonal > 0 ? multiply(source(id, d, reverse ? d.pong : d.ping), 1 / diagonal) : Quantity{};
    if (!valid(value)) {
        reject(d);
        return;
    }
    (reverse ? d.ping : d.pong)[id] = value;
}
__global__ void residual(Device d) {
    if (*d.failure || !d.control->active)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.cells)
        return;
    const auto rhs = source(id, d, d.ping), value = d.ping[id], q = d.quantity[id];
    // Scale by actual equation terms, independently for volume and each
    // momentum component. Capacity is not a transported quantity: using it as
    // an absolute floor could declare a nonzero, dilute owner solved at zero.
    // A max-term backward error also handles opposing momentum cancellation.
    Quantity scale{};
    for (uint32_t a = 0; a < 4; ++a) {
        const double lhs = d.diagonal[id] * (&value.x)[a];
        if (!isfinite(lhs) || !isfinite((&rhs.x)[a])) {
            reject(d);
            return;
        }
        (&scale.x)[a] = fmax(fabs((&q.x)[a]), fabs(lhs));
    }
    const uint3 p = coord(id, d);
    for (uint32_t axis = 0; axis < 3; ++axis)
        for (uint32_t side = 0; side < 2; ++side) {
            uint3 neighbor = p, facePoint = p;
            if (side) {
                (&neighbor.x)[axis]++;
                (&facePoint.x)[axis]++;
            } else
                (&neighbor.x)[axis]--;
            const double rate = flow(facePoint, axis, d) * (side ? -1 : 1);
            if ((&neighbor.x)[axis] >= (&d.grid.x)[axis] || rate <= 0)
                continue;
            const auto term = multiply(d.ping[index(neighbor, d)], rate);
            for (uint32_t a = 0; a < 4; ++a) {
                if (!isfinite((&term.x)[a])) {
                    reject(d);
                    return;
                }
                (&scale.x)[a] = fmax((&scale.x)[a], fabs((&term.x)[a]));
            }
        }
    double error = 0;
    for (uint32_t a = 0; a < 4; ++a) {
        const double difference = fabs(d.diagonal[id] * (&value.x)[a] - (&rhs.x)[a]);
        const double component = (&scale.x)[a] > 0 ? difference / (&scale.x)[a] : 0;
        if (!isfinite(component)) {
            reject(d);
            return;
        }
        error = fmax(error, component);
    }
    if (!isfinite(error)) {
        reject(d);
        return;
    }
    maximum(&d.control->workingResidual, error);
}
__global__ void decide(Device d) {
    if (!d.control->active)
        return;
    auto &c = *d.control;
    c.metrics.residual = c.workingResidual;
    c.workingResidual = 0;
    c.active = !*d.failure && c.metrics.residual > d.tolerance;
    c.metrics.converged = !*d.failure && !c.active;
}
__global__ void finish(Device d) {
    if (d.control->active) {
        d.control->metrics.capped = 1;
        d.control->metrics.converged = 0;
        atomicExch(d.failure, 1u);
    }
}
__global__ void validateOutput(Device d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < d.cells) {
        const auto q = multiply(d.ping[id], d.capacity[id].x);
        if (!valid(q)) {
            reject(d);
            return;
        }
        const double excess = fmax(0., q.w - d.capacity[id].x);
        maximum(&d.control->metrics.maximumExcess, excess);
        if (excess > 2 * d.tolerance * d.capacity[id].x)
            reject(d);
    }
    if (id < d.stride * 3) {
        uint32_t a;
        uint3 p;
        if (!internalFace(id, d, a, p))
            return;
        const double q = d.dt * d.rates[id];
        if (q >= 0)
            (&p.x)[a]--;
        const auto flux = multiply(d.ping[index(p, d)], q);
        if (!isfinite(flux.x) || !isfinite(flux.y) || !isfinite(flux.z) || !isfinite(flux.w))
            reject(d);
    }
}
__global__ void publish(Device d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < d.cells)
        d.output[id] = multiply(d.ping[id], d.capacity[id].x);
    if (id >= d.stride * 3)
        return;
    uint32_t a;
    uint3 p;
    if (!internalFace(id, d, a, p)) {
        d.transfers[id] = {};
        return;
    }
    const double q = d.dt * d.rates[id];
    if (q >= 0)
        (&p.x)[a]--;
    d.transfers[id] = multiply(d.ping[index(p, d)], q);
}
__global__ void validateFineRates(detail::Frame f, const float4 *faces, uint32_t *failure) {
    if (*failure)
        return;
    const uint32_t stride = (f.grid.x + 1) * (f.grid.y + 1) * (f.grid.z + 1);
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= stride * 3)
        return;
    const uint32_t axis = id / stride, k = id % stride;
    const uint3 p = make_uint3(k % (f.grid.x + 1), (k / (f.grid.x + 1)) % (f.grid.y + 1),
                               k / ((f.grid.x + 1) * (f.grid.y + 1)));
    uint3 extent = make_uint3(f.grid.x, f.grid.y, f.grid.z);
    (&extent.x)[axis]++;
    // Padding is not a MAC face. All real faces must be valid, including
    // interior fine faces that disappear under restriction; otherwise a bad
    // canonical velocity field could be mistaken for a valid coarse flux.
    if (p.x >= extent.x || p.y >= extent.y || p.z >= extent.z)
        return;
    const float velocity = faces[id].x;
    const bool boundary = (&p.x)[axis] == 0 || (&p.x)[axis] == (&f.grid.x)[axis];
    if (!isfinite(velocity) || (boundary && velocity != 0))
        atomicExch(failure, 1u);
}
__global__ void restrictRates(Device d, detail::Frame f, const float4 *faces, double *rates) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.stride * 3)
        return;
    uint32_t axis;
    uint3 p;
    const bool internal = internalFace(id, d, axis, p);
    uint3 extent = d.grid;
    (&extent.x)[axis]++;
    if (p.x >= extent.x || p.y >= extent.y || p.z >= extent.z) {
        rates[id] = 0;
        return;
    }
    const uint32_t u = (axis + 1) % 3, v = (axis + 2) % 3;
    double rate = 0;
    for (uint32_t j = 0; j < 2; ++j)
        for (uint32_t i = 0; i < 2; ++i) {
            uint3 q = make_uint3(p.x * 2, p.y * 2, p.z * 2);
            if ((&p.x)[axis] == (&d.grid.x)[axis])
                (&q.x)[axis] = (&f.grid.x)[axis];
            (&q.x)[u] += i;
            (&q.x)[v] += j;
            if ((&q.x)[u] >= (&f.grid.x)[u] || (&q.x)[v] >= (&f.grid.x)[v])
                continue;
            const double velocity = faces[detail::faceIndex(make_int3(q.x, q.y, q.z), int(axis), f)].x;
            if (!isfinite(velocity) || (!internal && velocity != 0)) {
                atomicExch(d.failure, 1u);
                return;
            }
            rate += velocity * double(f.minimumCell.w) * double(f.minimumCell.w);
        }
    if (!isfinite(rate)) {
        atomicExch(d.failure, 1u);
        return;
    }
    rates[id] = rate;
}
} // namespace
struct OwnedTransport {
    OwnedTransportConfig config;
    uint32_t cells, stride;
    Quantity *ping = nullptr, *pong = nullptr;
    double *diagonal = nullptr;
    Control *control = nullptr;
    ~OwnedTransport() {
        cudaFree(ping);
        cudaFree(pong);
        cudaFree(diagonal);
        cudaFree(control);
    }
};
OwnedTransport *createOwnedTransport(const OwnedTransportConfig &c) {
    if (!c.nx || !c.ny || !c.nz || c.nx > 1048576 || c.ny > 1048576 || c.nz > 1048576 ||
        uint64_t(c.nx) * c.ny * c.nz > 1048576 || !c.maxIterations || c.maxIterations > 256 ||
        (c.maxIterations & 1) || !std::isfinite(c.residualTolerance) || c.residualTolerance <= 0 ||
        c.residualTolerance > 2e-13)
        throw std::runtime_error("Invalid CUDA owned transport configuration");
    auto p = std::make_unique<OwnedTransport>();
    p->config = c;
    p->cells = c.nx * c.ny * c.nz;
    p->stride = (c.nx + 1) * (c.ny + 1) * (c.nz + 1);
    check(cudaMalloc(&p->ping, size_t(p->cells) * 32), "CUDA owned transport ping");
    check(cudaMalloc(&p->pong, size_t(p->cells) * 32), "CUDA owned transport pong");
    check(cudaMalloc(&p->diagonal, size_t(p->cells) * 8), "CUDA owned transport diagonal");
    check(cudaMalloc(&p->control, sizeof(Control)), "CUDA owned transport control");
    return p.release();
}
void destroyOwnedTransport(OwnedTransport *p) noexcept {
    delete p;
}
size_t ownedTransportBytes(const OwnedTransport *p) {
    return p ? size_t(p->cells) * 72 + sizeof(Control) : 0;
}
const void *ownedTransportSolution(const OwnedTransport *p) {
    return p ? p->ping : nullptr;
}
const OwnedTransportMetrics *ownedTransportMetrics(const OwnedTransport *p) {
    return p ? reinterpret_cast<const OwnedTransportMetrics *>(p->control) : nullptr;
}
void enqueueOwnedTransport(OwnedTransport *p, void *stream, const OwnedTransportInputs &v, float dt) {
    if (!p || !stream || !std::isfinite(dt) || dt <= 0)
        throw std::runtime_error("Invalid CUDA owned transport invocation");
    const void *buffers[]{v.quantity, v.capacity, v.rates, v.output, v.transfers, v.failure};
    for (uint32_t i = 0; i < 6; ++i) {
        if (!buffers[i])
            throw std::runtime_error("Missing CUDA owned transport buffer");
        for (uint32_t j = 0; j < i; ++j)
            if (buffers[i] == buffers[j])
                throw std::runtime_error("Aliased CUDA owned transport buffers");
    }
    Device d{make_uint3(p->config.nx, p->config.ny, p->config.nz),
             p->cells,
             p->stride,
             double(dt),
             p->config.residualTolerance,
             static_cast<const Quantity *>(v.quantity),
             static_cast<const double2 *>(v.capacity),
             static_cast<const double *>(v.rates),
             p->ping,
             p->pong,
             static_cast<Quantity *>(v.output),
             static_cast<Quantity *>(v.transfers),
             p->diagonal,
             p->control,
             v.failure};
    const auto s = static_cast<cudaStream_t>(stream);
    const uint32_t cells = (p->cells + 127) / 128, faces = (p->stride * 3 + 127) / 128;
    clear<<<1, 1, 0, s>>>(d);
    validateRates<<<faces, 128, 0, s>>>(d);
    initialize<<<cells, 128, 0, s>>>(d);
    residual<<<cells, 128, 0, s>>>(d);
    decide<<<1, 1, 0, s>>>(d);
    for (uint32_t i = 0; i < p->config.maxIterations; i += 2) {
        iterate<<<cells, 128, 0, s>>>(d, false);
        iterate<<<cells, 128, 0, s>>>(d, true);
        residual<<<cells, 128, 0, s>>>(d);
        decide<<<1, 1, 0, s>>>(d);
    }
    finish<<<1, 1, 0, s>>>(d);
    validateOutput<<<std::max(cells, faces), 128, 0, s>>>(d);
    publish<<<std::max(cells, faces), 128, 0, s>>>(d);
    check(cudaGetLastError(), "CUDA owned conservative transport kernels");
}
void enqueueOwnedTransportRates(OwnedTransport *p, void *stream, const void *rawFrame, const void *fineFaces,
                                void *rates, uint32_t *failure) {
    if (!p)
        throw std::runtime_error("Missing CUDA owned face restriction workspace");
    enqueueOwnedTransportRates(p->config, stream, rawFrame, fineFaces, rates, failure);
}
void enqueueOwnedTransportRates(const OwnedTransportConfig &c, void *stream, const void *rawFrame,
                                const void *fineFaces, void *rates, uint32_t *failure) {
    if (!stream || !rawFrame || !fineFaces || !rates || !failure || fineFaces == rates ||
        fineFaces == failure || rates == failure)
        throw std::runtime_error("Invalid CUDA owned face restriction buffers");
    detail::Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    if (!f.grid.x || !f.grid.y || !f.grid.z || f.grid.x > 1048576 || f.grid.y > 1048576 ||
        f.grid.z > 1048576 || uint64_t(f.grid.x) * f.grid.y * f.grid.z != f.grid.w || f.grid.w > 1048576 ||
        (f.grid.x + 1) / 2 != c.nx || (f.grid.y + 1) / 2 != c.ny || (f.grid.z + 1) / 2 != c.nz ||
        !std::isfinite(f.minimumCell.w) || f.minimumCell.w <= 0)
        throw std::runtime_error("CUDA owned face restriction grid mismatch");
    Device d{};
    d.grid = make_uint3(c.nx, c.ny, c.nz);
    d.stride = (c.nx + 1) * (c.ny + 1) * (c.nz + 1);
    d.failure = failure;
    const uint32_t fineFacesCount = 3 * (f.grid.x + 1) * (f.grid.y + 1) * (f.grid.z + 1);
    validateFineRates<<<(fineFacesCount + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(
        f, static_cast<const float4 *>(fineFaces), failure);
    restrictRates<<<(d.stride * 3 + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(
        d, f, static_cast<const float4 *>(fineFaces), static_cast<double *>(rates));
    check(cudaGetLastError(), "CUDA owned canonical MAC flux restriction");
}
} // namespace lab::cuda_fluid
