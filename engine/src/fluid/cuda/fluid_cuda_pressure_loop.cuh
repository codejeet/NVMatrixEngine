#pragma once
#include <cuda_runtime.h>
#include "fluid_cuda_mac.h"
#include "fluid_cuda_pressure.h"

namespace lab::cuda_fluid {
// Isolated compilation unit permits CUDA's supported hybrid memcheck mode:
// compile-time instrumentation for the handle kernel, binary instrumentation
// for the numerical kernels. The production build instruments neither.
void enqueuePressureLoopCondition(cudaStream_t, MacView, PressureView, PressureConfig,
                                  cudaGraphConditionalHandle, bool completedBody);
} // namespace lab::cuda_fluid
