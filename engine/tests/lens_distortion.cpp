#include "../src/experience.h"
#include <iostream>
#include <stdexcept>

void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        double worstPixelError = 0;
        for (float aspect : {1.f, 16.f / 9, 32.f / 9, 9.f / 16})
            for (float fov : {90.f, 120.f, 160.f}) {
                const float halfAngle = fov * .00872664626f;
                const double h = double(halfAngle);
                lab::Lens lens{true, fov};
                const auto toRect = lab::lensMapping::fisheyeToRectilinearScale;
                const auto toFish = lab::lensMapping::rectilinearToFisheyeScale;
                require(std::isfinite(toRect(0, halfAngle)) && std::isfinite(toFish(0, halfAngle)),
                        "Lens centre produced NaN/Inf");
                require(std::abs(toRect(0, halfAngle) * toFish(0, halfAngle) - 1) < 1e-6,
                        "Centre derivatives are not reciprocal");
                for (int y = -50; y <= 50; ++y)
                    for (int x = -50; x <= 50; ++x) {
                        const float px = x / 50.f, py = y / 50.f;
                        const float radius = std::hypot(px * aspect, py) / std::sqrt(1 + aspect * aspect);
                        const float forward = toRect(radius, halfAngle);
                        const float inverse = toFish(radius * forward, halfAngle);
                        require(std::isfinite(forward) && std::isfinite(inverse), "Nonfinite lens map");
                        require(std::abs(forward * inverse - 1) < 3e-6, "Forward/reverse lens roundtrip");
                        const double theta = 2 * std::asin(double(radius) * std::sin(h * .5));
                        const double reference = radius ? std::tan(theta) / (std::tan(h) * radius)
                                                        : 2 * std::sin(h * .5) / std::tan(h);
                        const double error = std::abs(reference - forward) * 3840 * .5;
                        worstPixelError = std::max(worstPixelError, error);
                        float pickX = px, pickY = py;
                        lens.rectilinear(pickX, pickY, aspect);
                        require(std::abs(pickX - px * forward) < 3e-6 &&
                                    std::abs(pickY - py * forward) < 3e-6,
                                "Picking and presentation lens mappings disagree");
                        const float fishScale = toFish(radius, halfAngle);
                        const float roundtrip = toRect(radius * fishScale, halfAngle);
                        require(std::abs(fishScale * roundtrip - 1) < 4e-6,
                                "Inverse direction clipped offscreen correspondences");
                    }
                require(std::abs(toRect(1, halfAngle) - 1) < 2e-6,
                        "Diagonal FOV must remain fixed at the image corners");
            }
        require(worstPixelError < .01, "Shared float lens mapping exceeds .01 pixels at 4K");
        std::cout << "PASS lens forward/inverse, centre/corners, picking, portrait/ultrawide; max 4K error "
                  << worstPixelError << " pixels\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
