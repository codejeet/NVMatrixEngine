#pragma once
#include "fluid_cuda_kernels.h"

namespace lab::cuda_fluid {
// Production transfer primitives, also exposed to the cross-backend GPU fixture.
// This is not a complete substep: pressure, material and collisions are separate.
enum class TransferStage { Bin, ToGrid, ToParticles };
struct Transfer;
// Actual exchange grid owners on the ceil(fine/2) lattice, not a bulk replica.
// Within each coarse cell, distribute its mean quantity in proportion to fine
// open volumes. This is a porosity model, not cut-cell surface reconstruction.
struct GridOwnership {
    const void *quantity = nullptr;     // FP64 volume-weighted velocity, volume
    const void *fineCapacity = nullptr; // FP64 open m^3 per fine cell
    uint32_t *failure = nullptr;        // shared sticky transaction latch
    bool geometric = false;             // reconstruct total-phase support on static Cartesian cells
};
Transfer *createTransfer(const Config &, void *const (&buffers)[BufferCount], const Ownership & = {},
                         const GridOwnership & = {});
void destroyTransfer(Transfer *) noexcept; // caller drains its stream first
void enqueueTransfer(Transfer *, void *stream, const void *frame, TransferStage,
                     void *facesOverride = nullptr, const uint32_t *failure = nullptr);
size_t gridOwnershipTransferBytes(const Transfer *);
const void *gridOwnershipPhase(const Transfer *);  // double2: total phase volume / capacity
const void *gridOwnershipPlanes(const Transfer *); // same PLIC interface used for fine support
// After Bin/ToGrid and forces/projection, use the grid forward operator's
// volume-integrated basis and the particle operator's PIC/FLIP blend. The
// distinct destination preserves rest volume; no spatial transport occurs.
// Caller stages particle/grid updates together before whole-step publication.
void enqueueGridOwnershipReturn(Transfer *, void *stream, const void *frame, void *output,
                                void *facesOverride = nullptr);
} // namespace lab::cuda_fluid
