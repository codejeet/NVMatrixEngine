#ifndef OCEAN_LIGHTS
#define OCEAN_LIGHTS
#include "ocean-terrain.hlsli"
#ifdef __cplusplus
#include <DirectXMath.h>
namespace lab::ocean {
#define OCEAN_LIGHT_INLINE inline
#define OCEAN_LIGHT_VECTOR DirectX::XMFLOAT3
#else
#define OCEAN_LIGHT_INLINE
#define OCEAN_LIGHT_VECTOR float3
#endif
// Shared positions for visible emitters, visibility sampling and solid posts.
static const unsigned int OCEAN_LANTERN_COUNT = 5;
static const float OCEAN_LANTERN_RADIUS = .15f;
static const float OCEAN_LANTERN_LUMENS = 12.f; // Small candle lanterns.
OCEAN_LIGHT_INLINE OCEAN_LIGHT_VECTOR oceanLanternPosition(unsigned int i) {
    if (i == 0) return OCEAN_LIGHT_VECTOR(-3.f, 8.09f, -8.75f);
    if (i == 1) return OCEAN_LIGHT_VECTOR(5.f, 8.09f, -8.75f);
    float x = i == 2 ? -9.f : i == 3 ? -19.f : -29.f;
    float z = i == 2 ? -10.f : i == 3 ? -11.f : -18.f;
    return OCEAN_LIGHT_VECTOR(x, oceanTerrainHeight(x, z) + 1.55f, z);
}
#ifdef __cplusplus
}
#else
bool oceanLanternMaterial(uint material) { return material >= 24 && material < 24 + OCEAN_LANTERN_COUNT; }
float3 oceanLanternRadiance() {
    // RGB source approximation, normalized by photopic luminance. Exposure and
    // the engine's white-spectrum luminous efficacy match the HDR environment.
    float3 color = float3(1, .57, .28);
    float power = OCEAN_LANTERN_LUMENS * OceanEnvironment[3].y / 182.458;
    return color * (power / (dot(color, float3(.2126,.7152,.0722)) *
                            4 * PI * PI * OCEAN_LANTERN_RADIUS * OCEAN_LANTERN_RADIUS));
}
#endif
#undef OCEAN_LIGHT_INLINE
#undef OCEAN_LIGHT_VECTOR
#endif
