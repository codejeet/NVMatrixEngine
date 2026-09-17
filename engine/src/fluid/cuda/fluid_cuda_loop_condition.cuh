#pragma once
#include <cuda_runtime.h>
#include <cstdint>
namespace lab::cuda_fluid {
// Implemented alongside pressure's handle kernel in the existing separately
// instrumentable translation unit. Numerical kernels do not call this API.
void enqueueCudaLoopCondition(cudaStream_t, const uint32_t *failure, const uint32_t *running,
                              cudaGraphConditionalHandle);
} // namespace lab::cuda_fluid
