#include "fluid_cuda_pressure_loop.cuh"
#include "fluid_cuda_loop_condition.cuh"

namespace lab::cuda_fluid {
namespace {
__global__ void boundedLoopCondition(const uint32_t *failure, const uint32_t *running,
                                     cudaGraphConditionalHandle handle) {
    if (blockIdx.x || threadIdx.x)
        return;
    cudaGraphSetConditional(handle, !*failure && *running);
}
__global__ void loopCondition(const uint32_t *control, uint32_t *counts, uint32_t maxIterations,
                              cudaGraphConditionalHandle handle, bool completedBody) {
    if (blockIdx.x || threadIdx.x)
        return;
    const bool valid = !control[BrickInvalid];
    if (completedBody)
        ++counts[PressureLoopIterations];
    else if (valid)
        ++counts[PressureLoopSolves];
    // Always set the handle, including empty/rejected work. The one-thread
    // launch and graph dependencies give each handle exactly one writer.
    const bool running = valid && counts[PressureRunning] && !counts[PressureInvalid];
    cudaGraphSetConditional(handle, running && counts[PressureIterations] < maxIterations);
}
} // namespace
void enqueueCudaLoopCondition(cudaStream_t stream, const uint32_t *failure, const uint32_t *running,
                              cudaGraphConditionalHandle handle) {
    boundedLoopCondition<<<1, 1, 0, stream>>>(failure, running, handle);
}
void enqueuePressureLoopCondition(cudaStream_t stream, MacView m, PressureView v, PressureConfig cfg,
                                  cudaGraphConditionalHandle handle, bool completedBody) {
    loopCondition<<<1, 1, 0, stream>>>(m.pool.control, v.counts, cfg.maxIterations, handle, completedBody);
}
} // namespace lab::cuda_fluid
