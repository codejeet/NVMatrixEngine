#pragma once
#include <DirectXMath.h>

namespace lab::largeWater {
// The ordinary Water Lab, with an expanded basin and a deeper starting fill.
// Keep props at their original scale and height relative to the free surface.
inline constexpr float depth = 1.5f, lift = depth - .34f, cellSize = .24f;
inline constexpr DirectX::XMFLOAT3 minimum{-11.95f, .02f, -11.95f};
inline constexpr DirectX::XMFLOAT3 maximum{11.95f, 3.06f + lift, 15.95f};
inline constexpr DirectX::XMFLOAT3 inlet{-11.6f, 1.85f + lift, 2.4f};
inline constexpr unsigned particles = 600000, capacity = 900000;
} // namespace lab::largeWater
