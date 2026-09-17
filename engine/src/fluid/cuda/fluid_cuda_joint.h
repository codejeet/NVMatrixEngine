#pragma once
#include "fluid_cuda_kernels.h"

namespace lab::cuda_fluid {
struct Transfer;
struct Joint;
struct GeometricTransportMetrics;
// Persistent, capturable grid-return/transport workspace. The Solver owns the
// shared failure latch and the transaction containing all authoritative views.
Joint *createJoint(const Config &, void *const (&buffers)[BufferCount], const Ownership &,
                   const GridInventory &, uint32_t *failure);
void destroyJoint(Joint *) noexcept;
size_t jointBytes(const Joint *);
uint64_t jointCapturedBodyNodes(const Joint *);
const GeometricTransportMetrics *jointGeometricMetrics(const Joint *);
// Requires current particle bins. Audits coarse combined occupancy and builds
// geometric capacities on GPU; this guard is not a coupled interface flux solve.
void enqueueJointCapacity(Joint *, void *stream, const void *frame);
void enqueueJointTransport(Joint *, Transfer *, void *stream, const void *frame, void *faces);
} // namespace lab::cuda_fluid
