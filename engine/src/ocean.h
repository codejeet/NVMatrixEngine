#pragma once
#include <DirectXMath.h>
#include <array>
#include "../shaders/ocean-terrain.hlsli"
#include "../shaders/ocean-lights.hlsli"
namespace lab::ocean {
inline constexpr float depth = 6.f, surface = depth + .02f, cellSize = 1.f;
inline constexpr DirectX::XMFLOAT3 minimum{-128.f, .02f, -128.f};
inline constexpr DirectX::XMFLOAT3 maximum{128.f, 13.02f, 128.f};
inline constexpr DirectX::XMFLOAT3 spawn{3.f, 7.3f, -8.f};
inline constexpr DirectX::XMFLOAT3 inlet{7.6f, 7.75f, -8.f};
inline constexpr unsigned particles = 800000, capacity = 1000000;
inline constexpr float terrainMinX = -72, terrainMinZ = -64, terrainStep = .5f;
inline constexpr unsigned terrainX = 201, terrainZ = 185;
struct Fixture { DirectX::XMFLOAT3 center, half; };
inline constexpr std::array<Fixture, 7> pier{{
    {{0,6.4f,-8},{7,.14f,1}}, {{5,3.2f,-8},{.18f,3.2f,.18f}},
    {{6.75f,7.15f,-8},{.15f,.85f,.15f}},
    {{7.3f,7.75f,-8.24f},{.3f,.28f,.04f}}, {{7.3f,7.75f,-7.76f},{.3f,.28f,.04f}},
    {{7.3f,7.99f,-8},{.3f,.04f,.24f}}, {{7.3f,7.51f,-8},{.3f,.04f,.24f}}
}};
}
