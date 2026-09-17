#pragma once
#include <cstddef>
#include <cstdint>

namespace lab::cuda_fluid {
// Same FP64 ownership quantities as FluidParticleGridExchange: xyz are
// volume-weighted velocity, w is liquid rest volume. Never pass the bulk replica.
struct OwnedTransportConfig {
    uint32_t nx = 0, ny = 0, nz = 0;
    uint32_t maxIterations = 256; // paired Jacobi; fixed allocation/capture shape
    double residualTolerance = 2e-13;
};
struct OwnedTransportInputs {
    const void *quantity = nullptr; // double4 per cell; immutable source owner
    const void *capacity = nullptr; // double2 per cell: endpoint/start open m^3
    const void *rates = nullptr;    // double per MAC face, signed m^3/second
    void *output = nullptr;         // double4 per cell; distinct destination owner
    void *transfers = nullptr;      // double4 per face, positive left -> right
    uint32_t *failure = nullptr;    // caller-owned sticky publication latch
};
struct OwnedTransportMetrics {
    uint32_t iterations, converged, invalid, capped;
    double residual, maximumExcess;
};
static_assert(sizeof(OwnedTransportMetrics) == 32);
struct OwnedTransport;
OwnedTransport *createOwnedTransport(const OwnedTransportConfig &);
void destroyOwnedTransport(OwnedTransport *) noexcept; // caller drains its stream
size_t ownedTransportBytes(const OwnedTransport *);
const void *ownedTransportSolution(const OwnedTransport *);                 // FP64 concentration
const OwnedTransportMetrics *ownedTransportMetrics(const OwnedTransport *); // GPU pointer
// Capturable, GPU-only work. Neither source, output nor face transfers changes
// on rejection. The caller checks failure at its existing completion boundary.
// This is quantity transport, not a whole coupled FLIP step or a surface model.
void enqueueOwnedTransport(OwnedTransport *, void *stream, const OwnedTransportInputs &, float dt);
// Restrict the actual projected fine MAC face velocities by face area onto
// the exchange's 2h lattice. This is not a new velocity solve: callers still
// need capacity-compatible fluxes for full cells and moving geometry.
void enqueueOwnedTransportRates(OwnedTransport *, void *stream, const void *fineFrame, const void *fineFaces,
                                void *rates, uint32_t *failure);
// Shares the restriction kernel without allocating implicit-solver workspace.
void enqueueOwnedTransportRates(const OwnedTransportConfig &, void *stream, const void *fineFrame,
                                const void *fineFaces, void *rates, uint32_t *failure);
} // namespace lab::cuda_fluid
