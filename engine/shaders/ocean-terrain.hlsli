#ifndef OCEAN_TERRAIN
#define OCEAN_TERRAIN
#ifdef __cplusplus
#include <cmath>
namespace lab::ocean {
#define OCEAN_INLINE inline
using std::sin;
#else
#define OCEAN_INLINE
#endif
// Metres, shared by the render mesh, Bullet terrain and fluid boundary.
OCEAN_INLINE float oceanTerrainHeight(float x, float z) {
    float u = (x + 22 + 2 * sin(z * .09f)) / 28;
    float v = (z + 18 + 1.5f * sin(x * .11f)) / 23;
    float t = 1 - (u * u + v * v) / 2.2f;
    t = t > 0 ? t : 0;
    return 10.5f * t * t;
}
#ifdef __cplusplus
}
#endif
#undef OCEAN_INLINE
#endif
