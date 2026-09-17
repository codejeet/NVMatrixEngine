#include "fluid_cuda_joint.h"
#include "fluid_cuda_device.cuh"
#include "fluid_cuda_transfer.h"
#include "fluid_cuda_owned_transport.h"
#include "fluid_cuda_geometric_transport.h"
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
using namespace detail;
struct alignas(16) Quantity {
    double v[4];
};
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
struct Views {
    uint3 coarse;
    const double *open;
    const Quantity *grid, *particles;
    const uint32_t *offsets, *indices;
    double2 *capacity;
    uint32_t *failure;
    bool narrowBand = false;
};
__global__ void capacity(Frame f, Views d) {
    if (*d.failure)
        return;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= d.coarse.x * d.coarse.y * d.coarse.z)
        return;
    const uint3 p =
        make_uint3(id % d.coarse.x, (id / d.coarse.x) % d.coarse.y, id / (d.coarse.x * d.coarse.y));
    double open = 0, particleVolume = 0;
    const double cellVolume = double(f.minimumCell.w) * f.minimumCell.w * f.minimumCell.w;
    for (uint32_t k = 0; k < 8; ++k) {
        const uint3 q = make_uint3(2 * p.x + (k & 1), 2 * p.y + ((k >> 1) & 1), 2 * p.z + (k >> 2));
        if (q.x >= f.grid.x || q.y >= f.grid.y || q.z >= f.grid.z)
            continue;
        const uint32_t cell = (q.z * f.grid.y + q.y) * f.grid.x + q.x;
        const double v = d.open[cell];
        if (!isfinite(v) || v < 0 || v > cellVolume * (1 + 2e-13)) {
            atomicExch(d.failure, 1u);
            return;
        }
        open += v;
        for (uint32_t j = d.offsets[cell]; j < d.offsets[cell + 1]; ++j)
            particleVolume += d.particles[d.indices[j]].v[3];
    }
    const double combined = particleVolume + d.grid[id].v[3];
    if (!isfinite(combined) || combined < 0 || (!d.narrowBand && combined > open * (1 + 2e-13)) ||
        d.grid[id].v[3] > open * (1 + 2e-13)) {
        atomicExch(d.failure, 1u);
        return;
    }
    d.capacity[id] = make_double2(open, open);
}
} // namespace
struct Joint {
    Views views{};
    GeometricTransport *geometric = nullptr;
    void *returned = nullptr, *rates = nullptr, *flux = nullptr;
    size_t bytes = 0;
    ~Joint() {
        destroyGeometricTransport(geometric);
        for (void *v : {returned, rates, flux, static_cast<void *>(views.capacity)})
            if (v)
                cudaFree(v);
    }
};
Joint *createJoint(const Config &c, void *const (&buffers)[BufferCount], const Ownership &owned,
                   const GridInventory &grid, uint32_t *failure) {
    bufferBytes(c, Particles);
    validateOwnership(c, owned);
    if (!validateGridInventory(c, grid) || !failure)
        throw std::runtime_error("Joint CUDA solver requires inventory and failure latch");
    if (!buffers[Offsets] || !buffers[Indices])
        throw std::runtime_error("Joint CUDA solver requires particle bins");
    auto p = std::make_unique<Joint>();
    const uint3 coarse = make_uint3((c.nx + 1) / 2, (c.ny + 1) / 2, (c.nz + 1) / 2);
    const size_t cells = size_t(coarse.x) * coarse.y * coarse.z;
    const size_t faces = size_t(coarse.x + 1) * (coarse.y + 1) * (coarse.z + 1) * 3;
    p->views = {coarse,
                static_cast<const double *>(grid[GridFineCapacity]),
                static_cast<const Quantity *>(grid[GridOwnedQuantity]),
                static_cast<const Quantity *>(owned[OwnedQuantity]),
                static_cast<const uint32_t *>(buffers[Offsets]),
                static_cast<const uint32_t *>(buffers[Indices]),
                nullptr,
                failure};
    p->views.narrowBand = c.narrowBand;
    check(cudaMalloc(&p->returned, cells * 32), "CUDA staged grid momentum return");
    check(cudaMalloc(&p->views.capacity, cells * 16), "CUDA joint geometric capacity");
    check(cudaMalloc(&p->rates, faces * 8), "CUDA joint restricted rates");
    check(cudaMalloc(&p->flux, faces * 32), "CUDA joint conservative face quantities");
    GeometricTransportConfig transport{c.nx, c.ny, c.nz};
    transport.conditionalGraphs = c.pressureConditionalGraphs;
    p->geometric = createGeometricTransport(transport);
    p->bytes = cells * 48 + faces * 40 + geometricTransportBytes(p->geometric);
    return p.release();
}
void destroyJoint(Joint *p) noexcept {
    delete p;
}
size_t jointBytes(const Joint *p) {
    return p ? p->bytes : 0;
}
uint64_t jointCapturedBodyNodes(const Joint *p) {
    return p ? geometricTransportCapturedBodyNodes(p->geometric) : 0;
}
const GeometricTransportMetrics *jointGeometricMetrics(const Joint *p) {
    return p ? geometricTransportMetrics(p->geometric) : nullptr;
}
void enqueueJointCapacity(Joint *p, void *stream, const void *rawFrame) {
    if (!p || !stream || !rawFrame)
        throw std::runtime_error("Invalid CUDA joint capacity invocation");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    const auto c = p->views.coarse;
    capacity<<<(c.x * c.y * c.z + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(f, p->views);
    check(cudaGetLastError(), "CUDA combined grid/particle capacity audit");
}
void enqueueJointTransport(Joint *p, Transfer *transfer, void *stream, const void *rawFrame, void *faces) {
    if (!p || !stream || !rawFrame)
        throw std::runtime_error("Invalid CUDA joint transport invocation");
    Frame f;
    std::memcpy(&f, rawFrame, sizeof(f));
    enqueueGridOwnershipReturn(transfer, stream, rawFrame, p->returned, faces);
    const auto c = p->views.coarse;
    enqueueOwnedTransportRates({c.x, c.y, c.z}, stream, rawFrame, faces, p->rates, p->views.failure);
    enqueueGeometricTransport(p->geometric, stream, rawFrame,
                              {p->returned, gridOwnershipPhase(transfer), gridOwnershipPlanes(transfer),
                               p->rates, const_cast<Quantity *>(p->views.grid), p->flux, p->views.failure});
}
} // namespace lab::cuda_fluid
