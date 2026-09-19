#include "../src/gameplay.h"
#include "../src/model_asset.h"
#include "../src/scene.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void advance(Game &game, int steps) { for (int i = 0; i < steps; ++i) game.step(1.f / 120); }
int main(int argc, char **argv) {
    try {
        require(argc == 2, "Pass the Neon Night glTF path");
        auto asset = lab::asset::loadModel(argv[1]);
        std::vector<lab::Mesh> meshes;
        size_t vertices = 0;
        for (const auto &source : asset.meshes) {
            lab::Mesh mesh; mesh.mask = 1;
            for (const auto &v : source.vertices)
                mesh.vertices.push_back({{v.position.x, v.position.y, v.position.z}, lab::asset::materialBase, {}, ~0u, 0});
            vertices += mesh.vertices.size();
            meshes.push_back(std::move(mesh));
        }
        meshes.insert(meshes.begin() + 1, lab::makePlayerMesh());
        meshes.insert(meshes.begin() + 2, lab::makeNeonBlockMesh(4));
        meshes.insert(meshes.begin() + 3, lab::makeNeonBlockMesh(5));
        auto level = lab::makeNeonLevel(meshes);
        require(level.terrain.size() == vertices && vertices > 300000, "Imported collision geometry differs from DXR");
        require(level.props.size() == 2 && level.sensors.empty(), "Alley blocks/puzzle mapping differs");
        Game game({std::move(level)});
        require(game.poses().size() == 4, "World/player/block instance mapping differs");
        advance(game, 240);
        require(game.grounded() && std::abs(game.playerPosition().y - .68f) < .03f, "Player fell through the imported road");
        auto start = game.playerPosition();
        game.input.forward = true; advance(game, 120); game.clearInput();
        require(game.playerPosition().z > start.z + 2, "W movement did not roll down the alley");
        game.input.jump = true; advance(game, 12);
        require(game.jumps == 1 && game.playerPosition().y > 1, "Jump failed on imported pavement");
        game.paused = true; auto paused = game.playerPosition();
        game.input.forward = true; advance(game, 120);
        require(game.playerPosition().x == paused.x && game.playerPosition().z == paused.z, "Pause did not stop physics");
        game.paused = false; game.load(0);
        require(std::abs(game.azimuth - XM_PI) < 1e-5 && game.distance == 5.5f, "Restart lost the alley camera");
        game.place(0, {0, .72f, -6}); advance(game, 120);
        game.input.left = true; advance(game, 240); game.clearInput();
        require(game.playerPosition().x > 2 && game.playerPosition().x < 2.9f, "Imported storefront wall did not block the ball");
        game.azimuth = XM_PIDIV2; CameraFollow follow;
        auto camera = game.camera(16.f/9, &follow, 1.f/60);
        require(camera.position.x < 3.25f && follow.distance < 1, "Camera passed through imported storefront geometry");
        game.load(0); game.place(0, {0, .72f, 1.4f}); advance(game, 120);
        game.input.right = true; advance(game, 240); game.clearInput();
        // The scanned bin and bags are rounded: sustained sideways input rolls
        // around them. Without their colliders this path would keep z == 1.4.
        require(game.playerPosition().x < -.4f && game.playerPosition().z < .8f,
                "Imported bin/bags did not deflect the player");
        LaserResult signal{}; signal.stats.y = 100;
        for (int i=0;i<300;++i) { game.receive(signal, 1.f/60); game.step(1.f/120); }
        require(!game.gateOpen && !game.won, "Free exploration triggered a nonexistent gate");
        game.load(0); advance(game, 120);
        game.place(0, {-1.65f,.72f,-1.8f});
        game.grab(); require(game.held == 1, "Pink neon block cannot be picked up");
        game.throwHeld(); require(game.held == -1 && game.throws == 1, "Neon block cannot be thrown");
        advance(game, 90);
        require(game.poses()[2]._43 > 1, "Thrown neon block did not move through the scene");
        game.load(0);
        require(std::abs(game.poses()[2]._43 + .3f) < .001f && std::abs(game.poses()[3]._43 - 4.5f) < .001f,
                "Restart did not restore both neon blocks");
        std::cout << "PASS: Neon collisions, player/camera controls, block pickup/throw/restart and free exploration\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
