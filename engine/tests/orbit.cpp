#include "../src/orbit.h"
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
double variance(const std::vector<float> &v) {
    double sum = 0, squares = 0;
    for (float x : v) {
        sum += x;
        squares += x * x;
    }
    return squares / v.size() - std::pow(sum / v.size(), 2);
}
int main() {
    try {
        float reference = 0;
        for (int hz : {60, 120, 144, 240, 360}) {
            lab::OrbitInput input;
            float yaw = 0, pitch = .5f;
            input.add(80, 0);
            for (int i = 0; i < hz; ++i)
                input.step(1.f / hz, yaw, pitch, true);
            require(std::abs(yaw + .4f) < .00001f, "Frame rate changed total orbit displacement");
            reference = yaw;
            input.step(.1f, yaw, pitch, true);
            require(yaw == reference, "Settled camera drifts");
            input.add(400, 400);
            input.clear();
            input.step(.1f, yaw, pitch, true);
            require(yaw == reference && pitch == .5f, "Focus/pause reset left queued movement");
            input.add(0, 1000);
            input.step(.02f, yaw, pitch, true);
            require(pitch == 1.4f, "Elevation clamp failed");
            input.add(0, -1);
            input.step(.01f, yaw, pitch, true);
            require(pitch < 1.4f, "Clamped input backlog prevented reversal");
        }
        lab::OrbitInput whole, partitioned;
        float a = 0, b = 0, pa = .5f, pb = .5f;
        whole.add(100, 0);
        partitioned.add(100, 0);
        whole.step(.012f, a, pa, true);
        for (float dt : {.001f, .005f, .002f, .004f})
            partitioned.step(dt, b, pb, true);
        require(std::abs(a - b) < .00001f, "Smoothing depends on frame partition");

        lab::OrbitInput input;
        float yaw = 0, pitch = .5f, previous = 0;
        int delivered = 0;
        std::vector<float> raw, smoothed;
        for (int frame = 1; frame <= 240; ++frame) {
            int packets = int((double(frame) / 240 + 1e-9) / .008);
            float dx = float((packets - delivered) * 8);
            delivered = packets;
            input.add(dx, 0);
            input.step(1.f / 240, yaw, pitch, true);
            if (frame > 24) {
                raw.push_back(dx * -.005f);
                smoothed.push_back(yaw - previous);
            }
            previous = yaw;
        }
        double ratio = variance(smoothed) / variance(raw);
        require(ratio < .4, "125 Hz packets still step the 240 Hz orbit camera");
        std::cout << "PASS: displacement, frame independence, reset/clamp, packet variance ratio " << ratio
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
