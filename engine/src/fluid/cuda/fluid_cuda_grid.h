#pragma once
#include "fluid_cuda_kernels.h"
namespace lab::cuda_fluid {
enum class GridStage {
    Classify,
    Forces,
    Viscosity,
    SurfaceColor,
    SurfaceCurvature,
    MaterialFixture,
    Divergence,
    Jacobi,
    Project,
    Measure,
    Extrapolate,
    DensityGather,
    DensityGatherAdaptive,
    DensityDisplace,
    DensityMeasure,
    DensityClearArguments,
    DensityContinueArguments,
    DensityPrepareArguments
};
// Logical Faces/Scratch and Pressure0/1 may be rebound by the caller; no copies
// or allocations are required to ping-pong. Conditional work reads the existing
// GPU density flag, never a CPU readback. CUDA graphs can replace launch overhead
// later without changing these numerical kernels.
void enqueueGrid(void *stream, void *const (&buffers)[BufferCount], const void *frame, GridStage,
                 uint32_t pressureIndex = 0, bool conditional = false, const uint32_t *failure = nullptr,
                 const void *cellMass = nullptr, const void *narrowGrid = nullptr);
} // namespace lab::cuda_fluid
