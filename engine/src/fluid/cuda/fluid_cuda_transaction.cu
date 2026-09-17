#include "fluid_cuda_transaction.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

namespace lab::cuda_fluid {
namespace {
constexpr uint32_t gridField = BufferCount + OwnershipBufferCount;
constexpr uint32_t surfaceField = gridField + GridInventoryBufferCount;
constexpr uint32_t maximumFields = surfaceField + SurfaceGeometryBufferCount;
void check(cudaError_t e, const char *where) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string(where) + ": " + cudaGetErrorString(e));
}
struct Copy {
    const uint32_t *source[maximumFields];
    uint32_t *destination[maximumFields], words[maximumFields], fields;
    const uint32_t *failure;
};
__global__ void copyFields(Copy c) {
    if (*c.failure)
        return;
    const uint32_t field = blockIdx.y;
    for (uint32_t i = blockIdx.x * blockDim.x + threadIdx.x; i < c.words[field]; i += gridDim.x * blockDim.x)
        c.destination[field][i] = c.source[field][i];
}
void enqueueCopy(Copy c, cudaStream_t stream) {
    uint32_t maximum = 0;
    for (auto words : c.words)
        maximum = std::max(maximum, words);
    copyFields<<<dim3(std::min((maximum + 255) / 256, 512u), c.fields), 256, 0, stream>>>(c);
    check(cudaGetLastError(), "CUDA transactional GPU field copy");
}
} // namespace
size_t bufferBytes(const Config &c, Buffer buffer) {
    if (!c.nx || !c.ny || !c.nz || c.nx > 1048576 || c.ny > 1048576 || c.nz > 1048576 ||
        uint64_t(c.nx) * c.ny * c.nz > 1048576 || !c.capacity || c.capacity > 1048576 ||
        buffer >= BufferCount)
        throw std::runtime_error("Invalid CUDA buffer layout");
    const size_t cells = size_t(c.nx) * c.ny * c.nz;
    const size_t faces = size_t(c.nx + 1) * (c.ny + 1) * (c.nz + 1) * 3;
    switch (buffer) {
    case Particles:
        return size_t(c.capacity) * 80;
    case Counts:
    case Cursors:
    case Pressure0:
    case Pressure1:
        return cells * 4;
    case Offsets:
        return (cells + 1) * 4;
    case Indices:
        return size_t(c.capacity) * 4;
    case Faces:
    case Scratch:
        return faces * 16;
    case Cells:
    case Density:
    case Solid:
    case Material:
        return cells * 16;
    case DensityArguments:
        return 256;
    default:
        throw std::runtime_error("Unknown CUDA buffer layout");
    }
}
size_t gridInventoryBytes(const Config &c, GridInventoryBuffer buffer) {
    bufferBytes(c, Particles); // Validate before dimension arithmetic.
    if (buffer == GridOwnedQuantity)
        return size_t((c.nx + 1) / 2) * ((c.ny + 1) / 2) * ((c.nz + 1) / 2) * 32;
    if (buffer == GridFineCapacity)
        return size_t(c.nx) * c.ny * c.nz * 8;
    throw std::runtime_error("Unknown CUDA grid inventory field");
}
bool validateGridInventory(const Config &c, const GridInventory &v) {
    if (!v[0] && !v[1])
        return false;
    if (!c.ownedParticles || !v[0] || !v[1] || v[0] == v[1])
        throw std::runtime_error(
            "CUDA grid inventory requires distinct complete views and particle ownership");
    return true;
}
size_t surfaceGeometryBytes(const Config &c, SurfaceGeometryBuffer buffer) {
    if (buffer >= SurfaceGeometryBufferCount)
        throw std::runtime_error("Unknown CUDA surface geometry field");
    return gridInventoryBytes(c, GridOwnedQuantity) / 32 * (buffer == SurfacePhase ? 16 : 32);
}
bool validateSurfaceGeometry(const Config &c, const GridInventory &grid, const SurfaceGeometry &v) {
    if (!v[0] && !v[1])
        return false;
    if (!validateGridInventory(c, grid) || !v[0] || !v[1] || v[0] == v[1])
        throw std::runtime_error("CUDA surface outputs require joint ownership and distinct complete views");
    return true;
}
struct Transaction {
    void *working[maximumFields]{}, *shared[maximumFields]{};
    uint32_t words[maximumFields]{}, fields = BufferCount;
    bool surface = false, narrow = false;
    const uint32_t *failure = nullptr;
    size_t bytes = 0;
    ~Transaction() {
        for (void *buffer : working)
            if (buffer)
                cudaFree(buffer);
    }
};
Transaction *createTransaction(const Config &c, void *const (&shared)[BufferCount], const uint32_t *failure,
                               const Ownership &owned, const GridInventory &grid,
                               const SurfaceGeometry &surface, void *narrowPrevious) {
    if (!failure)
        throw std::runtime_error("CUDA transaction requires a GPU failure latch");
    auto p = std::make_unique<Transaction>();
    validateOwnership(c, owned);
    p->fields = validateGridInventory(c, grid) ? surfaceField : c.ownedParticles ? gridField : BufferCount;
    p->surface = validateSurfaceGeometry(c, grid, surface);
    if (c.narrowBand != bool(narrowPrevious) ||
        (c.narrowBand && (!validateGridInventory(c, grid) || p->surface)))
        throw std::runtime_error("Narrow-band transaction requires joint ownership and previous positions, "
                                 "not total-phase geometry");
    p->narrow = c.narrowBand;
    p->failure = failure;
    for (uint32_t i = 0; i < p->fields; ++i) {
        void *source = i < BufferCount ? shared[i]
                       : i < gridField ? owned[i - BufferCount]
                                       : grid[i - gridField];
        if (!source)
            throw std::runtime_error("Missing CUDA transaction input");
        for (uint32_t j = 0; j < i; ++j)
            if (source == p->shared[j])
                throw std::runtime_error("Aliased CUDA transaction fields");
        const size_t bytes = i < BufferCount ? bufferBytes(c, Buffer(i))
                             : i < gridField ? ownershipBytes(c, OwnershipBuffer(i - BufferCount))
                                             : gridInventoryBytes(c, GridInventoryBuffer(i - gridField));
        p->shared[i] = source;
        p->words[i] = uint32_t(bytes / 4);
        check(cudaMalloc(p->working + i, bytes), "CUDA persistent transaction working set");
        p->bytes += bytes;
    }
    if (p->narrow) {
        for (uint32_t i = 0; i < p->fields; ++i)
            if (p->shared[i] == narrowPrevious)
                throw std::runtime_error("Aliased narrow-band motion history");
        const size_t bytes = size_t(c.capacity) * 16;
        p->shared[surfaceField] = narrowPrevious;
        p->words[surfaceField] = uint32_t(bytes / 4);
        check(cudaMalloc(p->working + surfaceField, bytes), "CUDA narrow-band motion-history staging");
        p->bytes += bytes;
        ++p->fields;
    }
    if (p->surface)
        for (uint32_t i = 0; i < SurfaceGeometryBufferCount; ++i) {
            for (uint32_t j = 0; j < surfaceField + i; ++j)
                if (surface[i] == p->shared[j])
                    throw std::runtime_error("Aliased CUDA surface output and transaction field");
            p->shared[surfaceField + i] = surface[i];
            p->words[surfaceField + i] = uint32_t(surfaceGeometryBytes(c, SurfaceGeometryBuffer(i)) / 4);
        }
    return p.release();
}
void destroyTransaction(Transaction *p) noexcept {
    delete p;
}
size_t transactionBytes(const Transaction *p) {
    return p ? p->bytes : 0;
}
void transactionBuffers(const Transaction *p, void *(&working)[BufferCount]) {
    if (!p)
        throw std::runtime_error("Missing CUDA transaction");
    std::copy_n(p->working, BufferCount, working);
}
Ownership transactionOwnership(const Transaction *p) {
    if (!p)
        throw std::runtime_error("Missing CUDA ownership transaction");
    Ownership v{};
    std::copy_n(p->working + BufferCount, OwnershipBufferCount, v.begin());
    return v;
}
GridInventory transactionGridInventory(const Transaction *p) {
    if (!p)
        throw std::runtime_error("Missing CUDA grid inventory transaction");
    GridInventory v{};
    std::copy_n(p->working + gridField, GridInventoryBufferCount, v.begin());
    return v;
}
void *transactionNarrowPrevious(const Transaction *p) {
    return p && p->narrow ? p->working[surfaceField] : nullptr;
}
void snapshotTransaction(Transaction *p, void *rawStream) {
    if (!p || !rawStream)
        throw std::runtime_error("Invalid CUDA transaction snapshot");
    Copy c{};
    c.failure = p->failure;
    c.fields = p->fields;
    for (uint32_t i = 0; i < p->fields; ++i) {
        c.source[i] = static_cast<const uint32_t *>(p->shared[i]);
        c.destination[i] = static_cast<uint32_t *>(p->working[i]);
        c.words[i] = p->words[i];
    }
    enqueueCopy(c, static_cast<cudaStream_t>(rawStream));
}
void publishTransaction(Transaction *p, void *rawStream, void *const (&working)[BufferCount], uint32_t pi,
                        const SurfaceGeometry &surface) {
    if (!p || !rawStream || pi > 1)
        throw std::runtime_error("Invalid CUDA transaction publication");
    Copy c{};
    c.failure = p->failure;
    c.fields = p->fields;
    for (uint32_t i = 0; i < p->fields; ++i) {
        const uint32_t source = i == Pressure0   ? (pi ? Pressure1 : Pressure0)
                                : i == Pressure1 ? (pi ? Pressure0 : Pressure1)
                                                 : i;
        c.source[i] = static_cast<const uint32_t *>(i < BufferCount ? working[source] : p->working[i]);
        c.destination[i] = static_cast<uint32_t *>(p->shared[i]);
        c.words[i] = p->words[i];
    }
    if (p->surface) {
        // Read the final validated transfer cache directly. Output buffers are
        // never snapshotted/read as inputs and need no duplicate working set.
        for (uint32_t i = 0; i < SurfaceGeometryBufferCount; ++i) {
            if (!surface[i])
                throw std::runtime_error("Missing CUDA surface publication source");
            c.source[surfaceField + i] = static_cast<const uint32_t *>(surface[i]);
            c.destination[surfaceField + i] = static_cast<uint32_t *>(p->shared[surfaceField + i]);
            c.words[surfaceField + i] = p->words[surfaceField + i];
        }
        c.fields = maximumFields;
    }
    enqueueCopy(c, static_cast<cudaStream_t>(rawStream));
}
} // namespace lab::cuda_fluid
