#pragma once
#include "fluid_cuda_kernels.h"
namespace lab::cuda_fluid {
enum class CollisionStage { BakeSolids, Collide };
void enqueueCollision(void *stream, void *const (&buffers)[BufferCount], const void *frame, CollisionStage,
                      const void *deviceColliders, const float *deviceMesh, bool conditional = false,
                      const uint32_t *failure = nullptr);
} // namespace lab::cuda_fluid
