#pragma once
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>
namespace lab {
using namespace DirectX;
constexpr uint32_t chartSize = 256, chartCount = 5, atlasWidth = chartSize * chartCount;
struct Vertex {
    XMFLOAT3 position;
    uint32_t material;
    XMFLOAT2 uv;
    uint32_t chart, pad;
};
struct Object {
    XMFLOAT4X4 world, previous;
    XMUINT4 info;
};
struct Constants {
    XMFLOAT4 camera, right, up, forward;
    XMFLOAT4X4 viewProjection, previousViewProjection;
    XMUINT4 dimensions; // internal xy, frame, photons
    XMUINT4 controls;   // reset, float atomics, SER enabled, history length
    XMFLOAT4 jitter, lightOrigin, lightDirection, lightRight, lightUp;
    XMFLOAT4 optics; // Cauchy B, C in um^2, absorption / metre, photon flux
    XMFLOAT4 atlas;  // width, height, chart size, fixed-point scale
    XMFLOAT4 play;   // playable scene, receiver charge, elapsed time, reserved
    XMFLOAT4 sensor; // center xyz, half-width (on right receiver wall)
    XMFLOAT4 water;  // enabled, flat reference surface, lasers enabled, laser wavelength nm
    XMFLOAT4 medium; // air scattering / m, water scattering / m, laser power W, water flood W
    XMFLOAT4 fluidMinimumSpacing;
    XMUINT4 fluidBricks, fluidState; // enabled, motion valid, reset, reserved
    XMUINT4 ptControls;
    XMFLOAT4 ptPreviousCamera;
    XMUINT4 opticalControls;
    XMFLOAT4 opticalParameters;
    XMFLOAT4 lighting, flashlightOrigin, flashlightDirection;
    XMUINT4 cameraState;
};
struct Mesh {
    std::vector<Vertex> vertices;
    uint32_t mask;
};
std::vector<Mesh> makeScene();
// The opt-in liquid lab appends a static dielectric tank after the gameplay bodies.
std::vector<Mesh> makePlayScene(bool water = true, bool flat = false, bool glassPit = false,
                                bool fluidRoom = false, bool boat = false, bool deepPool = false,
                                bool largeWaterLab = false, bool oceanLab = false);
XMFLOAT4X4 prismTransform(float angle);
// Conservative world-space displacement bound for a rigid object's bounding sphere.
bool transportMoved(const XMFLOAT4X4 &now, const XMFLOAT4X4 &anchor, float radius);
static_assert(sizeof(Vertex) == 32 && sizeof(Object) == 144 && sizeof(Constants) == 576);
} // namespace lab
