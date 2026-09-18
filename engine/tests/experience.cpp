#include "../src/experience.h"
#include "../src/gameplay.h"
#include "../src/ocean.h"
#include "../src/ocean_environment.h"
#include "watercraft.h"
#include <iostream>
#include <stdexcept>
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
void waterRoomControls() {
    // Run the same controller in both rooms, including positions beyond the
    // original room's walls. A large-room camera must stay with its player.
    for (bool large : {false, true}) {
        Game game({lab::makePlayLevel(true, true, false, large)});
        const auto &room = *game.level().roomBounds;
        const XMFLOAT3 player{large ? 9.f : -3.f, 1.8f, large ? 12.f : 3.f};
        game.place(0, player);
        game.firstPerson = true;
        auto camera = game.camera(16.f / 9);
        require(std::abs(camera.position.x - player.x) < 1e-5f &&
                    std::abs(camera.position.z - player.z) < 1e-5f &&
                    std::abs(camera.position.y - player.y - .12f) < 1e-5f,
                "Water-room first person detached from the player");
        game.firstPerson = false;
        game.distance = 3;
        CameraFollow follow;
        camera = game.camera(16.f / 9, &follow, 1.f / 60);
        require(std::abs(camera.position.x - player.x -
                         3 * std::cos(game.elevation) * std::sin(game.azimuth)) < 1e-4f &&
                    std::abs(camera.position.z - player.z -
                             3 * std::cos(game.elevation) * std::cos(game.azimuth)) < 1e-4f,
                "Water-room orbit retained another room's bounds");
        game.place(0, {room.half.x - 1.2f, 1.8f, player.z});
        game.azimuth = XM_PIDIV2;
        game.elevation = .1f;
        game.distance = 14;
        follow.reset();
        camera = game.camera(16.f / 9, &follow, 1.f / 60);
        require(camera.position.x <= room.half.x - .29f &&
                    camera.position.x > room.half.x - 1.2f,
                "Water-room orbit failed to stop at its actual wall");
        if (large) {
            game.place(0, {0, 5.5f, 12});
            game.azimuth = 0;
            game.elevation = .56f;
            game.distance = 3;
            follow.reset();
            camera = game.camera(16.f / 9, &follow, 1.f / 60);
            require(camera.position.y > 5.65f &&
                        camera.position.y <= room.center.y + room.half.y - .35f,
                    "Large-room orbit retained the small room's ceiling");
        }
        const XMFLOAT3 boat{large ? 8.f : 1.f, 1.8f, large ? 11.f : 5.f};
        game.place(game.boatBody, boat);
        game.place(0, {boat.x, boat.y + .5f, boat.z - 2});
        require(game.toggleBoat() && game.toggleBoat(), "Water-room boat entry/exit failed");
        require(std::abs(game.playerPosition().x - boat.x - 2.1f) < 1e-5f &&
                    std::abs(game.playerPosition().z - boat.z) < 1e-5f,
                "Leaving the boat teleported the player into the small room");
        game.place(game.boatBody, {room.half.x - 1.2f, boat.y, boat.z});
        game.place(0, {room.half.x - 2, boat.y + .5f, boat.z});
        require(game.toggleBoat() && game.toggleBoat(), "Wall-side boat entry/exit failed");
        require(std::abs(game.playerPosition().x - (room.half.x - .8f)) < 1e-5f,
                "Leaving the boat penetrated the room wall");
    }
    Game small({lab::makePlayLevel(true, true)});
    Game large({lab::makePlayLevel(true, true, false, true)});
    small.place(0, {-3, .70f, 5.8f});
    large.place(0, {8, .70f, 12});
    for (int i = 0; i < 120; ++i) {
        small.step(1.f / 120);
        large.step(1.f / 120);
    }
    const auto a = small.playerPosition(), b = large.playerPosition();
    for (auto *game : {&small, &large}) {
        game->azimuth = 0;
        game->input.forward = game->input.right = true;
    }
    for (int i = 0; i < 90; ++i) {
        small.step(1.f / 120);
        large.step(1.f / 120);
    }
    require(small.speed() > 1 && std::abs(small.speed() - large.speed()) < .01f &&
                std::abs(small.playerPosition().x - a.x - large.playerPosition().x + b.x) < .01f &&
                std::abs(small.playerPosition().z - a.z - large.playerPosition().z + b.z) < .01f,
            "Large Water Lab changed the small lab's movement response");
    for (auto *game : {&small, &large}) {
        game->clearInput();
        game->input.jump = true;
        game->step(1.f / 120);
        require(game->jumps == 1, "Water-room grounded jump failed");
    }
}
void oceanScene(const std::filesystem::path &runtime) {
    Game ocean({lab::makePlayLevel(true, true, false, false, true)});
    const auto &level = ocean.level();
    require(level.roomBounds && level.roomBounds->half.x == 128 && level.roomBounds->half.z == 128,
            "Ocean did not create the extra-large playable domain");
    require(level.terrain.size() > 10000 && level.terrain.size() % 3 == 0,
            "Island has no complete collision triangles");
    require(level.sensors.empty(), "Ocean retained the indoor receiver puzzle");
    for (const auto &p : level.terrain)
        require(std::abs(p.y - lab::ocean::oceanTerrainHeight(p.x,p.z)) < 1e-5f,
                "Island collision and fluid terrain differ");
    ocean.place(0, {-22, lab::ocean::oceanTerrainHeight(-22,-18)+.75f, -18});
    for (int i=0; i<180; ++i) ocean.step(1.f/120);
    auto p = ocean.playerPosition();
    require(ocean.grounded() && p.y > lab::ocean::oceanTerrainHeight(p.x,p.z)+.6f,
            "Player fell through the island");
    ocean.place(0, {4,7.25f,-8});
    for (int i=0; i<120; ++i) ocean.step(1.f/120);
    require(ocean.grounded() && ocean.playerPosition().y > 7.1f, "Pier has no physical deck");
    ocean.place(0, {85,6.8f,92});
    ocean.firstPerson = true;
    auto camera = ocean.camera(16.f/9);
    require(camera.position.x == 85 && camera.position.z == 92, "Ocean camera retained room-sized bounds");
    const auto assets = runtime / "assets/ocean";
    auto environment = lab::ocean::loadEnvironment(assets);
    const unsigned w = unsigned(environment[0].x), h = unsigned(environment[0].y);
    require(w == 1024 && h == 512 && environment.size() == 4 + 2 * (size_t(w) * h + h), "HDRI data size mismatch");
    for (unsigned layer=0;layer<2;++layer) {
        float previous = 0; double integral = 0;
        for (unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
            if (!x) previous = 0;
            const auto &v = environment[4+layer*(w*h+h)+y*w+x];
            require(std::isfinite(v.x+v.y+v.z+v.w) && v.x>=0 && v.y>=0 && v.z>=0 && v.w>previous,
                    "HDRI has invalid linear radiance or importance CDF");
            previous = v.w;
            if (x == w-1) require(previous == 1, "HDRI row probability is not normalized");
            const double pi = 3.141592653589793;
            integral += (.2126*v.x+.7152*v.y+.0722*v.z)*(2*pi/w)*
                (std::cos(pi*y/h)-std::cos(pi*(y+1)/h))*std::max(0.,std::cos(pi*(y+.5)/h));
        }
        const double expected = layer ? .002*4096/182.458 : 80000./128/182.458;
        require(previous == 1 && std::abs(integral/expected-1)<1e-5, "HDRI energy normalization changed");
        previous = 0;
        for (unsigned y=0;y<h;++y) {
            float cdf = environment[4+layer*(w*h+h)+w*h+y].x;
            require(cdf>previous && cdf<=1, "HDRI row CDF lost sampling support");
            previous = cdf;
        }
        require(previous == 1, "HDRI row selection is not normalized");
    }
    require(environment[1].y > 0 && environment[1].w > 0 && environment[2].w == 0,
            "Day/night sun selection is invalid");
}
void swimmingControls() {
    for (float hz : {30.f,60.f,144.f}) {
        Game game({lab::makePlayLevel(true,true,false,false,true)});
        game.setBallFloating(false);
        game.place(0,{30,.7f,30});
        auto advance=[&](float seconds,bool samples=true) {
            for(int i=0;i<int(seconds*hz);++i) {
                if(samples) {
                    auto water=game.waterQueries();
                    for(auto &p:water)p={6.02f,0,0,0};
                    game.receiveWater(water,1025,9.81f);
                }
                game.step(1/hz);
            }
        };
        advance(.5f);
        game.input.ascend=game.input.jump=true;
        advance(2);
        require(game.playerPosition().y>3 && game.playerPosition().y<3.8f && game.jumps==0,
                "Holding Space did not propel sinking glass smoothly off the seabed");
        require(!game.ballFloats() && game.ballDensity()>2400,"Swimming changed the ball density");
        float raised=game.playerPosition().y;
        game.input.ascend=false;
        advance(1);
        require(game.playerPosition().y<raised-.5f,"Releasing Space did not resume sinking");
        game.place(0,{30,3,30});game.input.ascend=game.input.dive=true;
        advance(.5f);
        require(game.playerPosition().y<2.6f,"Opposing swim controls did not cancel thrust");
        game.clearInput();require(!game.input.ascend,"Clearing input left swimming engaged");
        game.place(0,{30,3,30});game.input.ascend=true;
        advance(1,false);
        require(game.playerPosition().y<2,"Stale water samples allowed airborne propulsion");
        game.clearInput();advance(2,false);
        game.input.jump=game.input.ascend=true;game.step(1/hz);
        require(game.jumps==1,"Space no longer jumps on dry ground");
        advance(.3f,false);
        require(game.jumps==1,"Holding Space repeated an airborne jump");
    }
}
int main(int argc, char **argv) {
    try {
        swimmingControls();
        oceanScene(std::filesystem::absolute(argv[0]).parent_path());
        waterRoomControls();
        require(lab::Lens{}.diagonalDegrees == 90, "Default starting FOV regressed");
        require(!lab::ExperienceSettings{}.ballFloats, "Water Lab must default to solid sinking glass");
        for (float aspect : {1.f, 16.f / 9, 32.f / 9}) {
            lab::Lens lens;
            require(!lens.fisheye, "Default launch must retain the normal lens");
            for (float fov : {90.f, 120.f, 160.f}) {
                lens.diagonalDegrees = fov;
                const float diagonal = 2 * std::atan(lens.tanHalfVertical(aspect) *
                                                     std::sqrt(1 + aspect * aspect));
                require(std::abs(diagonal - fov * XM_PI / 180) < 1e-5,
                        "Normal camera ignored the selected diagonal FOV");
                float x = .6f, y = -.4f;
                lens.rectilinear(x, y, aspect);
                require(x == .6f && y == -.4f, "Normal lens warped picking coordinates");
            }
        }
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
        Game game({lab::makePlayLevel(true, true, false, true)});
        game.setBallFloating(true); // Boat propulsion fixture uses the lightweight pilot.
        game.place(game.boatBody,{7,1.3f,10}); // Room for the larger hull's powered turning circle.
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
        std::cout << "Boat equilibrium: height=" << pose._42 << " up=" << pose._22
                  << " supported kg=" << game.submergedBoat*998.207f << '\n';
        require(std::abs(game.submergedBoat*998.207f-watercraft::mass)<watercraft::mass*.04f,
                "Tapered boat does not settle at Archimedes displacement");
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
                     "underwater camera, room-sized camera/boat bounds, movement parity, "
                     "sink/float physics, sustained swimming and passenger load\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
