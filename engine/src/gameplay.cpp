#include "gameplay.h"
#include "scene.h"
#include "deep_pool.h"
#include <cmath>
#include <stdexcept>
namespace lab {
Level makePlayLevel(bool fluidRoom, bool boat, bool deep) {
    Level l{};
    l.name = "Spectral playground";
    l.objective = "Feed green light into the marked receiver to open the cyan portal.";
    l.hint = "Roll to the prism. F locks it in the cradle. Drag to shift the spectrum; scroll to rotate. "
             "Hold the receiver for two seconds.";
    l.ballStart = {0, .72f, 3.2f};
    l.source = {0, 2, -5};
    l.direction = {0, -.07f, 1};
    l.receiver = {receiverTarget.x, receiverTarget.y, receiverTarget.z};
    l.receiverHalf = {.01f, .65f, receiverTarget.w};
    l.sensors.push_back(
        {{6, 1.5f, 5.1f, 0}, {.01f, .65f, .65f, 0}, {510, 570, receiverThreshold, 40}, {0, 1, 1, 0}});
    l.solids = {{{0, -.25f, 1}, {6, .25f, 7}},
                {{-6.15f, 3, 1}, {.15f, 3, 7}},
                {{6.15f, 3, 1}, {.15f, 3, 7}},
                {{0, 3, 8.15f}, {6, 3, .15f}},
                {{-3.8f, 1.4f, -6.2f}, {2.2f, 1.4f, .16f}},
                {{3.8f, 1.4f, -6.2f}, {2.2f, 1.4f, .16f}},
                {{0, -.2f, -6.65f}, {1.6f, .2f, .8f}},
                {{0, .29f, 0}, {1.05f, .29f, 1.05f}},
                {{0, 1.2f, -5.25f}, {.30f, 1.2f, .25f}}};
    l.props = {{{0, 1.60f, 0},
                {.85f, 1, .9814954f},
                XM_PI / 6 + .26f,
                5,
                1,
                0,
                {0, 1.60f, 0, XM_PI / 6},
                {1, 0, 0, 0}},
               {{-3.2f, .40f, 1.4f}, {.38f, .38f, .38f}, 0, 3, 0, 0, {}, {}},
               {{3.2f, .40f, 1.4f}, {.38f, .38f, .38f}, 0, 4, 0, 0, {}, {}},
               {{0, 1.4f, -6.2f}, {1.48f, 1.4f, .16f}, 0, 0, 1, 1, {}, {}}};
    // Bullet uses the exact same prism vertices as DXR, not its bounding box.
    // Optical water is renderer-owned, not a rigid glass block. Tank walls are solid.
    if (!fluidRoom) {
        for (float x : {1.70f, 5.70f})
            l.solids.push_back({{x, .6f, -3.10f}, {.10f, .6f, 1.90f}});
        for (float z : {-5.f, -1.20f})
            l.solids.push_back({{3.70f, .6f, z}, {1.90f, .6f, .10f}});
    } else {
        l.name = "Flood chamber";
        l.hint = "Press T or use the wall-valve button to pour water. E operates the valve when nearby. "
                 "B drains and resets the room. Throw objects into the inlet to make splashes.";
    }
    if (fluidRoom && boat)
        l.props.push_back({{2.5f, .52f, 4.7f}, {.95f, .3f, 1.25f}, 0, 8, 0, 0, {}, {}});
    if (deep) {
        l.name = "Deep-water scale lab · 48 x 56 x 8 metres";
        l.objective = "Explore the deep pool; T opens the localized wall inlet. I switches inspection view.";
        l.ballStart = deepPool::spawn;
        l.source = {0, 10, -20};
        l.solids = {{{0, -.25f, 4}, {24, .25f, 28}},         {{-24.15f, 7, 4}, {.15f, 7, 28}},
                    {{24.15f, 7, 4}, {.15f, 7, 28}},         {{0, 7, 32.15f}, {24, 7, .15f}},
                    {{0, 7, -24.15f}, {24, 7, .15f}},        {{0, 4.29f, 0}, {1.05f, 4.29f, 1.05f}},
                    {{0, 5.2f, -20.25f}, {.30f, 5.2f, .25f}}};
        l.props[0].position = {0, 9.6f, 0};
        l.props[0].dockYaw = {0, 9.6f, 0, XM_PI / 6};
        l.props[1].position = {-18, 8.4f, 9.6f};
        l.props[2].position = {-18, 8.4f, 12.6f};
        l.props[3].position = {0, 9.4f, -24.2f};
        if (boat)
            l.props.back().position = {-17.5f, 8.2f, 9.6f};
    }
    auto meshes = makePlayScene(false, false, false, fluidRoom, boat, deep);
    l.meshes.resize(meshes.size());
    for (size_t i = 0; i < meshes.size(); ++i)
        for (auto &v : meshes[i].vertices)
            l.meshes[i].push_back({v.position, {}, v.material, 0});
    return l;
}
void runPlayTests() {
    auto require = [](bool ok, const char *why) {
        if (!ok)
            throw std::runtime_error(why);
    };
    const auto room = makePlayLevel(true);
    const auto roomMeshes = makePlayScene(false, false, false, true);
    require(room.solids.size() == 9 && room.meshes.size() == 6 && roomMeshes.size() == 6,
            "Room pool retained legacy tank collision/render walls");
    bool valve = false;
    for (const auto &mesh : roomMeshes)
        for (const auto &v : mesh.vertices) {
            require(v.chart != 4 && v.material != 10,
                    "Room contains the old elevated tank floor/glass shell");
            valve |= v.material == 14;
        }
    require(valve, "Room has no wall valve geometry");
    const auto deep = makePlayLevel(true, true, true);
    const auto deepMeshes = makePlayScene(false, false, false, true, true, true);
    require(deep.solids.size() == 7 && deep.meshes.size() == 7 && deepMeshes.size() == 7,
            "Deep pool body/renderer layout differs");
    require(deep.ballStart.y > deepPool::depth && deep.props.back().position.y > deepPool::depth,
            "Deep pool avatar/boat spawned on the bottom");
    std::array<double, 5> areas{};
    for (size_t i = 0; i < deepMeshes[0].vertices.size(); i += 3) {
        const auto &v = deepMeshes[0].vertices;
        if (v[i].chart >= 5)
            continue;
        auto a = XMLoadFloat3(&v[i].position), b = XMLoadFloat3(&v[i + 1].position),
             c = XMLoadFloat3(&v[i + 2].position);
        areas[v[i].chart] +=
            .5 *
            XMVectorGetX(XMVector3Length(XMVector3Cross(XMVectorSubtract(b, a), XMVectorSubtract(c, a))));
    }
    require(areas == std::array<double, 5>{2688, 672, 784, 784, 672},
            "Deep pool photon chart areas differ from physical geometry");
    XMFLOAT4X4 anchor, pose;
    XMStoreFloat4x4(&anchor, XMMatrixIdentity());
    pose = anchor;
    require(!transportMoved(pose, anchor, 1), "Static transport invalidated");
    pose._41 = .00002f;
    require(!transportMoved(pose, anchor, 1), "Sub-tolerance physics jitter invalidated transport");
    // Slow movement is tested against the unchanged history anchor, not last frame.
    for (int i = 0; i < 10; ++i)
        pose._41 += .00002f;
    require(transportMoved(pose, anchor, 1), "Slow translation never invalidated transport");
    XMStoreFloat4x4(&pose, XMMatrixRotationY(.001f));
    require(transportMoved(pose, anchor, 1), "Prism rotation failed to invalidate transport");
    require(!transportMoved(pose, pose, 1), "New anchor did not settle");
    Game g({makePlayLevel()});
    for (int i = 0; i < 360; ++i)
        g.step(1.f / 120);
    require(g.grounded(), "Lab avatar did not settle on the floor");
    auto start = g.playerPosition();
    g.input.right = true;
    for (int i = 0; i < 70; ++i)
        g.step(1.f / 120);
    require(std::hypot(g.playerPosition().x - start.x, g.playerPosition().z - start.z) > 1,
            "Lab movement failed");
    g.clearInput();
    g.input.jump = true;
    g.step(1.f / 120);
    require(g.jumps == 1, "Lab jump failed");
    g.input.jump = true;
    g.step(1.f / 120);
    require(g.jumps == 1, "Air jump was allowed");
    g.load(0);
    g.azimuth = 0;
    require(g.beginTuning(), "Prism must be reachable from spawn");
    for (int i = 0; i < 90; ++i)
        g.step(1.f / 120);
    require(g.tuningReady(), "Top-down camera transition failed");
    auto p = g.opticPosition();
    float yaw = g.opticYaw();
    require(g.tuneMove(.15f, .1f) && g.tuneRotate(.08f), "Fine tuning failed");
    require(!g.tuneMove(10, 0), "Cradle travel limit ignored");
    g.endTuning(true);
    require(std::abs(g.opticPosition().x - p.x) < .001f && std::abs(g.opticYaw() - yaw) < .001f,
            "Cancel must restore pose");
    g.grab();
    require(g.held == 1, "Grab must release the locked prism");
    g.throwHeld();
    require(g.held < 0 && g.throws == 1, "Throw failed");
    g.load(0);
    g.place(0, {-3.2f, .7f, 3.4f});
    g.azimuth = 0;
    g.grab();
    require(g.held == 2, "Luminous cube was not grabbable");
    g.throwHeld();
    require(g.held < 0, "Luminous cube was not thrown");
    // Exercise existing charge/gate/win logic; GPU sensor tests separately verify real transport.
    LaserResult signal{};
    g.load(0);
    for (int i = 0; i < 180; ++i) {
        g.receive(signal, 1.f / 60);
        g.step(1.f / 60);
    }
    require(!g.gateOpen, "Dark receiver unlocked the gate");
    signal.stats.y = .35f;
    signal.sensors[0].x = receiverThreshold;
    for (int i = 0; i < 240; ++i) {
        g.receive(signal, 1.f / 60);
        g.step(1.f / 60);
    }
    require(g.gateOpen && g.gateLift > 2.2f, "Charged receiver did not open the gate");
    g.place(0, {0, .7f, -6.6f});
    g.receive(signal, 1.f / 60);
    require(g.won, "Exit did not complete the test chamber");
}
} // namespace lab
