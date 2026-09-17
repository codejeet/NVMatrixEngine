#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lab {
namespace lensMapping {
using std::asin;
using std::atan;
using std::sin;
using std::tan;
#include "../shaders/lens-mapping.hlsli"
} // namespace lensMapping
// A full-frame equisolid-angle lens: r = 2 f sin(theta/2).
// Ray reconstruction sees a matching rectilinear image; presentation resamples
// it into this calibrated sensor projection. Not a polynomial barrel effect.
struct Lens {
    bool fisheye = false;
    float diagonalDegrees = 90;
    float tanHalfVertical(float aspect) const {
        // Both lens modes use the selected diagonal FOV. Fisheye changes the
        // sensor mapping below, not whether the FOV control affects the camera.
        return std::tan(std::clamp(diagonalDegrees, 90.f, 160.f) * .00872664626f) /
               std::sqrt(1 + aspect * aspect);
    }
    void rectilinear(float &x, float &y, float aspect) const {
        if (!fisheye)
            return;
        float r = std::sqrt(x * x * aspect * aspect + y * y) / std::sqrt(1 + aspect * aspect);
        float halfAngle = std::clamp(diagonalDegrees, 90.f, 160.f) * .00872664626f;
        float scale = lensMapping::fisheyeToRectilinearScale(r, halfAngle);
        x *= scale;
        y *= scale;
    }
};
struct ExperienceSettings {
    Lens lens;
    // 0 neon, 1 overhead only, 2 white studio, 3 blackout.
    uint32_t environment = 0;
    bool flashlight = false, firstPerson = false, ballFloats = false;
    uint32_t particleCapacity = 500000;
    float cellSize = .16f, simulationHz = 120;
    bool deepPool = false;
    bool rebuildWater = false;
};
} // namespace lab
