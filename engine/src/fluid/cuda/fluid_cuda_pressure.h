#pragma once
#include <cstddef>
#include <cstdint>
#include "fluid_cuda_kernels.h"
namespace lab::cuda_fluid {
struct MacView;
struct PressureConfig {
    uint32_t nx, ny, nz, maxIterations = 32;
    float relativeTolerance = 1e-5f, divergenceTolerance = 1e-4f;
    bool conditionalGraphs = true;
};
struct PressureLevel {
    uint32_t x, y, z, offset;
};
enum PressureCounter : uint32_t {
    PressureRunning,
    PressureInvalid,
    PressureIterations,
    PressureConverged,
    PressureCapped,
    PressureCalls,
    PressureTotalIterations,
    PressurePeakIterations,
    PressureTotalCapped,
    PressureLoopSolves,
    PressureLoopIterations,
    PressureCacheValid,
    PressureCacheDirty,
    PressureUseWarmStart,
    PressureHierarchyBuilds,
    PressureHierarchyReuses,
    PressureWarmStarts,
    PressureCounterCount
};
// Deferred diagnostics only. No host iteration decisions or GPU data downloads
// are required by enqueuePressure. All arrays are owned by Pressure.
struct PressureView {
    PressureLevel levels[16]{};
    uint32_t levelCount = 0, capacity = 0;
    void *rows = nullptr, *vectors = nullptr, *partials = nullptr;
    float *rhs = nullptr, *correction[2]{}, *fine[2]{}, *factor = nullptr;
    uint32_t *counts = nullptr;
    double *scalars = nullptr, *trace = nullptr;
    // Dense, matrix-free safety projection when the bounded sparse page pool
    // cannot publish its candidate. Only RHS + pressure (16 bytes/fine cell),
    // not a duplicate dense copy of the sparse 240-byte operator records.
    double *fallback = nullptr;
    // Geometric indexing, independent of recycled/double-buffered brick slots.
    // Exact cell tags, not a probabilistic hash. History commits only after the
    // final stored-face divergence gate in enqueueMac.
    uint32_t *topology = nullptr;
    double *previousPressure = nullptr;
    float *previousParameters = nullptr;
};
struct Pressure;
Pressure *createPressure(const PressureConfig &);
void destroyPressure(Pressure *) noexcept;
PressureView pressureView(const Pressure *);
size_t pressureBytes(const Pressure *);
// Cumulative construction metadata, not per-frame device readback. A caller
// takes differences around capture to include nested body nodes in its count.
uint64_t pressureCapturedBodyNodes(const Pressure *);
void resetPressureHistory(Pressure *, void *stream, MacView);
void preparePressure(Pressure *, void *stream, void *const (&buffers)[BufferCount], const void *frame,
                     MacView);
void publishPressureHistory(Pressure *, void *stream, void *const (&buffers)[BufferCount], const void *frame,
                            MacView);
void enqueuePressure(Pressure *, void *stream, void *const (&buffers)[BufferCount], const void *frame,
                     MacView);
} // namespace lab::cuda_fluid
