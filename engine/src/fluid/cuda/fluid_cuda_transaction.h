#pragma once
#include "fluid_cuda_kernels.h"
namespace lab::cuda_fluid {
// A bounded private GPU working set. Snapshot and publication share a stream
// with simulation; the renderer can never observe a partially accepted frame.
struct Transaction;
Transaction *createTransaction(const Config &, void *const (&shared)[BufferCount], const uint32_t *failure,
                               const Ownership & = {}, const GridInventory & = {},
                               const SurfaceGeometry & = {}, void *narrowPreviousPositions = nullptr);
void destroyTransaction(Transaction *) noexcept;
size_t transactionBytes(const Transaction *);
void transactionBuffers(const Transaction *, void *(&working)[BufferCount]);
Ownership transactionOwnership(const Transaction *);
GridInventory transactionGridInventory(const Transaction *);
void *transactionNarrowPrevious(const Transaction *);
void snapshotTransaction(Transaction *, void *stream);
// Export logical Faces/Scratch and the selected pressure bank to canonical
// shared views. The public State is always {false, 0} for this mode.
void publishTransaction(Transaction *, void *stream, void *const (&working)[BufferCount],
                        uint32_t pressureIndex, const SurfaceGeometry &sources = {});
} // namespace lab::cuda_fluid
