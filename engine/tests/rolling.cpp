#include "../src/gameplay.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}

float length(XMFLOAT3 p) {
    return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
}
XMFLOAT3 subtract(XMFLOAT3 a, XMFLOAT3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
void settle(Game &game) {
    for (int i = 0; i < 360; ++i)
        game.step(1.f / 120);
}
void finite(const Camera &camera) {
    for (auto p : {camera.position, camera.forward, camera.right, camera.up})
        require(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z), "Nonfinite camera pose");
    for (const auto &matrix : {camera.view, camera.projection, camera.viewProjection})
        for (const auto &row : matrix.m)
            for (float value : row)
                require(std::isfinite(value), "Nonfinite camera matrix");
}
Camera advance(Game &game, CameraFollow &follow, float dt) {
    float previous = follow.distance;
    CameraFollow fresh;
    auto raw = game.camera(16.f / 9, &fresh, dt);
    auto camera = game.camera(16.f / 9, &follow, dt);
    finite(raw);
    finite(camera);
    require(follow.distance > 0 && follow.distance <= fresh.distance + .00001f,
            "Recovery put the camera beyond collision-safe clearance");
    if (previous >= 0) {
        float elapsed = game.paused || game.won ? 0 : std::clamp(dt, 0.f, .05f);
        require(follow.distance - previous <= 8.f * elapsed + .00001f,
                "Camera clearance popped outward faster than the recovery limit");
        if (fresh.distance <= previous)
            require(follow.distance == fresh.distance, "Camera failed to retract immediately");
    }
    return camera;
}
int main() {
    try {
        float worstLegacy240 = 0, worstFixed240 = 0;
        for (int hz : {60, 144, 240}) {
            for (float yaw : {0.f, .42f, XM_PIDIV2, -XM_PIDIV2, XM_PI}) {
                Game game({lab::makePlayLevel()});
                settle(game);
                game.azimuth = yaw;
                game.input.forward = true;
                CameraFollow follow;
                float maxLegacy = 0, maxFixed = 0, maxPlayer = 0, previousRadius = 0, previousFixed = 0;
                XMFLOAT3 previous{};
                for (int frame = 0; frame < hz * 4; ++frame) {
                    game.step(1.f / hz);
                    auto pose = game.poses()[1];
                    XMFLOAT3 ball{pose._41, pose._42, pose._43}, target{ball.x, ball.y + .48f, ball.z};
                    auto legacy = game.camera(16.f / 9);
                    auto camera = advance(game, follow, 1.f / hz);
                    require(length(subtract(camera.forward, legacy.forward)) < .0001f &&
                                length(subtract(camera.right, legacy.right)) < .0001f,
                            "Collision recovery changed orbit orientation");
                    float radius = length(subtract(legacy.position, target));
                    float fixedRadius = length(subtract(camera.position, target));
                    require(std::abs(fixedRadius - follow.distance) < .00001f,
                            "Follow camera is not anchored to the interpolated ball");
                    if (frame) {
                        maxLegacy = std::max(maxLegacy, std::abs(radius - previousRadius));
                        maxFixed = std::max(maxFixed, std::abs(fixedRadius - previousFixed));
                        float step = length(subtract(ball, previous));
                        maxPlayer = std::max(maxPlayer, step);
                        require(step >= 1e-7f || game.speed() <= .5f, "Rolling render pose stalled");
                    }
                    previous = ball;
                    previousRadius = radius;
                    previousFixed = fixedRadius;
                }
                require(maxFixed <= 8.f / hz + .001f, "Wall-edge route still contains a camera jump");
                if (hz == 240) {
                    worstLegacy240 = std::max(worstLegacy240, maxLegacy);
                    worstFixed240 = std::max(worstFixed240, maxFixed);
                }
                std::cout << "hz=" << hz << " yaw=" << yaw << " legacyRadiusStep=" << maxLegacy
                          << " fixedRadiusStep=" << maxFixed << " playerStep=" << maxPlayer << '\n';
            }
        }
        require(worstLegacy240 > .6f, "Regression route no longer exercises the original wall-edge pop");
        require(worstFixed240 < .035f, "240 Hz camera pop regression");

        Game game({lab::makePlayLevel()});
        settle(game);
        CameraFollow follow;
        game.resetHistory = false;
        bool sawJump = false;
        for (int frame = 0; frame < 900; ++frame) {
            const float cadence[] = {1.f / 240, 1.f / 144, 1.f / 60, .001f, .05f};
            float dt = cadence[frame % 5];
            game.input.forward = frame % 240 < 120;
            game.input.back = !game.input.forward;
            // Match real key-down events: retain a jump until a physics step consumes it.
            if (frame % 180 == 0)
                game.input.jump = true;
            game.azimuth += dt * .4f;
            game.step(dt);
            sawJump = sawJump || game.jumps > 0;
            if (game.resetHistory) {
                follow.reset();
                game.resetHistory = false;
            }
            advance(game, follow, dt);
        }
        require(sawJump, "Variable-cadence route did not exercise jumping");
        game.clearInput();
        game.paused = true;
        // Pausing publishes Bullet's current pose instead of its interpolated one;
        // the first paused camera may therefore need a collision-safe pull-in.
        advance(game, follow, 1.f / 60);
        float frozen = follow.distance;
        for (int i = 0; i < 30; ++i)
            advance(game, follow, 1.f / 60);
        require(follow.distance == frozen, "Paused camera kept recovering");
        game.paused = false;
        game.load(0);
        follow.reset();
        auto reset = advance(game, follow, 1.f / 240);
        CameraFollow fresh;
        auto raw = game.camera(16.f / 9, &fresh);
        require(length(subtract(reset.position, raw.position)) < .00001f,
                "Restart retained stale follow-camera recovery");
        require(game.beginTuning(), "Failed to enter precision camera fixture");
        for (int i = 0; i < 180; ++i) {
            game.step(1.f / 144);
            advance(game, follow, 1.f / 144);
        }
        require(game.tuningReady(), "Precision transition did not settle");
        require(length(subtract(game.camera(16.f / 9).position, game.camera(16.f / 9, &follow, 0).position)) <
                    .001f,
                "Follow recovery displaced the overhead tuning camera");
        game.endTuning();
        for (int i = 0; i < 180; ++i) {
            game.step(1.f / 144);
            advance(game, follow, 1.f / 144);
        }
        std::cout << "PASS: rolling interpolation, safe/rate-limited recovery, unchanged orbit, variable "
                     "cadence, reversal/jump, pause/restart and precision transitions; 240 Hz peak "
                  << worstLegacy240 << " -> " << worstFixed240 << " m/frame\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
