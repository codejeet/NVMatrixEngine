#pragma once
#include <DirectXMath.h>

namespace lab::deepPool {
// Separate scale-test preset. Metres, not a transform of the simulation output.
// Keep room and collision boundaries identical; the free surface starts at 8.02 m.
inline constexpr DirectX::XMFLOAT3 minimum{-23.95f, .02f, -23.95f};
inline constexpr DirectX::XMFLOAT3 maximum{23.95f, 12.18f, 31.95f};
inline constexpr DirectX::XMFLOAT3 inlet{-23.35f, 9.5f, 9.6f};
inline constexpr DirectX::XMFLOAT3 spawn{-20.f, 8.74f, 9.6f};
inline constexpr float depth = 8.f, cellSize = .64f;
inline constexpr unsigned particles = 900000, capacity = 1000000;
} // namespace lab::deepPool
