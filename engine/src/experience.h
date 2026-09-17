#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lab {
// A full-frame equisolid-angle lens: r = 2 f sin(theta/2).
// Ray reconstruction sees a matching rectilinear image; presentation resamples
// it into this calibrated sensor projection. Not a polynomial barrel effect.
struct Lens {
    bool fisheye = false;
    float diagonalDegrees = 120;
    float tanHalfVertical(float aspect) const {
        return fisheye ? std::tan(std::clamp(diagonalDegrees, 90.f, 160.f) * .00872664626f) /
                             std::sqrt(1 + aspect * aspect)
                       : .57735026919f;
    }
    void rectilinear(float &x, float &y, float aspect) const {
        if (!fisheye)
            return;
        float r = std::sqrt(x * x * aspect * aspect + y * y) / std::sqrt(1 + aspect * aspect);
        float halfAngle = std::clamp(diagonalDegrees, 90.f, 160.f) * .00872664626f;
        float theta = 2 * std::asin(std::clamp(r * std::sin(halfAngle * .5f), 0.f, .99999f));
        float scale = r > 1e-7f ? std::tan(theta) / (std::tan(halfAngle) * r) : 1;
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
