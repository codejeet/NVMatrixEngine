#pragma once
#include <DirectXMath.h>
struct Camera {
    DirectX::XMFLOAT3 position, right, up, forward;
    DirectX::XMFLOAT4X4 view, projection, viewProjection;
};
