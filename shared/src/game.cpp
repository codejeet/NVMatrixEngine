#include "game.h"
#include "watercraft.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <iostream>
namespace {
btVector3 v(XMFLOAT3 p) {
    return {p.x, p.y, p.z};
}
XMFLOAT3 f(btVector3 p) {
    return {p.x(), p.y(), p.z()};
}
template <class T> T read(std::istream &s) {
    T x{};
    if (!s.read(reinterpret_cast<char *>(&x), sizeof(x)))
        throw std::runtime_error("Truncated chamber asset");
    return x;
}
uint32_t count(std::istream &s, uint32_t max) {
    auto n = read<uint32_t>(s);
    if (n > max)
        throw std::runtime_error("Invalid chamber asset count");
    return n;
}
std::string text(std::istream &s) {
    std::string x(count(s, 8192), ' ');
    if (!s.read(x.data(), x.size()))
        throw std::runtime_error("Truncated chamber text");
    return x;
}
struct RayIgnore : btCollisionWorld::ClosestRayResultCallback {
    const btCollisionObject *ignore;
    bool staticsOnly;
    RayIgnore(btVector3 a, btVector3 b, const btCollisionObject *object, bool statics = false)
        : ClosestRayResultCallback(a, b), ignore(object), staticsOnly(statics) {}
    bool needsCollision(btBroadphaseProxy *proxy) const override {
        auto object = static_cast<btCollisionObject *>(proxy->m_clientObject);
        return object != ignore && (!staticsOnly || object->isStaticObject()) &&
               ClosestRayResultCallback::needsCollision(proxy);
    }
};
struct CameraSweep : btCollisionWorld::ClosestConvexResultCallback {
    const btCollisionObject *ignore;
    CameraSweep(btVector3 a, btVector3 b, const btCollisionObject *object)
        : ClosestConvexResultCallback(a, b), ignore(object) {}
    bool needsCollision(btBroadphaseProxy *proxy) const override {
        auto object = static_cast<btCollisionObject *>(proxy->m_clientObject);
        return object != ignore && object->isStaticObject() &&
               ClosestConvexResultCallback::needsCollision(proxy);
    }
};
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
} // namespace
std::vector<Level> loadLevels(const std::filesystem::path &path) {
    std::ifstream s(path, std::ios::binary);
    if (!s)
        throw std::runtime_error("Cannot open assets/chambers.bin");
    check(read<uint32_t>(s) == 0x4e4d554c && read<uint32_t>(s) == 2, "Invalid chamber asset header");
    std::vector<Level> levels(count(s, 32));
    check(!levels.empty(), "Expected a nonempty campaign");
    for (auto &l : levels) {
        l.name = text(s);
        l.objective = text(s);
        l.hint = text(s);
        l.color = read<XMFLOAT3>(s);
        l.receiver = read<XMFLOAT3>(s);
        l.receiverHalf = read<XMFLOAT3>(s);
        l.source = read<XMFLOAT3>(s);
        l.direction = read<XMFLOAT3>(s);
        l.ballStart = read<XMFLOAT3>(s);
        l.reflections = read<uint32_t>(s);
        l.transmissions = read<uint32_t>(s);
        l.launch = read<XMFLOAT4>(s);
        l.sensors.resize(count(s, MaxSensors));
        check(!l.sensors.empty(), "A chamber needs at least one receiver");
        for (auto &sensor : l.sensors) {
            sensor = read<Sensor>(s);
            check(sensor.band.z > 0 && sensor.band.w > 0, "Invalid spectral receiver calibration");
        }
        l.solids.resize(count(s, 128));
        for (auto &x : l.solids)
            x = read<Solid>(s);
        l.props.resize(count(s, 32));
        for (auto &x : l.props)
            x = read<Prop>(s);
        l.materials.resize(count(s, 256));
        for (auto &x : l.materials)
            x = read<Material>(s);
        l.meshes.resize(count(s, 34));
        for (auto &mesh : l.meshes) {
            mesh.resize(count(s, 1000000));
            check(mesh.size() % 3 == 0, "Invalid triangle count");
            for (auto &x : mesh) {
                x = read<Vertex>(s);
                check(x.material < l.materials.size(), "Invalid material index");
            }
        }
        check(l.meshes.size() == l.props.size() + 2, "Mesh/physics body mismatch");
    }
    return levels;
}
Game::Game(std::vector<Level> data) : levels(std::move(data)) {
    load(0);
}
Game::~Game() {
    destroyWorld();
}
void Game::destroyWorld() {
    if (world)
        for (auto &b : owned)
            world->removeRigidBody(b.get());
    bodies.clear();
    owned.clear();
    shapes.clear();
    terrainMesh.reset();
    world.reset();
    solver.reset();
    broadphase.reset();
    dispatcher.reset();
    config.reset();
}
btRigidBody *Game::add(btCollisionShape *shape, btVector3 p, float mass, bool upright) {
    shapes.emplace_back(shape);
    btVector3 inertia(0, 0, 0);
    if (mass)
        shape->calculateLocalInertia(mass, inertia);
    btRigidBody::btRigidBodyConstructionInfo info(mass, nullptr, shape, inertia);
    info.m_friction = .85f;
    info.m_restitution = .28f;
    info.m_linearDamping = .22f;
    info.m_angularDamping = .35f;
    auto body = std::make_unique<btRigidBody>(info);
    btTransform pose;
    pose.setIdentity();
    pose.setOrigin(p);
    body->setWorldTransform(pose);
    body->setCcdMotionThreshold(.05f);
    body->setCcdSweptSphereRadius(.08f);
    if (upright) {
        body->setAngularFactor({0, 1, 0});
        body->setDamping(.22f, .98f);
    }
    world->addRigidBody(body.get());
    auto ptr = body.get();
    owned.push_back(std::move(body));
    return ptr;
}
void Game::load(int index) {
    poseDiscontinuities.clear();
    boatBody = -1;
    piloting = false;
    floatPoints.clear();
    waterSamples.clear();
    waterAge = 100;
    tuning = -1;
    tuneBlend = 0;
    destroyWorld();
    chapter = std::clamp(index, 0, int(levels.size()) - 1);
    const auto &l = level();
    config = std::make_unique<btDefaultCollisionConfiguration>();
    dispatcher = std::make_unique<btCollisionDispatcher>(config.get());
    broadphase = std::make_unique<btDbvtBroadphase>();
    solver = std::make_unique<btSequentialImpulseConstraintSolver>();
    world = std::make_unique<btDiscreteDynamicsWorld>(dispatcher.get(), broadphase.get(), solver.get(),
                                                      config.get());
    world->setGravity({0, -16, 0});
    world->getSolverInfo().m_numIterations = 12;
    for (auto &s : l.solids)
        add(new btBoxShape(v(s.half)), v(s.center), 0);
    if (!l.terrain.empty()) {
        terrainMesh = std::make_unique<btTriangleMesh>();
        for (size_t i = 0; i < l.terrain.size(); i += 3)
            terrainMesh->addTriangle(v(l.terrain[i]), v(l.terrain[i + 1]), v(l.terrain[i + 2]), false);
        add(new btBvhTriangleMeshShape(terrainMesh.get(), true), {0, 0, 0}, 0);
    }
    for (const auto &sensor : l.sensors)
        add(new btBoxShape({sensor.half.x, sensor.half.y, sensor.half.z}),
            {sensor.centerMaterial.x, sensor.centerMaterial.y, sensor.centerMaterial.z}, 0);
    bodies.push_back(add(new btSphereShape(.68f), v(l.ballStart), 3.95f));
    bodies[0]->setCcdSweptSphereRadius(.60f);
    for (size_t i = 0; i < l.props.size(); i++) {
        const auto &p = l.props[i];
        float mass = p.fixed ? 0 : 8 * p.half.x * p.half.y * p.half.z * 1.3f;
        btCollisionShape *shape = nullptr;
        if (p.kind == 5) {
            auto hull = new btConvexHullShape();
            for (const auto &vertex : l.meshes[i + 2])
                hull->addPoint(v(vertex.position), false);
            hull->recalcLocalAabb();
            hull->setMargin(.002f);
            shape = hull;
        } else if (p.kind == 8) {
            auto hull = new btConvexHullShape();
            for (auto vertex : watercraft::hullVertices()) hull->addPoint(v(vertex), false);
            hull->recalcLocalAabb(); hull->setMargin(.002f);
            shape = hull;
            mass = watercraft::mass;
            boatBody = int(i + 1);
        } else if (p.kind == 6 || p.kind == 7) {
            // A concave lens must not receive a solid box/convex-hull collider.
            // Small radial convex wedges approximate its actual curved volume.
            auto compound = new btCompoundShape();
            const float sign = p.kind == 6 ? 1.f : -1.f;
            constexpr int sectors = 24, rings = 4;
            for (int ring = 0; ring < rings; ring++)
                for (int sector = 0; sector < sectors; sector++) {
                    auto wedge = new btConvexHullShape();
                    for (int r = ring; r <= ring + 1; r++)
                        for (int a = sector; a <= sector + 1; a++) {
                            float rho = p.optical.z * r / rings, angle = XM_2PI * a / sectors;
                            float x = p.optical.w -
                                      sign * (p.optical.y - std::sqrt(p.optical.y * p.optical.y - rho * rho));
                            for (float side : {-1.f, 1.f})
                                wedge->addPoint({side * x, rho * std::cos(angle), rho * std::sin(angle)},
                                                false);
                        }
                    wedge->recalcLocalAabb();
                    wedge->setMargin(.002f);
                    shapes.emplace_back(wedge);
                    compound->addChildShape(btTransform::getIdentity(), wedge);
                }
            shape = compound;
        } else
            shape = new btBoxShape(v(p.half));
        bodies.push_back(add(shape, v(p.position), mass, p.upright != 0));
        auto t = bodies.back()->getWorldTransform();
        t.setRotation(btQuaternion({0, 1, 0}, p.yaw));
        bodies.back()->setWorldTransform(t);
    }
    if (boatBody >= 0) {
        world->setGravity({0, -waterGravity, 0});
        // Gameplay weights remain unchanged in non-water chambers. Here the
        // ball uses the selected density and throwables float. Pilot load equals
        // the actual ball mass.
        for (int body : {0, 2, 3}) {
            float mass = body ? 8 * .38f * .38f * .38f * 700 : watercraft::ballMass(ballFloating);
            btVector3 inertia;
            bodies[body]->getCollisionShape()->calculateLocalInertia(mass, inertia);
            bodies[body]->setMassProps(mass, inertia);
            bodies[body]->updateInertiaTensor();
        }
        for (int body : {0, 2, 3})
            for (float x : {-1.f, 1.f})
                for (float z : {-1.f, 1.f}) {
                    float h = body ? .38f : .68f;
                    floatPoints.push_back({body,
                                           {x * h * .5f, 0, z * h * .5f},
                                           {x * (h + .15f), 0, z * (h + .15f)},
                                           body ? 2 * h * h * h : watercraft::sphereVolume(h, 2 * h) / 4,
                                           h});
                }
        for (unsigned i=0;i<watercraft::stations;++i) {
            float z=watercraft::bow+(i+.5f)*watercraft::stationLength;
            for (float side : {-1.f,1.f}) {
                float beam=watercraft::halfBeam(z,watercraft::deck);
                floatPoints.push_back({boatBody,{side*beam*.55f,0,z},{side*(beam+.3f),0,z},
                    watercraft::columnVolume(z,watercraft::deck), (watercraft::deck-watercraft::keel)*.5f});
            }
        }
    }
    held = -1;
    docked = false;
    seated.assign(bodies.size(), false);
    sensorPower.fill(0);
    charge = 0;
    power = 0;
    gateLift = 0;
    gateOpen = false;
    won = false;
    accumulator = 0;
    jumpCooldown = 0;
    input = {};
    previousStep.clear();
    resetHistory = true;
    azimuth = .42f;
    elevation = .56f;
    distance = 9.5f;
    hint = false;
    toast = l.objective;
}
bool Game::grounded() const {
    if (bodies[0]->getLinearVelocity().y() > 1)
        return false;
    for (auto offset :
         std::array<btVector3, 5>{{{0, 0, 0}, {.22f, 0, 0}, {-.22f, 0, 0}, {0, 0, .22f}, {0, 0, -.22f}}}) {
        const auto a = bodies[0]->getWorldTransform().getOrigin() + offset, b = a + btVector3(0, -.765f, 0);
        RayIgnore ray(a, b, bodies[0]);
        world->rayTest(a, b, ray);
        if (ray.hasHit() && ray.m_hitNormalWorld.y() > .55f)
            return true;
    }
    return false;
}
XMFLOAT3 Game::playerPosition() const {
    return f(bodies[0]->getWorldTransform().getOrigin());
}
float Game::speed() const {
    auto vel = bodies[0]->getLinearVelocity();
    return std::hypot(vel.x(), vel.z());
}
void Game::setFixed(btRigidBody *b, bool fixed) {
    world->removeRigidBody(b);
    btScalar mass = 0;
    btVector3 inertia(0, 0, 0);
    if (!fixed) {
        auto found = std::find(bodies.begin(), bodies.end(), b);
        check(found != bodies.end() && found != bodies.begin(), "Invalid optic body");
        auto h = level().props[size_t(found - bodies.begin()) - 1].half;
        mass = 8 * h.x * h.y * h.z * 1.3f;
        b->getCollisionShape()->calculateLocalInertia(mass, inertia);
    }
    b->setMassProps(mass, inertia);
    b->updateInertiaTensor();
    b->setLinearVelocity({0, 0, 0});
    b->setAngularVelocity({0, 0, 0});
    b->clearForces();
    world->addRigidBody(b);
    b->activate(true);
}
void Game::grab() {
    int preferred = tuning;
    if (tuning >= 0)
        endTuning();
    if (held >= 0) {
        bodies[held]->clearForces();
        held = -1;
        return;
    }
    auto p = bodies[0]->getWorldTransform().getOrigin();
    btVector3 forward(-std::sin(azimuth), 0, -std::cos(azimuth));
    float best = 4.5f;
    for (size_t i = 1; i < bodies.size(); i++) {
        if (level().props[i - 1].fixed || int(i) == boatBody)
            continue;
        auto delta = bodies[i]->getWorldTransform().getOrigin() - p;
        if (delta.length() < best && delta.dot(forward) > -.4f) {
            best = delta.length();
            held = int(i);
        }
    }
    if (preferred > 0 && size_t(preferred) < bodies.size() && !level().props[preferred - 1].fixed &&
        (bodies[preferred]->getWorldTransform().getOrigin() - p).length() < 5.f)
        held = preferred;
    if (held >= 0) {
        if (seated[held]) {
            setFixed(bodies[held], false);
            seated[held] = false;
            docked = std::find(seated.begin(), seated.end(), true) != seated.end();
        }
        bodies[held]->activate(true);
    }
}
void Game::throwHeld() {
    if (held < 0)
        return;
    auto b = bodies[held];
    b->clearForces();
    auto velocity = bodies[0]->getLinearVelocity();
    b->setLinearVelocity(velocity + btVector3(-std::sin(azimuth) * 14, 6, -std::cos(azimuth) * 14));
    b->setAngularVelocity({3.5f, 5, 2.4f});
    b->activate(true);
    held = -1;
    throws++;
}
int Game::nearbyOptic() const {
    auto isOptic = [&](int i) {
        if (i <= 0 || size_t(i) >= bodies.size())
            return false;
        const auto kind = level().props[i - 1].kind;
        return kind == 1 || kind == 2 || kind >= 5;
    };
    if (isOptic(held))
        return held;
    int selected = -1;
    float nearest = 3.8f;
    auto p = bodies[0]->getWorldTransform().getOrigin();
    for (int i = 1; i < int(bodies.size()); i++) {
        auto delta = bodies[i]->getWorldTransform().getOrigin() - p;
        float d = std::hypot(delta.x(), delta.z());
        if (isOptic(i) && d < nearest) {
            selected = i;
            nearest = d;
        }
    }
    return selected;
}
bool Game::dock(int body) {
    if (body < 0)
        body = nearbyOptic();
    if (body <= 0 || size_t(body) >= bodies.size())
        return false;
    const auto &prop = level().props[body - 1];
    if (!(prop.kind == 1 || prop.kind == 2 || prop.kind >= 5))
        return false;
    auto b = bodies[body];
    auto q = b->getWorldTransform().getOrigin(), p = bodies[0]->getWorldTransform().getOrigin();
    const auto dock = prop.dockYaw;
    if (std::hypot(q.x() - dock.x, q.z() - dock.z) > 2.6f || std::hypot(p.x() - dock.x, p.z() - dock.z) > 4)
        return false;
    auto t = b->getWorldTransform();
    t.setOrigin({dock.x, dock.y, dock.z});
    auto local = t.inverse() * p;
    auto half = v(prop.half);
    btVector3 d = local.absolute() - half;
    for (int i = 0; i < 3; i++)
        d[i] = std::max(0.f, d[i]);
    if (d.length() < .73f)
        return false;
    b->setWorldTransform(t);
    setFixed(b, true);
    held = -1;
    docked = true;
    seated[body] = true;
    world->updateSingleAabb(b);
    previousStep.clear();
    resetHistory = true;
    return true;
}
bool Game::rotate(float angle, int body) {
    if (body < 0 && tuning >= 0)
        return tuneRotate(angle);
    if (body < 0)
        body = nearbyOptic();
    if (body <= 0 || size_t(body) >= bodies.size())
        return false;
    auto b = bodies[body], player = bodies[0];
    auto delta = b->getWorldTransform().getOrigin() - player->getWorldTransform().getOrigin();
    if (held != body && std::hypot(delta.x(), delta.z()) > 3.8f)
        return false;
    auto t = b->getWorldTransform();
    auto q = t.getRotation();
    float yaw = 2 * std::atan2(q.y(), q.w()) + angle;
    t.setRotation(btQuaternion({0, 1, 0}, yaw));
    b->setWorldTransform(t);
    b->setAngularVelocity({0, 0, 0});
    b->activate(true);
    world->updateSingleAabb(b);
    tuneTurns++;
    return true;
}
bool Game::beginTuning() {
    if (tuning >= 0)
        return true;
    int body = nearbyOptic();
    if (body < 1 || (!seated[body] && !dock(body)))
        return false;
    tuning = body;
    tuneStart = bodies[body]->getWorldTransform();
    tuneFocus = f(tuneStart.getOrigin());
    // Include the receiver end of the path, not just a magnified view of the object.
    XMFLOAT3 receivers{};
    for (const auto &s : level().sensors) {
        receivers.x += s.centerMaterial.x;
        receivers.z += s.centerMaterial.z;
    }
    if (!level().sensors.empty()) {
        tuneFocus.x = tuneFocus.x * .75f + receivers.x / float(level().sensors.size()) * .25f;
        tuneFocus.z = tuneFocus.z * .75f + receivers.z / float(level().sensors.size()) * .25f;
    }
    clearInput();
    bodies[0]->setLinearVelocity({0, 0, 0});
    bodies[0]->setAngularVelocity({0, 0, 0});
    toast = "Precision mode. Drag to move; scroll to rotate. Shift makes smaller adjustments.";
    return true;
}
void Game::endTuning(bool cancel) {
    if (tuning < 0)
        return;
    if (cancel) {
        bodies[tuning]->setWorldTransform(tuneStart);
        world->updateSingleAabb(bodies[tuning]);
        previousStep.clear();
        resetHistory = true;
    }
    tuning = -1;
    clearInput();
    toast = cancel ? "Adjustment cancelled. Optic remains locked."
                   : "Position saved. E to pick up; F to fine-tune again.";
}
XMFLOAT3 Game::opticPosition() const {
    int i = tuning >= 0 ? tuning : nearbyOptic();
    return i > 0 ? f(bodies[i]->getWorldTransform().getOrigin()) : XMFLOAT3{};
}
float Game::opticYaw() const {
    int i = tuning >= 0 ? tuning : nearbyOptic();
    if (i < 1)
        return 0;
    auto q = bodies[i]->getWorldTransform().getRotation();
    return std::remainder(2 * std::atan2(q.y(), q.w()), XM_2PI);
}
bool Game::tuneTransform(const btTransform &candidate) {
    if (tuning < 1 || paused || won)
        return false;
    const auto &prop = level().props[tuning - 1];
    auto p = candidate.getOrigin();
    if (!std::isfinite(p.x()) || !std::isfinite(p.z()) ||
        std::hypot(p.x() - prop.dockYaw.x, p.z() - prop.dockYaw.z) > .85f) {
        toast = "Cradle travel limit reached (0.85 m).";
        return false;
    }
    struct Contact : btCollisionWorld::ContactResultCallback {
        const btCollisionObject *ignore;
        float bottom;
        bool blocked = false;
        Contact(const btCollisionObject *object, float y) : ignore(object), bottom(y) {}
        btScalar addSingleResult(btManifoldPoint &point, const btCollisionObjectWrapper *a, int, int,
                                 const btCollisionObjectWrapper *b, int, int) override {
            if (a->getCollisionObject() != ignore && b->getCollisionObject() != ignore &&
                point.getDistance() < -.015f &&
                std::max(point.getPositionWorldOnA().y(), point.getPositionWorldOnB().y()) > bottom + .08f)
                blocked = true;
            return 0;
        }
    } contact(bodies[tuning], p.y() - prop.half.y);
    btCollisionObject probe;
    probe.setCollisionShape(bodies[tuning]->getCollisionShape());
    probe.setWorldTransform(candidate);
    world->contactTest(&probe, contact);
    if (contact.blocked) {
        toast = "Adjustment blocked. Leave clearance around the optic.";
        return false;
    }
    auto b = bodies[tuning];
    b->setWorldTransform(candidate);
    b->setLinearVelocity({0, 0, 0});
    b->setAngularVelocity({0, 0, 0});
    world->updateSingleAabb(b);
    // Immediate laser response; previous rendered poses still supply correct DLSS motion.
    if (previousStep.size() == bodies.size())
        previousStep[tuning] = candidate;
    return true;
}
bool Game::tuneMove(float dx, float dz) {
    if (tuning < 1 || !std::isfinite(dx) || !std::isfinite(dz))
        return false;
    auto t = bodies[tuning]->getWorldTransform();
    t.setOrigin(t.getOrigin() + btVector3(dx, 0, dz));
    if (!tuneTransform(t))
        return false;
    tuneMoves++;
    return true;
}
bool Game::tuneRotate(float radians) {
    if (tuning < 1 || !std::isfinite(radians))
        return false;
    auto t = bodies[tuning]->getWorldTransform();
    t.setRotation(btQuaternion({0, 1, 0}, opticYaw() + radians));
    if (!tuneTransform(t))
        return false;
    tuneTurns++;
    return true;
}
float Game::ballMass() const {
    return 1 / bodies[0]->getInvMass();
}
float Game::ballDensity() const {
    return ballMass() / watercraft::sphereVolume(watercraft::ballRadius, 2 * watercraft::ballRadius);
}
void Game::setBallFloating(bool floating) {
    if (boatBody < 0 || ballFloating == floating)
        return;
    ballFloating = floating;
    auto player = bodies[0];
    const float mass = watercraft::ballMass(floating);
    btVector3 inertia;
    player->getCollisionShape()->calculateLocalInertia(mass, inertia);
    player->setMassProps(mass, inertia);
    player->updateInertiaTensor();
    // Apply at rest or in motion without teleporting/resetting velocities. The
    // next fixed step recomputes buoyancy and controls using the new mass.
    player->clearForces();
    player->activate(true);
    bodies[boatBody]->activate(true);
}
bool Game::nearBoat() const {
    return boatBody >= 0 &&
           (bodies[boatBody]->getWorldTransform().getOrigin() - bodies[0]->getWorldTransform().getOrigin())
                   .length() < watercraft::boardingDistance;
}
bool Game::toggleBoat() {
    if (boatBody < 0 || (!piloting && !nearBoat()))
        return false;
    auto player = bodies[0];
    held = -1;
    piloting = !piloting;
    player->setCollisionFlags(piloting
                                  ? player->getCollisionFlags() | btCollisionObject::CF_NO_CONTACT_RESPONSE
                                  : player->getCollisionFlags() & ~btCollisionObject::CF_NO_CONTACT_RESPONSE);
    if (!piloting) {
        auto p = bodies[boatBody]->getWorldTransform() * btVector3(2.1f, 1.f, 0);
        if (level().roomBounds) {
            const auto &room = *level().roomBounds;
            p.setX(std::clamp(p.x(), room.center.x - room.half.x + .8f,
                             room.center.x + room.half.x - .8f));
            p.setZ(std::clamp(p.z(), room.center.z - room.half.z + .8f,
                             room.center.z + room.half.z - .8f));
        }
        place(0, f(p));
    }
    clearInput();
    resetHistory = true;
    return true;
}
std::vector<XMFLOAT4> Game::waterQueries() const {
    return waterQueries(0);
}
std::vector<XMFLOAT4> Game::waterQueries(float solidClearance) const {
    std::vector<XMFLOAT4> queries;
    for (const auto &sample : floatPoints) {
        auto q=sample.query;
        if(sample.body==boatBody) {
            // A surface kernel has no particle support inside the hull. Probe
            // beyond that exclusion band; forces still act at hull quadrature
            // points, not at the displaced probe locations.
            float radius=watercraft::halfBeam(q.z,watercraft::deck)+std::max(.3f,solidClearance);
            q.x=q.x<0?-radius:radius;
        }
        auto p = bodies[sample.body]->getWorldTransform() * v(q);
        queries.push_back({p.x(), p.y(), p.z(), 0});
    }
    return queries;
}
void Game::receiveWater(const std::vector<XMFLOAT4> &samples, float density, float gravity) {
    if (samples.size() != floatPoints.size())
        return;
    waterSamples = samples;
    waterDensity = density;
    waterGravity = gravity;
    waterAge = 0;
    if (boatBody >= 0)
        world->setGravity({0, -gravity, 0});
}
bool Game::waterForces(float dt) {
    if (boatBody < 0)
        return false;
    waterAge += dt;
    submergedBoat = 0;
    float playerLift = 0, playerWaterForceY = 0;
    if (waterAge < .15f && waterSamples.size() == floatPoints.size()) {
        for (size_t i = 0; i < floatPoints.size(); ++i) {
            const auto &point = floatPoints[i];
            if (piloting && point.body == 0)
                continue;
            auto b = bodies[point.body];
            const auto &sample = waterSamples[i];
            auto offset = b->getWorldTransform().getBasis() * v(point.local);
            auto p = b->getWorldTransform().getOrigin() + offset;
            float fraction = watercraft::immersedFraction(sample.x, p.y(), point.halfHeight);
            if (point.body == boatBody) {
                float upright=std::max(.2f, float(std::abs(b->getWorldTransform().getBasis()[1][1])));
                fraction=watercraft::columnVolume(point.local.z,(sample.x-p.y())/upright)/point.volume;
            }
            if (!point.body) {
                // Exact spherical-cap volume in locally planar water.
                fraction = watercraft::sphereVolume(.68f, sample.x - p.y() + .68f) /
                           watercraft::sphereVolume(.68f, 1.36f);
                offset = {0, 0, 0};
            }
            const float volume = point.volume * fraction;
            if (!point.body)
                playerLift += waterDensity * waterGravity * volume;
            if (point.body == boatBody)
                submergedBoat += volume;
            if (volume <= 0)
                continue;
            auto relative = b->getVelocityInLocalPoint(offset) - btVector3(sample.y, sample.z, sample.w);
            const float area = std::pow(point.volume, 2.f / 3) * fraction;
            auto drag = -relative * (.5f * waterDensity * .7f * area * relative.length());
            if (point.body == boatBody) {
                auto basis=b->getWorldTransform().getBasis();
                auto local=basis.transpose()*relative;
                drag=basis*btVector3(-local.x()*std::abs(local.x())*1.1f,
                    -local.y()*(std::abs(local.y())*1.3f+1.5f),
                    -local.z()*std::abs(local.z())*.12f)*(.5f*waterDensity*area);
            }
            // Dissipative bound: an explicit drag impulse may not reverse the
            // relative velocity within one fixed step, including large impacts.
            // Effective mass at the force point includes angular response. A
            // centre-of-mass-only cap can inject rotational energy at long levers.
            auto axis = relative.length2() > 1e-12f ? relative.normalized() : btVector3(0, 1, 0);
            auto lever = offset.cross(axis);
            float inverseMass = b->getInvMass() + lever.dot(b->getInvInertiaTensorWorld() * lever);
            float cap = 1 / (inverseMass * dt * (point.body == boatBody ? 2*watercraft::stations : 4));
            if (drag.length() > relative.length() * cap)
                drag = -relative * cap;
            if (!point.body)
                playerWaterForceY += waterDensity * waterGravity * volume + drag.y();
            b->activate(true);
            b->applyForce(btVector3(0, waterDensity * waterGravity * volume, 0) + drag, offset);
        }
    }
    const int verticalInput = int(input.ascend) - int(input.dive);
    if (!piloting && verticalInput && playerLift > 0) {
        auto player = bodies[0];
        float mass = 1 / player->getInvMass();
        // Swimming uses sustained thrust, including for heavy solid glass.
        // The controller overcomes weight/drag while wet; it never changes
        // density or teleports the ball. Opposing controls cancel propulsion.
        player->applyCentralForce(
            {0,
             -playerWaterForceY + mass * waterGravity + mass * (verticalInput * 1.5f - player->getLinearVelocity().y()) * 8,
             0});
    }
    if (piloting) {
        auto boat = bodies[boatBody], player = bodies[0];
        auto t = player->getWorldTransform();
        // Stay above the craft even if it capsizes, instead of teleporting the
        // camera through a room wall with a rotated one-metre seat offset.
        t.setOrigin(boat->getWorldTransform().getOrigin() + btVector3(0, 1.f, 0));
        player->setWorldTransform(t);
        player->setLinearVelocity(boat->getLinearVelocity());
        player->setAngularVelocity({0, 0, 0});
        player->clearForces();
        player->applyCentralForce({0, waterGravity / player->getInvMass(), 0});
        world->updateSingleAabb(player);
        boat->applyCentralForce({0, -ballMass() * waterGravity, 0});
        if (submergedBoat > .01f) {
            auto basis = boat->getWorldTransform().getBasis();
            auto direction = basis * btVector3(0, 0, -1);
            float throttle = float(input.forward - input.back), steering = float(input.right - input.left);
            // Propeller thrust plus rudder moment; never teleports the hull or
            // sets its velocity. MAC solid velocities produce the moving wake.
            boat->applyCentralForce(direction * (throttle * watercraft::thrust * (throttle < 0 ? .45f : 1.f)));
            boat->applyTorque({0, -steering * (450 + std::abs(throttle) * 1800), 0});
            boat->activate(true);
        }
    }
    return !piloting && playerLift > 0;
}
void Game::step(float delta) {
    const float blendTarget = tuning >= 0 ? 1.f : 0.f;
    tuneBlend += (blendTarget - tuneBlend) * (1 - std::exp(-std::clamp(delta, 0.f, .1f) * 12));
    if (std::abs(tuneBlend - blendTarget) < .001f)
        tuneBlend = blendTarget;
    if (paused || won)
        return;
    delta = std::clamp(delta, 0.f, .1f);
    elapsed += delta;
    accumulator += delta;
    while (accumulator >= 1.f / 120) {
        constexpr float dt = 1.f / 120;
        accumulator -= dt;
        jumpCooldown = std::max(0.f, jumpCooldown - dt);
        const bool ground = grounded();
        auto player = bodies[0];
        auto velocity = player->getLinearVelocity();
        if (tuning >= 0) {
            player->setLinearVelocity({0, 0, 0});
            player->setAngularVelocity({0, 0, 0});
            velocity = {0, 0, 0};
            input.forward = input.back = input.left = input.right = input.jump = false;
            input.ascend = input.dive = false;
        }
        btVector3 forward(-std::sin(azimuth), 0, -std::cos(azimuth)),
            right(std::cos(azimuth), 0, -std::sin(azimuth));
        btVector3 move =
            forward * float(input.forward - input.back) + right * float(input.right - input.left);
        if (move.length2() > 1)
            move.normalize();
        player->clearForces();
        const float mass = 1 / player->getInvMass();
        if (move.length2() > 0) {
            player->activate(true);
            auto force =
                (move * 5 - btVector3(velocity.x(), 0, velocity.z())) * mass * (ground ? 10.f : 2.2f);
            player->applyCentralForce(force);
            if (ground)
                player->applyTorque({move.z() * mass * 8, 0, -move.x() * mass * 8});
        } else if (ground) {
            player->applyCentralForce({-velocity.x() * mass * 3.5f, 0, -velocity.z() * mass * 3.5f});
            player->applyTorque(-player->getAngularVelocity() * mass * .5f);
        }
        if (held >= 0) {
            auto b = bodies[held];
            auto p = player->getWorldTransform().getOrigin(), q = b->getWorldTransform().getOrigin();
            auto target = p + forward * 2.2f;
            const auto &prop = level().props[held - 1];
            target.setY(prop.upright ? prop.half.y + .12f : p.y() + 1.05f);
            auto force =
                ((target - q) * 45 + (velocity - b->getLinearVelocity()) * 11 + btVector3(0, 16, 0)) /
                b->getInvMass();
            const float limit = boatBody >= 0 ? 150.f / b->getInvMass() : 150.f;
            for (int i = 0; i < 3; i++)
                force[i] = std::clamp(force[i], -limit, limit);
            b->clearForces();
            b->applyCentralForce(force);
            b->activate(true);
        }
        if (input.turnLeft || input.turnRight)
            rotate(float(input.turnRight - input.turnLeft) * dt * (input.fine ? .15f : .8f));
        if (gateOpen && gateLift < 3.3f) {
            gateLift = std::min(3.3f, gateLift + dt * 1.7f);
            auto b = boatBody == int(bodies.size()) - 1 ? bodies[bodies.size() - 2] : bodies.back();
            auto t = b->getWorldTransform();
            const size_t gate = boatBody == int(bodies.size()) - 1 ? bodies.size() - 3 : bodies.size() - 2;
            const auto &origin = level().props[gate].position;
            t.setOrigin({origin.x, origin.y + gateLift, origin.z});
            b->setWorldTransform(t);
            world->updateSingleAabb(b);
        }
        const bool swimming = waterForces(dt);
        if (input.jump) {
            if (!swimming && !piloting && ground && jumpCooldown <= 0) {
                player->activate(true);
                player->applyCentralImpulse({0, mass * 7.2f, 0});
                jumpCooldown = .2f;
                jumps++;
            }
            input.jump = false;
        }
        previousStep.clear();
        for (auto b : bodies)
            previousStep.push_back(b->getWorldTransform());
        world->stepSimulation(dt, 0);
        if (playerPosition().y < -8) {
            load(chapter);
            return;
        }
        for (size_t i = 1; i < bodies.size(); i++)
            if (bodies[i]->getWorldTransform().getOrigin().y() < -8)
                place(int(i), level().props[i - 1].position);
    }
}
void Game::receive(const LaserResult &laser, float delta) {
    power = std::isfinite(laser.stats.y) ? std::max(0.f, laser.stats.y) : 0;
    for (size_t i = 0; i < level().sensors.size(); i++)
        sensorPower[i] = std::isfinite(laser.sensors[i].x) ? std::max(0.f, laser.sensors[i].x) : 0;
    if (paused || won)
        return;
    if (!gateOpen) {
        charge = std::clamp(charge + (power >= .35f ? delta / 2 : -delta * 1.5f), 0.f, 1.f);
        if (charge >= 1 - 1e-6f) {
            gateOpen = true;
            toast = "Light restored. Roll through the illuminated portal.";
        }
    }
    auto p = playerPosition();
    const size_t gate = boatBody == int(bodies.size()) - 1 ? bodies.size() - 3 : bodies.size() - 2;
    const auto &exit = level().props[gate];
    if (gateOpen && gateLift > 2.2f && p.z < exit.position.z - .25f &&
        std::abs(p.x - exit.position.x) < exit.half.x) {
        completed = std::max(completed, chapter + 1);
        if (chapter + 1 == int(levels.size())) {
            won = true;
            paused = true;
            clearInput();
        } else
            load(chapter + 1);
    }
}
void Game::place(int index, XMFLOAT3 p, float yaw) {
    auto b = bodies.at(index);
    if (poseDiscontinuities.size() != bodies.size())
        poseDiscontinuities.assign(bodies.size(), 0);
    poseDiscontinuities[size_t(index)] = 1;
    btTransform t;
    t.setIdentity();
    t.setOrigin(v(p));
    t.setRotation(btQuaternion({0, 1, 0}, yaw));
    b->setWorldTransform(t);
    b->setLinearVelocity({0, 0, 0});
    b->setAngularVelocity({0, 0, 0});
    b->clearForces();
    b->activate(true);
    world->updateSingleAabb(b);
    previousStep.clear();
    resetHistory = true;
}
std::vector<XMFLOAT4X4> Game::poses() const {
    std::vector<XMFLOAT4X4> out(bodies.size() + 1);
    XMStoreFloat4x4(&out[0], XMMatrixIdentity());
    for (size_t i = 0; i < bodies.size(); i++) {
        const auto t = bodies[i]->getWorldTransform();
        auto q = t.getRotation();
        auto p = t.getOrigin();
        if (!paused && previousStep.size() == bodies.size()) {
            float alpha = std::clamp(accumulator * 120, 0.f, 1.f);
            p = previousStep[i].getOrigin().lerp(p, alpha);
            q = previousStep[i].getRotation().slerp(q, alpha).normalized();
        }
        XMStoreFloat4x4(&out[i + 1], XMMatrixRotationQuaternion(XMVectorSet(q.x(), q.y(), q.z(), q.w())) *
                                         XMMatrixTranslation(p.x(), p.y(), p.z()));
    }
    return out;
}
Camera Game::camera(float aspect, CameraFollow *follow, float delta) const {
    const auto pose = poses()[1];
    XMFLOAT3 target{pose._41, pose._42 + .48f, pose._43};
    btVector3 direction(std::cos(elevation) * std::sin(azimuth), std::sin(elevation),
                        std::cos(elevation) * std::cos(azimuth));
    auto a = v(target);
    float actual = distance;
    if (follow) {
        // Sweep the camera's volume, not a point beyond its endpoint. The old
        // extra 30 cm ray/backoff made the arm pop at the top/edge of a wall.
        auto b = a + direction * distance;
        btSphereShape probe(.30f);
        btTransform from = btTransform::getIdentity(), to = from;
        from.setOrigin(a);
        to.setOrigin(b);
        CameraSweep sweep(a, b, bodies[0]);
        world->convexSweepTest(&probe, from, to, sweep);
        if (sweep.hasHit())
            actual = std::max(.05f, distance * sweep.m_closestHitFraction - .01f);
        // Pull in immediately for collision safety, ease out as clearance grows.
        // Smooth only boom length: avatar interpolation and orbit angles stay exact.
        if (follow->distance < 0 || actual <= follow->distance)
            follow->distance = actual;
        else {
            float dt = paused || won ? 0 : std::clamp(delta, 0.f, .05f);
            float expansion = (actual - follow->distance) * -std::expm1(-dt / .10f);
            follow->distance += std::min(expansion, 8.f * dt);
        }
        actual = follow->distance;
    } else {
        auto b = a + direction * (distance + .3f);
        RayIgnore ray(a, b, bodies[0], true);
        world->rayTest(a, b, ray);
        actual = ray.hasHit()
                     ? std::max(.18f, std::min(distance, ray.m_closestHitFraction * (distance + .3f) - .3f))
                     : distance;
    }
    Camera c{};
    c.position = f(a + direction * actual);
    if (firstPerson && tuning < 0 && tuneBlend < .001f) {
        // The primary camera ray omits the avatar only; other transport still
        // sees the glass ball. No water-height clamp: the lens can stay immersed.
        c.position = {pose._41, pose._42 + .12f, pose._43};
        target = f(v(c.position) - direction);
    }
    if (tuneBlend > 0) {
        // A near-vertical view retains a stable north-up basis, including at blend == 1.
        auto overhead = v(tuneFocus) + btVector3(0, 12.f, .025f);
        c.position = f(v(c.position).lerp(overhead, tuneBlend));
        target = f(v(target).lerp(v(tuneFocus), tuneBlend));
    }
    if (boatBody >= 0 && tuning < 0 && level().roomBounds) {
        const auto &room = *level().roomBounds;
        c.position.x = std::clamp(c.position.x, room.center.x - room.half.x + .35f,
                                 room.center.x + room.half.x - .35f);
        c.position.y = std::clamp(c.position.y, room.center.y - room.half.y + .30f,
                                 room.center.y + room.half.y - .35f);
        c.position.z = std::clamp(c.position.z, room.center.z - room.half.z + .35f,
                                 room.center.z + room.half.z - .35f);
    }
    auto eye = XMLoadFloat3(&c.position), focus = XMLoadFloat3(&target);
    auto forward = XMVector3Normalize(XMVectorSubtract(focus, eye)),
         right = XMVector3Normalize(XMVector3Cross(forward, XMVectorSet(0, 1, 0, 0))),
         up = XMVector3Cross(right, forward);
    XMStoreFloat3(&c.forward, forward);
    XMStoreFloat3(&c.right, right);
    XMStoreFloat3(&c.up, up);
    auto view = XMMatrixLookAtRH(eye, focus, up),
         projection = XMMatrixPerspectiveFovRH(XM_PI / 3, aspect, .05f, 200.f);
    XMStoreFloat4x4(&c.view, view);
    XMStoreFloat4x4(&c.projection, projection);
    XMStoreFloat4x4(&c.viewProjection, view * projection);
    return c;
}
void runGameTests(const std::filesystem::path &assets) {
    Game g(loadLevels(assets / "chambers.bin"));
    for (int i = 0; i < 360; i++)
        g.step(1.f / 120);
    check(g.grounded(), "Player must settle on floor");
    check(std::abs(g.playerPosition().y - .68f) < .03f, "Sphere collider radius mismatch");
    auto start = g.playerPosition();
    g.input.right = true;
    for (int i = 0; i < 120; i++)
        g.step(1.f / 120);
    check(std::hypot(g.playerPosition().x - start.x, g.playerPosition().z - start.z) > 1,
          "WASD movement failed");
    g.clearInput();
    g.input.jump = true;
    g.step(1.f / 120);
    check(g.jumps == 1, "Grounded jump failed");
    g.input.jump = true;
    g.step(1.f / 120);
    check(g.jumps == 1, "Air jumping must be rejected");
    g.load(1);
    g.place(0, {0, .7f, 3});
    g.place(1, {0, 1.15f, 0});
    check(g.dock(), "Mirror docking failed");
    check(g.rotate(XM_PI / 4), "Mirror rotation failed");
    check(g.beginTuning() && g.tuning == 1, "Seated mirror must enter precision mode");
    auto tunePosition = g.opticPosition();
    auto tuneYaw = g.opticYaw();
    check(g.tuneMove(.05f, 0), "Fine positioning must allow a clear move");
    check(g.tuneRotate(.001f), "Fine wheel rotation must work");
    check(!g.tuneMove(10, 0), "Cradle travel bounds must reject distant placement");
    g.input.right = true;
    auto ball = g.playerPosition();
    for (int i = 0; i < 120; i++)
        g.step(1.f / 120);
    check(std::abs(g.playerPosition().x - ball.x) < .02f, "Precision mode must suppress player movement");
    check(g.camera(16.f / 9).forward.y < -.99f, "Precision camera must settle overhead");
    g.place(0, {.6f, .7f, 0});
    check(!g.tuneMove(.1f, 0), "Fine positioning must reject overlap with the player");
    g.place(0, ball);
    g.endTuning(true);
    check(g.tuning < 0 && std::abs(g.opticPosition().x - tunePosition.x) < 1e-5f &&
              std::abs(g.opticYaw() - tuneYaw) < 1e-5f,
          "Cancel must restore optic pose and leave it locked");
    g.azimuth = 0;
    g.grab();
    check(g.held == 1 && !g.docked, "Docked mirror must be grabbable");
    g.throwHeld();
    check(g.held == -1 && g.throws == 1, "Physics throw failed");
    // Both native prisms and both true-volume lens colliders must independently
    // support the ordinary nearest-optic controls, not just scripted poses.
    for (int chapter : {4, 5}) {
        g.load(chapter);
        for (int body : {1, 2}) {
            const auto p = g.level().props[body - 1];
            g.place(body, {p.dockYaw.x, p.dockYaw.y, p.dockYaw.z});
            g.place(0, {p.dockYaw.x + 2.3f, .7f, p.dockYaw.z});
            g.azimuth = XM_PIDIV2;
            check(g.nearbyOptic() == body, "Nearest multi-optic selection failed");
            check(g.dock(), "Independent multi-optic docking failed");
            check(g.rotate(.05f), "Independent multi-optic rotation failed");
            check(g.beginTuning() && g.tuning == body, "Each prism/lens must enter its own workbench");
            check(g.tuneMove(.015f, 0) && g.tuneRotate(.001f), "Each prism/lens must support precise edits");
            g.grab();
            check(g.held == body && g.tuning < 0, "E must leave workbench and pick up the selected optic");
            g.throwHeld();
            g.place(body, {6, p.half.y, 5});
        }
    }
    g.load(int(g.levels.size()) - 1);
    LaserResult laser{};
    laser.stats.y = 1;
    for (int i = 0; i < 19; i++)
        g.receive(laser, .1f);
    check(!g.gateOpen, "Gate opened before sustained power");
    g.receive(laser, .1f);
    check(g.gateOpen, "Gate failed to open after sustained power");
    for (int i = 0; i < 240; i++)
        g.step(1.f / 120);
    check(g.gateLift > 2.2f, "Gate collider did not lift");
    g.place(0, {0, .7f, -6.65f});
    g.receive(laser, .01f);
    check(g.won && g.completed == int(g.levels.size()), "Victory transition failed");
    g.load(0);
    g.paused = false;
    laser.stats.y = 0;
    g.receive(laser, .1f);
    check(g.charge == 0, "Blocked beam must not charge");
    g.paused = true;
    laser.stats.y = 1;
    g.receive(laser, .1f);
    check(g.charge == 0, "Paused locks must not charge");
    std::cout << "PASS: shared assets, rigid bodies, movement, grounded jump, docking, rotation, throwing, "
                 "timed locks, gates and victory.\n";
}
