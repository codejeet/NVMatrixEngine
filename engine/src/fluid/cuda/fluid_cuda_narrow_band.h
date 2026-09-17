#pragma once
#include "fluid_cuda_kernels.h"
namespace lab::cuda_fluid {
struct Transfer;
struct NarrowBand;
// Particle and grid quantities are the ONLY owners. Retired particle caches
// are cleared, not left active as a second representation of the same water.
struct NarrowBandMetrics {
    uint32_t retired, restored, deferred, activeParticles;
    uint32_t gridCells, changed, invalid, reserved;
    double gridVolume, particleVolume;
};
NarrowBand *createNarrowBand(const Config &, void *const (&buffers)[BufferCount], const Ownership &,
                             const GridInventory &, const void *phase, void *previousPositions,
                             uint32_t *failure);
void destroyNarrowBand(NarrowBand *) noexcept;
size_t narrowBandBytes(const NarrowBand *);
const NarrowBandMetrics *narrowBandMetrics(const NarrowBand *);
void resetNarrowBand(NarrowBand *, void *stream, const void *frame);
void beginNarrowBandFrame(NarrowBand *, void *stream, const void *frame);
// Current bins and ledger velocities are required. Decisions have separate
// generations: padding cannot read partially updated neighboring decisions.
// reusablePrefix protects future DX12 emitter birth IDs.
void exchangeNarrowBand(NarrowBand *, void *stream, const void *frame, const void *colliders,
                        const float *mesh, uint32_t reusablePrefix, bool forceParticles = false,
                        bool advanceAge = true);
// Statistics use the accepted ownership stores, not inactive particle caches.
void measureNarrowBand(NarrowBand *, void *stream, const void *frame);
} // namespace lab::cuda_fluid
