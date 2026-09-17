#pragma once
#include <DirectXMath.h>

namespace lab {
// Matches FluidFrame in common.hlsli. Shared with hardware transfer fixtures so
// they exercise the production kernels and their actual constant-buffer ABI.
struct FluidSimulationConstants {
    DirectX::XMFLOAT4 minimumCell, maximumRadius, gravityDt;
    DirectX::XMUINT4 counts;
    DirectX::XMFLOAT4X4 viewProjection;
    DirectX::XMFLOAT4 cameraRight, cameraUp, cameraPosition, cameraForward;
    DirectX::XMUINT4 grid;
    DirectX::XMFLOAT4 solver;
    DirectX::XMUINT4 display;
    DirectX::XMUINT4 collision;
    DirectX::XMFLOAT4 material;
    DirectX::XMFLOAT4 initialMinimum, initialMaximum;
    DirectX::XMUINT4 initialLattice, emission;
    DirectX::XMFLOAT4 emitterOriginRadius, emitterVelocity;
};
static_assert(sizeof(FluidSimulationConstants) == 368);
} // namespace lab
