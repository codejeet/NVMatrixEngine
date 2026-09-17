#include <cuda_runtime.h>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {
__global__ void transform(uint32_t *data, uint32_t count, uint32_t round) {
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    uint32_t x = data[i] ^ (0x9e3779b9u + round);
    data[i] = ((x << 7) | (x >> 25)) + i;
}
} // namespace
void cudaInteropTransform(void *stream, void *data, uint32_t count, uint32_t round) {
    transform<<<(count + 127) / 128, 128, 0, static_cast<cudaStream_t>(stream)>>>(
        static_cast<uint32_t *>(data), count, round);
    const auto error = cudaGetLastError();
    if (error != cudaSuccess)
        throw std::runtime_error(std::string("CUDA interop test launch: ") + cudaGetErrorString(error));
}
