#pragma once
#include <cstdint>
#include <cstddef>
namespace lab::cuda_fluid {
struct GeometricTransport;
struct GeometricTransportConfig {
    uint32_t nx, ny, nz; // fine MAC dimensions; owners occupy ceil(fine/2)
    uint32_t maxSubsteps = 16;
    uint32_t maxCapacityIterations = 128; // receiver admission, not pressure iterations
    bool conditionalGraphs = true;        // device early exit during captured replay
};
struct GeometricTransportInputs {
    const void *quantity; // FP64 double4 grid-owned quantity
    const void *phase;    // FP64 double2: total liquid volume, geometric capacity
    const void *planes;   // optional first-step PLIC cache, double4
    const void *rates;    // FP64 full MAC volume rates on the coarse owner lattice
    void *output;         // distinct staged double4 grid-owned destination
    void *transfers;      // accumulated double4 per face
    uint32_t *failure;
};
enum GeometricFailureStage : uint32_t {
    GeometricValid,
    GeometricInput,
    GeometricCourant,
    GeometricRate,
    GeometricPlaneFinite,
    GeometricPlaneVolume,
    GeometricSweep,
    GeometricCandidate,
    GeometricLowOrderBounds,
    GeometricDonorBounds,
    GeometricBoundary,
    GeometricEmptyDonor,
    GeometricUpdateBounds,
    GeometricCapacityConvergence
};
struct GeometricTransportMetrics {
    uint32_t substeps, completed, invalid, capped;
    double maximumCourant, maximumExcess;
    // First invalid check and its cell/face index, retained on device for a
    // deferred diagnostic read. Zero means no geometric-transport rejection.
    uint32_t failureStage, failureIndex;
    uint32_t capacityIterations, capacityCapped;
    double maximumAdmissionError, maximumFluxReduction;
};
static_assert(sizeof(GeometricTransportMetrics) == 64);
GeometricTransport *createGeometricTransport(const GeometricTransportConfig &);
void destroyGeometricTransport(GeometricTransport *) noexcept;
size_t geometricTransportBytes(const GeometricTransport *);
uint64_t geometricTransportCapturedBodyNodes(const GeometricTransport *);
const GeometricTransportMetrics *geometricTransportMetrics(const GeometricTransport *);
// Finite-domain geometric VOF transport, shared conservative face fluxes and
// bounded GPU CFL subdivision. Static Cartesian capacities only; no moving GCL.
// Total phase is a non-owning transport guide, not a second physical inventory.
void enqueueGeometricTransport(GeometricTransport *, void *stream, const void *frame,
                               const GeometricTransportInputs &);
} // namespace lab::cuda_fluid
