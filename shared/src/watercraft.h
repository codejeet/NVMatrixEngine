#pragma once
#include <DirectXMath.h>
#include <array>
#include <algorithm>
namespace watercraft {
// Shared collision/render dimensions, metres. Twin pontoons have 0.828 m^3
// displacement; 220 kg hull + 60 kg pilot draws about 12 cm in fresh water.
struct Box {
    DirectX::XMFLOAT3 center, half;
};
inline constexpr std::array<Box, 3> hull{{{{-.72f, 0, 0}, {.23f, .18f, 1.25f}},
                                          {{.72f, 0, 0}, {.23f, .18f, 1.25f}},
                                          {{0, .24f, 0}, {.88f, .06f, .8f}}}};
inline constexpr float mass = 220, riderMass = 60, ballRadius = .68f, glassDensity = 2500;
inline float immersedFraction(float height, float center, float halfHeight) {
    return std::clamp((height - center + halfHeight) / (2 * halfHeight), 0.f, 1.f);
}
inline float sphereVolume(float radius, float height) {
    const float h = std::clamp(height, 0.f, 2 * radius);
    return 3.14159265359f * h * h * (radius - h / 3);
}
inline float ballMass(bool floating) {
    // Float keeps the lightweight hollow avatar; Sink fills the same volume
    // with glass. Optical appearance and displaced volume do not change.
    return floating ? riderMass : glassDensity * sphereVolume(ballRadius, 2 * ballRadius);
}
} // namespace watercraft
