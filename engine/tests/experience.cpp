#include "../src/experience.h"
#include "../src/gameplay.h"
#include "watercraft.h"
#include <iostream>
#include <stdexcept>
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    try {
        require(lab::Lens{}.diagonalDegrees == 120, "Default fisheye FOV regressed");
        require(!lab::ExperienceSettings{}.ballFloats, "Water Lab must default to solid sinking glass");
        for (float aspect : {1.f, 16.f / 9, 32.f / 9})
            for (float fov : {90.f, 140.f, 160.f}) {
                lab::Lens lens{true, fov};
                for (float x : {-.99f, -.5f, 0.f, .5f, .99f})
                    for (float y : {-.99f, 0.f, .99f}) {
                        float u = x, v = y;
                        lens.rectilinear(u, v, aspect);
                        float radius = std::hypot(x * aspect, y) / std::sqrt(1 + aspect * aspect);
                        float expected = 2 * std::asin(radius * std::sin(fov * XM_PI / 720));
                        float actual = std::atan(std::hypot(u * aspect, v) * lens.tanHalfVertical(aspect));
                        require(std::abs(expected - actual) < 1e-5, "Fisheye ray/sensor projection mismatch");
                    }
            }
        require(watercraft::immersedFraction(.5f, .5f, .2f) == .5f, "Half-submerged box volume");
        require(std::abs(watercraft::sphereVolume(1, 1) * 2 - watercraft::sphereVolume(1, 2)) < 1e-5,
                "Sphere cap volume");
        {
            Game ball({lab::makePlayLevel(true, true)});
            auto advance = [&](int count) {
                for (int i = 0; i < count; ++i) {
                    auto water = ball.waterQueries();
                    for (auto &p : water)
                        p = {2.5f, 0, 0, 0};
                    ball.receiveWater(water, 998.207f, 9.81f);
                    ball.step(1.f / 120);
                }
            };
            require(!ball.ballFloats() && std::abs(ball.ballDensity() - 2500) < .01f,
                    "Physics construction must default to solid sinking glass without UI synchronization");
            ball.place(0, {-1.8f, 1.4f, 3.2f});
            advance(600);
            require(std::abs(ball.playerPosition().y - .68f) < .03f,
                    "Default ball must sink without a density setter call");
            // Explicitly select Float for the existing moving Float -> Sink test.
            ball.setBallFloating(true);
            require(ball.ballFloats() && std::abs(ball.ballMass() - 60) < .001f,
                    "Explicit Float must retain hollow floating mass");
            ball.place(0, {-1.8f, 1.4f, 3.2f});
            require(!ball.poseDiscontinuities.empty() && ball.poseDiscontinuities[0],
                    "Explicit placement must notify the fluid boundary consumer");
            ball.poseDiscontinuities.clear();
            ball.input.forward = true;
            advance(10);
            require(ball.poseDiscontinuities.empty(), "Ordinary physics was marked as a teleport");
            ball.paused = true;
            const auto before = ball.poses()[1];
            const float speed = ball.speed();
            require(speed > .01f, "Density switch fixture is not moving");
            ball.resetHistory = false;
            ball.setBallFloating(false);
            ball.setBallFloating(false); // Per-frame synchronization must be idempotent.
            const auto after = ball.poses()[1];
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    require(before.m[row][col] == after.m[row][col], "Density switch changed ball pose");
            require(ball.speed() == speed && !ball.resetHistory, "Density switch reset motion/history");
            require(!ball.ballFloats() && std::abs(ball.ballDensity() - 2500) < .01f,
                    "Sink must use physical solid-glass density");
            ball.paused = false;
            ball.clearInput();
            advance(600);
            require(std::abs(ball.playerPosition().y - .68f) < .03f, "Solid glass failed to sink to floor");
            ball.setBallFloating(true);
            advance(1200);
            require(ball.playerPosition().y > 2.5f && ball.playerPosition().y < 3.18f,
                    "Hollow glass failed to rise and float at free surface");
            auto boat = ball.poses()[6];
            ball.place(0, {boat._41, boat._42 + .5f, boat._43 - 2});
            require(ball.toggleBoat(), "Density fixture failed to board");
            advance(600);
            const float lightDraft = ball.poses()[6]._42;
            ball.setBallFloating(false);
            advance(30);
            require(ball.poses()[6]._42 < lightDraft - .1f, "Boat ignored heavy ball passenger load");
            ball.load(0);
            require(!ball.ballFloats() && std::abs(ball.ballDensity() - 2500) < .01f,
                    "Chamber reset discarded sink preference");
            ball.setBallFloating(true);
            ball.load(0);
            require(ball.ballFloats() && std::abs(ball.ballMass() - 60) < .001f,
                    "Chamber reset discarded float preference");
            Game fresh({lab::makePlayLevel(true, true)});
            require(!fresh.ballFloats() && std::abs(fresh.ballDensity() - 2500) < .01f,
                    "A new session inherited another session's Float selection");
            Game deep({lab::makePlayLevel(true, true, true)});
            require(!deep.ballFloats() && std::abs(deep.ballDensity() - 2500) < .01f,
                    "Deep pool must also default to solid sinking glass");
            Game legacy({lab::makePlayLevel()});
            legacy.setBallFloating(false);
            require(std::abs(legacy.ballMass() - 3.95f) < .001f,
                    "Water density option changed non-water gameplay mass");
        }
        Game game({lab::makePlayLevel(true, true)});
        game.setBallFloating(true); // Boat propulsion fixture uses the lightweight pilot.
        require(game.boatBody == 5 && game.poses().size() == 7, "Boat body/mesh layout");
        auto step = [&] {
            auto water = game.waterQueries();
            for (auto &p : water)
                p = {.8f, 0, 0, 0};
            game.receiveWater(water, 998.207f, 9.81f);
            game.step(1.f / 120);
        };
        for (int i = 0; i < 1200; ++i)
            step();
        auto pose = game.poses()[6];
        float equilibrium = .8f + .18f - watercraft::mass / (998.207f * 2 * .46f * 2.5f);
        require(std::abs(pose._42 - equilibrium) < .025f, "Boat does not settle at Archimedes draft");
        game.place(0, {pose._41, .72f, pose._43 - 2});
        require(game.toggleBoat() && game.piloting, "Board boat");
        float start = pose._43;
        game.input.forward = true;
        for (int i = 0; i < 240; ++i)
            step();
        pose = game.poses()[6];
        require(pose._43 < start - .4f, "Submerged propeller failed to move boat");
        game.input.right = true;
        for (int i = 0; i < 120; ++i)
            step();
        require(std::abs(game.poses()[6]._31) > .1f, "Steering torque failed");
        require(game.toggleBoat() && !game.piloting, "Leave boat");
        game.firstPerson = true;
        game.place(0, {0, .68f, 2});
        auto camera = game.camera(16.f / 9);
        require(camera.position.y < .85f && camera.position.y > .68f,
                "First person lens cannot stay underwater");
        game.firstPerson = false;
        game.elevation = -.5f;
        CameraFollow follow;
        camera = game.camera(16.f / 9, &follow, 1.f / 60);
        require(std::isfinite(camera.forward.x) && camera.position.y >= .25f,
                "Underwater orbit penetrates floor");
        std::cout << "PASS: equisolid projection, hydrostatic draft, boat propulsion/steering, entry/exit, "
                     "underwater camera, sink/float physics and passenger load\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
