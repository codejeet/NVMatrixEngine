#pragma once
#include "camera.h"
#include <DirectXMath.h>
#include <btBulletDynamicsCommon.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
using namespace DirectX;
struct Vertex {
    XMFLOAT3 position, normal;
    uint32_t material, pad;
};
struct Material {
    XMFLOAT4 colorRoughness, properties, emission;
};
struct Solid {
    XMFLOAT3 center, half;
};
struct Prop {
    XMFLOAT3 position, half;
    float yaw;
    uint32_t kind, upright, fixed;
    XMFLOAT4 dockYaw, optical; // position + solution yaw; shape, curvature, aperture, center half-thickness
};
struct Sensor {
    XMFLOAT4 centerMaterial, half, band; // nm min/max, lock threshold, launched band energy
    XMUINT4 required;                    // reflections, transmissions, exited-optic mask, reserved
};
struct Level {
    std::string name, objective, hint;
    XMFLOAT3 color, receiver, receiverHalf, source, direction, ballStart;
    uint32_t reflections, transmissions;
    XMFLOAT4 launch; // spectral packet, spatial radius, aperture samples, monochromatic wavelength
    std::vector<Sensor> sensors;
    std::vector<Solid> solids;
    std::vector<Prop> props;
    std::vector<Material> materials;
    std::vector<std::vector<Vertex>> meshes;
};
std::vector<Level> loadLevels(const std::filesystem::path &path);
struct Input {
    bool forward = false, back = false, left = false, right = false, jump = false, turnLeft = false,
         turnRight = false, fine = false, dive = false;
};
constexpr uint32_t MaxBeams = 256, MaxSensors = 8, BeamNodes = 512;
struct LaserResult {
    XMFLOAT4 stats, received, reserved0, reserved1;
    std::array<XMFLOAT4, MaxSensors> sensors;
    struct Beam {
        XMFLOAT4 a, b, power, normal;
    };
    std::array<Beam, MaxBeams> beams;
    struct Node {
        XMFLOAT4 low, high;
    };
    std::array<Node, BeamNodes> nodes;
};
static_assert(sizeof(Vertex) == 32 && sizeof(Material) == 48 && sizeof(Solid) == 24 && sizeof(Prop) == 72 &&
                  sizeof(Sensor) == 64 && sizeof(LaserResult) == 32960,
              "CPU, exporter and HLSL layouts must agree");
// Optional render-camera state. Legacy callers keep their existing point-ray camera.
struct CameraFollow {
    float distance = -1;
    void reset() {
        distance = -1;
    }
};
class Game {
  public:
    explicit Game(std::vector<Level> levels);
    ~Game();
    void load(int chapter);
    void step(float delta);
    void receive(const LaserResult &laser, float delta);
    void grab();
    void throwHeld();
    bool dock(int body = -1);
    bool rotate(float angle, int body = -1);
    bool beginTuning();
    void endTuning(bool cancel = false);
    bool tuneMove(float dx, float dz);
    bool tuneRotate(float radians);
    XMFLOAT3 opticPosition() const;
    float opticYaw() const;
    int tuning = -1;
    bool tuningReady() const {
        return tuning >= 0 && tuneBlend > .995f;
    }
    uint64_t tuneMoves = 0, tuneTurns = 0;
    int nearbyOptic() const;
    bool grounded() const;
    Camera camera(float aspect, CameraFollow *follow = nullptr, float delta = 0) const;
    std::vector<XMFLOAT4X4> poses() const;
    void place(int body, XMFLOAT3 p, float yaw = 0); // Explicit fixture/gameplay teleport, not ordinary motion.
    // Explicit discontinuities (also used when exiting the boat), not fast
    // ordinary motion. The lab's fluid collider consumer clears these flags.
    std::vector<uint8_t> poseDiscontinuities;
    const Level &level() const {
        return levels[chapter];
    }
    XMFLOAT3 playerPosition() const;
    float speed() const;
    bool firstPerson = false;
    int boatBody = -1;
    bool piloting = false;
    bool nearBoat() const;
    bool toggleBoat();
    void setBallFloating(bool floating);
    bool ballFloats() const {
        return ballFloating;
    }
    float ballMass() const;
    float ballDensity() const;
    // Small body-level readback, not particles. Heights/velocities are sampled
    // from the GPU fluid field; callers deliver results after their existing fence.
    std::vector<XMFLOAT4> waterQueries() const;
    void receiveWater(const std::vector<XMFLOAT4> &samples, float density, float gravity);
    float submergedBoat = 0;
    void clearInput() {
        input = {};
    }
    std::vector<Level> levels;
    Input input;
    int chapter = 0, held = -1, completed = 0, throws = 0, jumps = 0;
    bool paused = false, ui = false, hint = false, won = false, docked = false, gateOpen = false,
         resetHistory = true;
    float azimuth = .42f, elevation = .56f, distance = 9.5f, charge = 0, gateLift = 0, elapsed = 0, power = 0,
          glow = 1;
    std::string toast;
    std::array<float, MaxSensors> sensorPower{};

  private:
    struct FloatPoint {
        int body;
        XMFLOAT3 local, query;
        float volume, halfHeight;
    };
    std::vector<FloatPoint> floatPoints;
    std::vector<XMFLOAT4> waterSamples;
    float waterDensity = 998.207f, waterGravity = 9.81f, waterAge = 100;
    // Construct solid glass immediately, even before a renderer/UI exists.
    // The selected session preference still survives chamber/water resets.
    bool ballFloating = false;
    void waterForces(float dt);
    std::unique_ptr<btDefaultCollisionConfiguration> config;
    std::unique_ptr<btCollisionDispatcher> dispatcher;
    std::unique_ptr<btDbvtBroadphase> broadphase;
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
    std::unique_ptr<btDiscreteDynamicsWorld> world;
    std::vector<std::unique_ptr<btCollisionShape>> shapes;
    std::vector<std::unique_ptr<btRigidBody>> owned;
    std::vector<btRigidBody *> bodies;
    std::vector<btTransform> previousStep;
    std::vector<bool> seated;
    float accumulator = 0, jumpCooldown = 0;
    float tuneBlend = 0;
    XMFLOAT3 tuneFocus{};
    btTransform tuneStart;
    bool tuneTransform(const btTransform &candidate);
    void destroyWorld();
    btRigidBody *add(btCollisionShape *shape, btVector3 position, float mass, bool upright = false);
    void setFixed(btRigidBody *body, bool fixed);
};
void runGameTests(const std::filesystem::path &assets);
