#pragma once
#include <DirectXMath.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>

namespace lab {
// Rigid transforms only. SDF samples and these structured records share this ABI.
// Moving solids are one-way coupled; future force feedback belongs on the GPU.
enum class FluidColliderType : uint32_t { Sphere, Box, Capsule, Cylinder, Plane, Mesh };
struct FluidCollider {
    DirectX::XMFLOAT4X4 worldToLocal;
    DirectX::XMFLOAT4 centerRestitution;
    DirectX::XMFLOAT4 extentType;
    DirectX::XMFLOAT4 velocityFriction;
    DirectX::XMFLOAT4 angularSlip;
    DirectX::XMFLOAT4 meshMinimumSpacing;
    DirectX::XMUINT4 meshDimensions;
};
static_assert(sizeof(FluidCollider) == 160);

// One immutable upload slice per simulation endpoint, plus slice zero for the
// current render surface. Only the bounded collider array is CPU-side; no liquid
// state or GPU work list is read back. Fixed slots identify colliders until reset.
class FluidColliderTimeline {
  public:
    static constexpr uint32_t capacity = 16, maxSteps = 16;
    static constexpr uint64_t sliceBytes = capacity * sizeof(FluidCollider);
    std::array<FluidCollider, capacity *(maxSteps + 1)> slices{};
    // Geometry at the last simulation endpoint, distinct from slice zero's
    // current render pose on frames that have not advanced the simulation.
    std::array<FluidCollider, capacity> simulationStart{};
    uint32_t count = 0;
    bool moving = false;

    bool set(std::span<const FluidCollider> colliders, uint32_t discontinuities = 0) {
        if (colliders.size() > capacity)
            throw std::runtime_error("Fluid supports at most 16 SDF colliders");
        if (discontinuities >> colliders.size())
            throw std::runtime_error("Fluid collider discontinuity refers to a missing slot");
        for (const auto &v : colliders)
            for (uint32_t i = 0; i < (sizeof(v) - sizeof(v.meshDimensions)) / sizeof(float); ++i)
                if (!std::isfinite(reinterpret_cast<const float *>(&v)[i]))
                    throw std::runtime_error("Nonfinite fluid collider");
        bool changed =
            pendingDiscontinuities || discontinuities || count != colliders.size() ||
            (!colliders.empty() && memcmp(target.data(), colliders.data(), colliders.size_bytes()));
        count = uint32_t(colliders.size());
        pendingDiscontinuities = (pendingDiscontinuities | discontinuities) & ((1u << count) - 1);
        if (count)
            memcpy(target.data(), colliders.data(), colliders.size_bytes());
        return changed;
    }

    void prepare(uint32_t steps, float rate, bool reset, bool paused, bool conserveBoundaryVolume = false) {
        using namespace DirectX;
        if (steps > maxSteps || !std::isfinite(rate) || rate <= 0)
            throw std::runtime_error("Invalid fluid collider timestep");
        // Resident-volume transport cannot silently insert/move a solid without
        // transporting the displaced capacity. Require a deliberate fluid reset.
        if (pendingDiscontinuities && conserveBoundaryVolume && !reset)
            throw std::runtime_error("Collider teleport requires a reset with resident-volume transport");
        moving = false;
        for (uint32_t k = 0; k < count; ++k) {
            const auto &end = target[k];
            auto start = previous[k];
            // New geometry/paused placement is a discontinuity, not a swept
            // impulse over zero time. Ordinary zero-substep frames retain the
            // last *simulation* pose, including skipped render-frame motion.
            const bool discontinuous = (pendingDiscontinuities & (1u << k)) != 0;
            if (reset || discontinuous || k >= previousCount ||
                (paused && !steps && !conserveBoundaryVolume) ||
                memcmp(&start.extentType, &end.extentType, sizeof(end.extentType)) ||
                memcmp(&start.meshMinimumSpacing, &end.meshMinimumSpacing, sizeof(end.meshMinimumSpacing)) ||
                memcmp(&start.meshDimensions, &end.meshDimensions, sizeof(end.meshDimensions)))
                start = end;
            // A resident-volume solver cannot accept new geometry without a
            // transport step. Keep its collision/reconstruction boundary at
            // the last simulation pose while paused or waiting for a substep;
            // the next step processes the pending motion with swept apertures.
            FluidCollider boundary = conserveBoundaryVolume && !steps ? start : end;
            boundary.velocityFriction.x = boundary.velocityFriction.y = boundary.velocityFriction.z = 0;
            boundary.angularSlip.x = boundary.angularSlip.y = boundary.angularSlip.z = 0;
            const bool poseChanged =
                memcmp(&start.worldToLocal, &end.worldToLocal, sizeof(end.worldToLocal)) != 0;
            XMVECTOR q0 = XMQuaternionIdentity(), q1 = q0;
            XMVECTOR p0 = XMLoadFloat4(&start.centerRestitution), p1 = XMLoadFloat4(&end.centerRestitution);
            if (steps && poseChanged) {
                moving = true;
                auto m0 = XMMatrixInverse(nullptr, XMLoadFloat4x4(&start.worldToLocal));
                auto m1 = XMMatrixInverse(nullptr, XMLoadFloat4x4(&end.worldToLocal));
                q0 = XMQuaternionNormalize(XMQuaternionRotationMatrix(m0));
                q1 = XMQuaternionNormalize(XMQuaternionRotationMatrix(m1));
                const float inverseTime = rate / steps;
                XMFLOAT3 velocity;
                XMStoreFloat3(&velocity, XMVectorScale(XMVectorSubtract(p1, p0), inverseTime));
                boundary.velocityFriction = {velocity.x, velocity.y, velocity.z, end.velocityFriction.w};
                // DirectX multiply(a,b) represents b*a. This is world-space
                // angular velocity, compatible with omega x (P - center).
                auto delta = XMQuaternionMultiply(XMQuaternionInverse(q0), q1);
                if (XMVectorGetW(delta) < 0)
                    delta = XMVectorNegate(delta);
                const float sine = XMVectorGetX(XMVector3Length(delta));
                // atan2 retains small-angle precision; acos(w) quantizes slow
                // rotating wakes when w rounds to one at high simulation rates.
                if (sine > 1e-8f) {
                    const float angle = 2 * std::atan2(sine, XMVectorGetW(delta));
                    XMStoreFloat3(&velocity, XMVectorScale(delta, angle * inverseTime / sine));
                    boundary.angularSlip = {velocity.x, velocity.y, velocity.z, end.angularSlip.w};
                }
            }
            slices[k] = boundary;
            simulationStart[k] = start;
            for (uint32_t s = 1; s <= steps; ++s) {
                auto value = boundary;
                if (poseChanged && s < steps) {
                    const float fraction = float(s) / steps;
                    const auto position = XMVectorLerp(p0, p1, fraction);
                    const auto rotation = XMQuaternionSlerp(q0, q1, fraction);
                    auto world = XMMatrixRotationQuaternion(rotation);
                    world.r[3] = XMVectorSetW(position, 1);
                    XMStoreFloat4x4(&value.worldToLocal, XMMatrixInverse(nullptr, world));
                    XMStoreFloat4(&value.centerRestitution, XMVectorSetW(position, end.centerRestitution.w));
                }
                // Last endpoint copies the exact supplied SDF transform: no
                // decomposition roundoff between simulation and DXR surface.
                slices[s * capacity + k] = value;
            }
            if (steps || reset || discontinuous || (paused && !conserveBoundaryVolume) || k >= previousCount)
                previous[k] = end;
        }
        previousCount = count;
        pendingDiscontinuities = 0;
    }

  private:
    std::array<FluidCollider, capacity> target{}, previous{};
    uint32_t previousCount = 0;
    uint32_t pendingDiscontinuities = 0;
};
} // namespace lab
