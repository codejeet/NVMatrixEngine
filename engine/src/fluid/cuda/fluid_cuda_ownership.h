#pragma once
#include "fluid_cuda_kernels.h"
namespace lab::cuda_fluid {
// A caller supplies the shared ledger and a transaction-owned failure latch.
// Birth/reset stays with FluidParticleGridExchange in the DX12 prefix.
enum class OwnershipStage { Validate, VelocityDelta };
void enqueueOwnership(void *stream, void *particles, const Ownership &, const void *frame, OwnershipStage,
                      uint32_t *failure);
} // namespace lab::cuda_fluid
