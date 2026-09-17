#include "../src/fluid/fluid_colliders.h"
#include <cstdlib>
#include <iostream>
using namespace DirectX;
using namespace lab;
static void require(bool ok, const char *message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
static FluidCollider collider(float x, XMVECTOR rotation = XMQuaternionIdentity()) {
    FluidCollider c{};
    auto world = XMMatrixRotationQuaternion(rotation);
    world.r[3] = XMVectorSet(x, 2, 3, 1);
    XMStoreFloat4x4(&c.worldToLocal, XMMatrixInverse(nullptr, world));
    c.centerRestitution = {x, 2, 3, .2f};
    c.extentType = {1, 1, 1, 1};
    c.velocityFriction = {999, 999, 999, .3f}; // render velocities must not leak
    c.angularSlip = {999, 999, 999, .7f};
    return c;
}
int main() {
    {
        FluidColliderTimeline teleports;
        std::array<FluidCollider, 2> bodies{collider(0), collider(1)};
        teleports.set(bodies);
        teleports.prepare(2, 120, true, false);
        bodies = {collider(20), collider(1.1f)};
        teleports.set(bodies, 1);
        teleports.prepare(2, 120, false, false);
        require(teleports.slices[16].centerRestitution.x == 20 &&
                    teleports.slices[16].velocityFriction.x == 0,
                "Explicit teleport injected a swept impulse");
        require(std::abs(teleports.slices[17].velocityFriction.x - 6) < 1e-4,
                "One teleport erased another collider's ordinary motion");
        bodies[0] = collider(30);
        teleports.set(bodies, 1);
        teleports.prepare(0, 120, false, false);
        bodies[0] = collider(30.01f);
        teleports.set(bodies);
        teleports.prepare(1, 120, false, false);
        require(std::abs(teleports.slices[16].velocityFriction.x - 1.2f) < .0001,
                "Zero-step teleport did not establish a new simulation anchor");
        teleports.set(bodies, 1);
        bool rejected = false;
        try {
            teleports.prepare(1, 120, false, false, true);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        require(rejected, "Resident capacity silently accepted a teleport");
        teleports.prepare(1, 120, true, false, true);
        require(teleports.slices[16].velocityFriction.x == 0, "Reset did not admit explicit placement");
    }
    FluidColliderTimeline t;
    std::array<FluidCollider, 1> c{collider(0)};
    require(t.set(c), "First collider submission must be dirty");
    t.prepare(2, 120, true, false);
    require(!t.moving && t.slices[0].velocityFriction.x == 0, "Reset must not inject render velocity");
    require(!t.set(c), "Unchanged colliders must not be dirty");
    c[0] = collider(.1f);
    t.set(c);
    t.prepare(0, 120, false, false);
    require(t.simulationStart[0].centerRestitution.x == 0 && t.slices[0].centerRestitution.x == .1f,
            "Zero-step geometry must retain a distinct simulation anchor");
    c[0] = collider(.2f);
    t.set(c);
    t.prepare(2, 120, false, false);
    require(t.simulationStart[0].centerRestitution.x == 0,
            "Swept capacity must include all skipped render motion");
    require(t.moving && std::abs(t.slices[16].centerRestitution.x - .1f) < 1e-6f,
            "First substep must include skipped render-frame motion");
    require(std::abs(t.slices[0].velocityFriction.x - 12) < 1e-5f,
            "Boundary velocity must use simulated time");
    require(memcmp(&t.slices[32].worldToLocal, &c[0].worldToLocal, 64) == 0,
            "Final SDF must exactly match renderer endpoint");
    require(t.slices[16].velocityFriction.w == .3f && t.slices[16].angularSlip.w == .7f &&
                t.slices[16].centerRestitution.w == .2f,
            "Interpolation must preserve boundary material");
    c[0] = collider(10);
    t.set(c);
    t.prepare(0, 120, false, true);
    require(t.simulationStart[0].centerRestitution.x == 10,
            "Paused placement must establish a fresh capacity anchor");
    t.prepare(1, 120, false, true);
    require(!t.moving && t.slices[16].velocityFriction.x == 0,
            "Paused placement/single step must not create catch-up impulse");
    // Coupled resident liquid must not lose capacity while transport is paused.
    c[0] = collider(0);
    t.set(c);
    t.prepare(2, 120, true, false, true);
    for (float x : {.01f, .02f, .03f}) {
        c[0] = collider(x);
        t.set(c);
        t.prepare(0, 120, false, true, true);
        require(t.simulationStart[0].centerRestitution.x == 0 && t.slices[0].centerRestitution.x == 0 &&
                    t.slices[0].velocityFriction.x == 0 && !t.moving,
                "Paused resident-volume geometry must retain its last transported capacity");
    }
    t.prepare(1, 120, false, true, true);
    require(t.moving && t.simulationStart[0].centerRestitution.x == 0 &&
                t.slices[16].centerRestitution.x == .03f &&
                std::abs(t.slices[16].velocityFriction.x - 3.6f) < 1e-6f,
            "Single step must sweep all pending paused boundary motion exactly once");
    t.prepare(0, 120, false, true, true);
    require(t.simulationStart[0].centerRestitution.x == .03f && t.slices[0].centerRestitution.x == .03f &&
                !t.moving,
            "Completed single step must commit the new capacity anchor");
    c[0] = collider(.04f);
    t.set(c);
    t.prepare(0, 120, false, false, true);
    require(t.slices[0].centerRestitution.x == .03f,
            "Non-paused zero-substep frames must also preserve transported geometry");
    t.prepare(0, 120, true, true, true);
    require(t.simulationStart[0].centerRestitution.x == .04f && t.slices[0].centerRestitution.x == .04f,
            "An explicit fluid reset may establish a new capacity anchor");
    // Noncommuting rotations test world-space omega, not merely single-axis
    // identity rotations that would conceal a quaternion multiplication error.
    const auto q0 = XMQuaternionRotationRollPitchYaw(.4f, -.6f, .2f);
    const auto dq = XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), .1f);
    c[0] = collider(0, q0);
    t.set(c);
    t.prepare(2, 120, true, false);
    c[0] = collider(0, XMQuaternionMultiply(q0, dq));
    t.set(c);
    t.prepare(2, 120, false, false);
    auto omega = t.slices[16].angularSlip;
    require(std::abs(omega.x) < 1e-4 && std::abs(omega.y - 6) < 1e-4 && std::abs(omega.z) < 1e-4,
            "Angular velocity must be world-space shortest-arc derivative");
    auto midpoint = XMMatrixInverse(nullptr, XMLoadFloat4x4(&t.slices[16].worldToLocal));
    auto expected = XMMatrixRotationQuaternion(
        XMQuaternionMultiply(q0, XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0), .05f)));
    for (uint32_t r = 0; r < 3; ++r)
        require(XMVectorGetX(XMVector3Length(midpoint.r[r] - expected.r[r])) < 1e-5f,
                "Rigid substep rotation must follow shortest arc");
    c[0] = collider(30);
    c[0].extentType.x = 2;
    t.set(c);
    t.prepare(2, 120, false, false);
    require(!t.moving, "Geometry replacement must establish a fresh simulation anchor");
    t.set({});
    t.prepare(0, 120, false, false);
    t.set(c);
    t.prepare(16, 120, false, false);
    require(!t.moving && t.slices[256].centerRestitution.x == 30,
            "Reintroduced colliders and maximum slice must be valid");
    c[0] = collider(0);
    t.set(c);
    t.prepare(2, 120, true, false);
    c[0] = collider(0, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), .00001f));
    t.set(c);
    t.prepare(2, 120, false, false);
    require(std::abs(t.slices[16].angularSlip.z - .0006f) < 1e-7f,
            "Very slow rotation must not quantize to zero");
    c[0] = collider(0, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), XM_PI - .01f));
    t.set(c);
    t.prepare(2, 120, true, false);
    c[0] = collider(0, XMQuaternionRotationAxis(XMVectorSet(0, 0, 1, 0), -XM_PI + .01f));
    t.set(c);
    t.prepare(2, 120, false, false);
    require(std::abs(t.slices[16].angularSlip.z - 1.2f) < .0001f,
            "Quaternion hemisphere crossing must not take a full revolution");
    std::cout << "PASS fluid collider timeline: immutable substeps, skipped frames, pause/reset, world-space "
                 "rotation\n";
}
