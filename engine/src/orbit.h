#pragma once
#include <algorithm>
#include <cmath>

namespace lab {
// Integrate mouse displacement at rendering cadence, rather than snapping the
// camera on each device packet. A short 6 ms time constant bridges packet gaps;
// no prediction/overshoot, frame-dependent gain, or lost displacement on release.
class OrbitInput {
  public:
    void add(float dx, float dy) {
        x += dx;
        y += dy;
    }
    void clear() {
        x = y = 0;
    }
    void step(float dt, float &yaw, float &pitch, bool playable) {
        const float alpha = -std::expm1(-std::max(dt, 0.f) / .006f);
        float dx = x * alpha, dy = y * alpha;
        x -= dx;
        y -= dy;
        yaw += dx * (playable ? -.005f : .005f);
        float next = pitch + dy * .005f;
        pitch = std::clamp(next, playable ? -1.35f : .05f, 1.4f);
        if (pitch != next)
            y = 0; // Never retain input pushing against the elevation limit.
        if (std::abs(x) < .00001f)
            x = 0;
        if (std::abs(y) < .00001f)
            y = 0;
    }

  private:
    float x = 0, y = 0;
};
} // namespace lab
