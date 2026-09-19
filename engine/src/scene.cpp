#include "scene.h"
#include "water.h"
#include "large_water.h"
#include "ocean.h"
#include "../../shared/src/watercraft.h"
#include <algorithm>
#include <cmath>
namespace lab {
namespace {
XMFLOAT3 sphere(float u, float v) {
    return {.68f * std::sin(v) * std::cos(u), .68f * std::cos(v),
            .68f * std::sin(v) * std::sin(u)};
}
}
bool transportMoved(const XMFLOAT4X4 &now, const XMFLOAT4X4 &anchor, float radius) {
    float translation2 = 0, rotation2 = 0;
    for (int j = 0; j < 3; ++j) {
        const float t = now.m[3][j] - anchor.m[3][j];
        translation2 += t * t;
        for (int i = 0; i < 3; ++i) {
            const float r = now.m[i][j] - anchor.m[i][j];
            rotation2 += r * r;
        }
    }
    // 0.1 mm, much smaller than a receiver texel. Compare with the LAST RESET
    // pose, not the previous frame, so slow deliberate adjustments accumulate.
    return std::sqrt(translation2) + radius * std::sqrt(rotation2) > .0001f;
}
std::vector<Mesh> makeScene() {
    std::vector<Mesh> meshes(2);
    meshes[0].mask = 1;
    meshes[1].mask = 2;
    auto triangle = [](Mesh &m, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, uint32_t material, uint32_t chart,
                       XMFLOAT2 ua, XMFLOAT2 ub, XMFLOAT2 uc) {
        m.vertices.push_back({a, material, ua, chart, 0});
        m.vertices.push_back({b, material, ub, chart, 0});
        m.vertices.push_back({c, material, uc, chart, 0});
    };
    auto quad = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d, uint32_t chart) {
        triangle(meshes[0], a, b, c, 0, chart, {0, 0}, {1, 0}, {1, 1});
        triangle(meshes[0], a, c, d, 0, chart, {0, 0}, {1, 1}, {0, 1});
    };
    quad({-6, 0, -6}, {-6, 0, 8}, {6, 0, 8}, {6, 0, -6}, 0);
    quad({-6, 0, 8}, {-6, 6, 8}, {6, 6, 8}, {6, 0, 8}, 1);
    quad({-6, 0, -6}, {-6, 6, -6}, {-6, 6, 8}, {-6, 0, 8}, 2);
    quad({6, 0, 8}, {6, 6, 8}, {6, 6, -6}, {6, 0, -6}, 3);
    // Watertight equilateral triangular prism, not a box with a prism shader.
    const XMFLOAT3 p[3] = {{-.85f, 0, -.4907477f}, {.85f, 0, -.4907477f}, {0, 0, .9814954f}};
    const auto at = [&](int i, float y) { return XMFLOAT3{p[i].x, y, p[i].z}; };
    triangle(meshes[1], at(0, 1), at(1, 1), at(2, 1), 1, ~0u, {}, {}, {});
    triangle(meshes[1], at(2, -1), at(1, -1), at(0, -1), 1, ~0u, {}, {}, {});
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        triangle(meshes[1], at(i, -1), at(j, -1), at(j, 1), 1, ~0u, {}, {}, {});
        triangle(meshes[1], at(i, -1), at(j, 1), at(i, 1), 1, ~0u, {}, {}, {});
    }
    // Orient every closed-prism triangle outward, independently of face winding above.
    auto &v = meshes[1].vertices;
    for (size_t i = 0; i < v.size(); i += 3) {
        XMVECTOR a = XMLoadFloat3(&v[i].position), b = XMLoadFloat3(&v[i + 1].position),
                 c = XMLoadFloat3(&v[i + 2].position);
        if (XMVectorGetX(XMVector3Dot(XMVector3Cross(b - a, c - a), a + b + c)) < 0)
            std::swap(v[i + 1], v[i + 2]);
    }
    return meshes;
}
XMFLOAT4X4 prismTransform(float angle) {
    XMFLOAT4X4 result;
    XMStoreFloat4x4(&result, XMMatrixRotationY(angle) * XMMatrixTranslation(0, 1.65f, 0));
    return result;
}
Mesh makePlayerMesh() {
    Mesh mesh; mesh.mask = 4;
    auto tri = [](Mesh &m, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, uint32_t material) {
        for (auto p : {a, b, c}) m.vertices.push_back({p, material, {}, ~0u, 0});
    };
    // Glass rolling avatar. Closed sphere, smooth shading normals reconstructed in raygen.
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            float u = XM_2PI * x / 64, v = XM_PI * y / 32, un = XM_2PI * (x + 1) / 64,
                  vn = XM_PI * (y + 1) / 32;
            if (y > 0)
                tri(mesh, sphere(u, v), sphere(un, v), sphere(un, vn), 2);
            if (y < 31)
                tri(mesh, sphere(u, v), sphere(un, vn), sphere(u, vn), 2);
        }
    // Enforce outward normals (important for entering/exiting dielectric medium tracking).
    auto &s = mesh.vertices;
    for (size_t i = 0; i < s.size(); i += 3) {
        auto a = XMLoadFloat3(&s[i].position), b = XMLoadFloat3(&s[i + 1].position),
             c = XMLoadFloat3(&s[i + 2].position);
        if (XMVectorGetX(XMVector3Dot(XMVector3Cross(b - a, c - a), a + b + c)) < 0)
            std::swap(s[i + 1], s[i + 2]);
    }
    return mesh;
}
Mesh makeNeonBlockMesh(uint32_t material) {
    Mesh mesh; mesh.mask = 1;
    XMFLOAT3 v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = {i & 1 ? .38f : -.38f, i & 2 ? .38f : -.38f, i & 4 ? .38f : -.38f};
    const int faces[6][4] = {{0,4,6,2},{1,3,7,5},{0,1,5,4},{2,6,7,3},{0,2,3,1},{4,5,7,6}};
    for (const auto &f : faces)
        for (int index : {f[0],f[1],f[2],f[0],f[2],f[3]})
            mesh.vertices.push_back({v[index], material, {}, ~0u, 0});
    return mesh;
}
std::vector<Mesh> makePlayScene(bool water, bool flat, bool glassPit, bool fluidRoom, bool boat,
                                bool deepPool, bool largeWaterLab, bool oceanLab) {
    auto fixture = makeScene();
    std::vector<Mesh> meshes(6);
    // Room construction holds references to meshes[0]; appending the optional
    // craft must not relocate that storage before the room fixtures are finished.
    if (fluidRoom && boat)
        meshes.reserve(7);
    meshes[0] = std::move(fixture[0]);
    if (largeWaterLab)
        for (auto &v : meshes[0].vertices) {
            v.position.x *= 2;
            v.position.z *= 2;
            if (v.position.y > 0)
                v.position.y += largeWater::lift;
        }
    meshes[2] = std::move(fixture[1]);
    for (int i : {1, 3, 4, 5})
        meshes[i].mask = i == 1 ? 4 : 1;
    auto tri = [](Mesh &m, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, uint32_t material) {
        for (auto p : {a, b, c})
            m.vertices.push_back({p, material, {}, ~0u, 0});
    };
    auto box = [&](Mesh &m, XMFLOAT3 p, XMFLOAT3 h, uint32_t material) {
        if (largeWaterLab && &m == &meshes[0]) {
            // Extend floor-mounted fixtures down to the same physical floor.
            if (std::abs(p.y - h.y) < .001f) {
                p.y += largeWater::lift * .5f;
                h.y += largeWater::lift * .5f;
            } else
                p.y += largeWater::lift;
        }
        XMFLOAT3 v[8];
        for (int i = 0; i < 8; ++i)
            v[i] = {p.x + (i & 1 ? h.x : -h.x), p.y + (i & 2 ? h.y : -h.y), p.z + (i & 4 ? h.z : -h.z)};
        const int faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                 {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
        for (auto &f : faces) {
            tri(m, v[f[0]], v[f[1]], v[f[2]], material);
            tri(m, v[f[0]], v[f[2]], v[f[3]], material);
        }
    };
    meshes[1] = makePlayerMesh();
    meshes[3] = makeNeonBlockMesh(4);
    meshes[4] = makeNeonBlockMesh(5);
    box(meshes[5], {0, 0, 0}, {1.48f, 1.4f, .16f}, 3);
    for (float x : {-1.35f, 1.35f})
        box(meshes[5], {x, 0, -.17f}, {.025f, 1.35f, .025f}, 5);
    auto &room = meshes[0];
    // Portal and containment match the existing physics/game's exit convention.
    box(room, {largeWaterLab ? -6.8f : -3.8f, 1.4f, largeWaterLab ? -12.2f : -6.2f},
        {largeWaterLab ? 5.2f : 2.2f, 1.4f, .16f}, 3);
    box(room, {largeWaterLab ? 6.8f : 3.8f, 1.4f, largeWaterLab ? -12.2f : -6.2f},
        {largeWaterLab ? 5.2f : 2.2f, 1.4f, .16f}, 3);
    box(room, {0, -.2f, largeWaterLab ? -12.65f : -6.65f}, {1.6f, .2f, .8f}, 3);
    // Optical cradle: elevated prism rests on a real collider, not in midair.
    box(room, {0, .29f, 0}, {1.05f, .29f, 1.05f}, 7);
    for (float x : {-1.03f, 1.03f})
        box(room, {x, .61f, 0}, {.018f, .015f, 1.05f}, 5);
    for (float z : {-1.03f, 1.03f})
        box(room, {0, .61f, z}, {1.05f, .015f, .018f}, 5);
    // Neon architecture reflected in glass; receiver charts remain unmodified.
    for (float wallX : {-5.96f, 5.96f}) {
        const float x = wallX + (largeWaterLab ? (wallX < 0 ? -6.f : 6.f) : 0.f);
        box(room, {x, .10f, largeWaterLab ? 2.f : 1.f}, {.015f, .035f, largeWaterLab ? 13.8f : 6.8f}, x < 0 ? 4 : 5);
        for (float z : {-4.f, 0.f, 4.f, 7.7f}) {
            z *= largeWaterLab ? 2.f : 1.f;
            box(room, {x, 2.6f, z}, {.025f, 1.8f, .028f}, x < 0 ? 4 : 5);
            box(room, {x, 4.45f, z}, {.025f, .025f, .35f}, 6);
        }
    }
    for (float x : {-4.f, -2.f, 0.f, 2.f, 4.f})
        box(room, {x * (largeWaterLab ? 2.f : 1.f), 4.8f, largeWaterLab ? 15.97f : 7.97f}, {.7f, .018f, .018f}, 5);
    // White collimator fixture: its aperture is unobstructed on the emission side.
    box(room, {0, 1.2f, -5.25f}, {.30f, 1.2f, .25f}, 3);
    box(room, {0, 2, -5.035f}, {.22f, .18f, .015f}, 6);
    if (fluidRoom) {
        if (boat) {
            Mesh craft;
            craft.mask = 1;
            auto hull=watercraft::hullTriangles();
            for (size_t i=0;i<hull.size();i+=3)
                tri(craft,hull[i],hull[i+1],hull[i+2],
                    hull[i].y==watercraft::deck && hull[i+1].y==watercraft::deck && hull[i+2].y==watercraft::deck ? 21 : 15);
            // Low gunwales follow the pointed bow, with a flat working deck,
            // upholstered seats, console and a stern-mounted outboard.
            auto outline=watercraft::hullVertices();
            for (unsigned i=0;i<5;++i) {
                auto a=outline[5+i],b=outline[5+(i+1)%5];
                XMFLOAT3 ai{a.x*.94f,a.y,a.z*.97f},bi{b.x*.94f,b.y,b.z*.97f};
                XMFLOAT3 au{a.x,.55f,a.z},bu{b.x,.55f,b.z},aiu{ai.x,.55f,ai.z},biu{bi.x,.55f,bi.z};
                tri(craft,a,b,bu,15);tri(craft,a,bu,au,15);
                tri(craft,ai,biu,bi,15);tri(craft,ai,aiu,biu,15);
                tri(craft,au,bu,biu,15);tri(craft,au,biu,aiu,15);
            }
            box(craft,{0,.42f,1.45f},{.9f,.12f,.3f},16);
            box(craft,{0,.62f,1.7f},{.9f,.22f,.07f},16);
            box(craft,{0,.60f,-.45f},{.45f,.30f,.28f},15);
            box(craft,{0,.95f,-.50f},{.52f,.06f,.34f},17);
            box(craft,{0,.80f,-.72f},{.50f,.20f,.035f},17);
            box(craft,{0,.35f,2.90f},{.28f,.48f,.26f},17);
            box(craft,{0,-.37f,2.91f},{.07f,.30f,.09f},17);
            meshes.push_back(std::move(craft));
        }
        // The whole playable footprint is now the basin. Remove the old pit
        // from BOTH rendering and physics; keep its overhead caustic illuminator.
        box(room, {3.70f, 4.65f, -3.10f}, {1.45f, .06f, 1.35f}, 3);
        box(room, {3.70f, 4.58f, -3.10f}, {1.35f, .008f, 1.25f}, 6);
        box(room, {2.40f, 4.03f, -3.90f}, {.13f, .10f, .13f}, 3);
        box(room, {2.40f, 3.92f, -3.90f}, {.035f, .008f, .035f}, 9);
        // Four rails form a hollow wall nozzle, never a solid cap over the inlet.
        for (float z : {2.14f, 2.66f})
            box(room, {largeWaterLab ? -11.80f : -5.80f, 1.85f, z}, {.18f, .26f, .035f}, 3);
        for (float y : {1.59f, 2.11f})
            box(room, {largeWaterLab ? -11.80f : -5.80f, y, 2.4f}, {.18f, .035f, .26f}, 3);
        box(room, {largeWaterLab ? -11.965f : -5.965f, .94f, 2.4f}, {.025f, .22f, .22f}, 3);
        box(room, {largeWaterLab ? -11.925f : -5.925f, .94f, 2.4f}, {.025f, .14f, .14f}, 14);
        if (deepPool) {
            room.vertices.clear();
            auto receiver = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d, uint32_t chart) {
                const XMFLOAT3 p[]{a, b, c, d};
                const XMFLOAT2 uv[]{{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                for (int i : {0, 1, 2, 0, 2, 3})
                    room.vertices.push_back({p[i], 0, uv[i], chart, 0});
            };
            receiver({-24, 0, -24}, {-24, 0, 32}, {24, 0, 32}, {24, 0, -24}, 0);
            receiver({-24, 0, 32}, {-24, 14, 32}, {24, 14, 32}, {24, 0, 32}, 1);
            receiver({-24, 0, -24}, {-24, 14, -24}, {-24, 14, 32}, {-24, 0, 32}, 2);
            receiver({24, 0, 32}, {24, 14, 32}, {24, 14, -24}, {24, 0, -24}, 3);
            receiver({24, 0, -24}, {24, 14, -24}, {-24, 14, -24}, {-24, 0, -24}, 4);
            for (float x : {-23.96f, 23.96f}) {
                box(room, {x, .10f, 4}, {.015f, .035f, 27.8f}, x < 0 ? 4 : 5);
                for (float z : {-20.f, -8.f, 4.f, 16.f, 28.f})
                    box(room, {x, 7.f, z}, {.025f, 6.8f, .028f}, x < 0 ? 4 : 5);
            }
            box(room, {0, 4.29f, 0}, {1.05f, 4.29f, 1.05f}, 7);
            box(room, {0, 5.2f, -20.25f}, {.30f, 5.2f, .25f}, 3);
            box(room, {0, 10, -20.035f}, {.22f, .18f, .015f}, 6);
            // Hollow nozzle above the water; only this small region is forced.
            for (float z : {9.20f, 10.f})
                box(room, {-23.65f, 9.5f, z}, {.25f, .40f, .035f}, 3);
            for (float y : {9.10f, 9.90f})
                box(room, {-23.65f, y, 9.6f}, {.25f, .035f, .40f}, 3);
            box(room, {-23.925f, 8.5f, 9.6f}, {.025f, .22f, .22f}, 14);
        }
        if (oceanLab) {
            room.vertices.clear();
            auto groundTriangle = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c) {
                for (auto p : {a, b, c})
                    room.vertices.push_back({p, 18, {(p.z + 128) / 256, (p.x + 128) / 256}, 0, 0});
            };
            groundTriangle({-128, 0, -128}, {-128, 0, 128}, {128, 0, 128});
            groundTriangle({-128, 0, -128}, {128, 0, 128}, {128, 0, -128});
            auto point = [](unsigned x, unsigned z) {
                float px = ocean::terrainMinX + x * ocean::terrainStep;
                float pz = ocean::terrainMinZ + z * ocean::terrainStep;
                return XMFLOAT3{px, ocean::oceanTerrainHeight(px, pz), pz};
            };
            for (unsigned z = 0; z + 1 < ocean::terrainZ; ++z)
                for (unsigned x = 0; x + 1 < ocean::terrainX; ++x) {
                    auto a = point(x, z), b = point(x, z + 1), c = point(x + 1, z + 1), d = point(x + 1, z);
                    if (std::max({a.y, b.y, c.y, d.y}) < .001f) continue;
                    groundTriangle(a, b, c);
                    groundTriangle(a, c, d);
                }
            // A few palms on dry ground. Trunks are real occluders, foliage is
            // tapered geometry, not screen-facing scenery.
            for (auto p : {XMFLOAT3{-25, 0, -19}, XMFLOAT3{-12, 0, -20}, XMFLOAT3{-30, 0, -7},
                           XMFLOAT3{-34, 0, -27}, XMFLOAT3{-17, 0, -29}}) {
                p.y = ocean::oceanTerrainHeight(p.x, p.z);
                box(room, {p.x, p.y + 2.4f, p.z}, {.17f, 2.4f, .17f}, 21);
                for (int leaf = 0; leaf < 9; ++leaf) {
                    float angle = leaf * XM_2PI / 9 + p.x;
                    XMFLOAT3 previous{p.x, p.y + 4.8f, p.z};
                    for (int segment = 1; segment <= 6; ++segment) {
                        float t = segment / 6.f, before = (segment - 1) / 6.f;
                        XMFLOAT3 next{p.x + std::cos(angle) * 3.7f * t,
                                      p.y + 4.8f + 1.2f * std::sin(t * XM_PI) - 1.3f * t * t,
                                      p.z + std::sin(angle) * 3.7f * t};
                        float w0 = .40f * std::sin(before * XM_PI), w1 = .40f * std::sin(t * XM_PI);
                        XMFLOAT3 a{previous.x - std::sin(angle) * w0, previous.y, previous.z + std::cos(angle) * w0};
                        XMFLOAT3 b{previous.x + std::sin(angle) * w0, previous.y, previous.z - std::cos(angle) * w0};
                        XMFLOAT3 c{next.x + std::sin(angle) * w1, next.y, next.z - std::cos(angle) * w1};
                        XMFLOAT3 d{next.x - std::sin(angle) * w1, next.y, next.z + std::cos(angle) * w1};
                        if (segment > 1) tri(room, a, b, c, 22);
                        if (segment < 6) tri(room, a, c, d, 22);
                        previous = next;
                    }
                }
            }
            // Distant optical ocean. Interactive waves occupy the inner 256 m;
            // the horizon geometry carries the same dielectric material.
            auto sea = [&](float x0, float z0, float x1, float z1) {
                XMFLOAT3 a{x0, ocean::surface, z0}, b{x0, ocean::surface, z1},
                         c{x1, ocean::surface, z1}, d{x1, ocean::surface, z0};
                tri(room, a, b, c, 8); tri(room, a, c, d, 8);
            };
            sea(-5000, -5000, -128, 5000); sea(128, -5000, 5000, 5000);
            sea(-128, -5000, 128, -128); sea(-128, 128, 128, 5000);
            // Continue the optical seabed too. Ending it at the simulation
            // boundary creates a rectangular jump from shallow to deep water.
            auto seabed = [&](float x0, float z0, float x1, float z1) {
                tri(room, {x0,0,z0}, {x0,0,z1}, {x1,0,z1}, 18);
                tri(room, {x0,0,z0}, {x1,0,z1}, {x1,0,z0}, 18);
            };
            seabed(-5000,-5000,-128,5000); seabed(128,-5000,5000,5000);
            seabed(-128,-5000,128,-128); seabed(-128,128,128,5000);
            for (const auto &part : ocean::pier) box(room, part.center, part.half, 21);
            for (unsigned i = 0; i < ocean::OCEAN_LANTERN_COUNT; ++i) {
                auto p = ocean::oceanLanternPosition(i);
                box(room, {p.x, p.y - .90f, p.z}, {.045f, .65f, .045f}, 17);
                box(room, {p.x, p.y - .20f, p.z}, {.20f, .05f, .20f}, 17);
                box(room, {p.x, p.y + .20f, p.z}, {.22f, .04f, .22f}, 17);
                auto globe = [&](float u, float v) {
                    auto q = sphere(u, v);
                    float scale = ocean::OCEAN_LANTERN_RADIUS / .68f;
                    return XMFLOAT3{p.x + q.x * scale, p.y + q.y * scale, p.z + q.z * scale};
                };
                size_t begin = room.vertices.size();
                for (int y = 0; y < 16; ++y) for (int x = 0; x < 32; ++x) {
                    float u = XM_2PI*x/32, un = XM_2PI*(x+1)/32, v = XM_PI*y/16, vn = XM_PI*(y+1)/16;
                    if (y > 0) tri(room, globe(u,v), globe(un,v), globe(un,vn), 24+i);
                    if (y < 15) tri(room, globe(u,v), globe(un,vn), globe(u,vn), 24+i);
                }
                auto &v = room.vertices;
                for (size_t j = begin; j < v.size(); j += 3) {
                    auto a = XMLoadFloat3(&v[j].position), b = XMLoadFloat3(&v[j+1].position),
                         c = XMLoadFloat3(&v[j+2].position), center = XMLoadFloat3(&p);
                    if (XMVectorGetX(XMVector3Dot(XMVector3Cross(b-a,c-a), a+b+c-center*3)) < 0)
                        std::swap(v[j+1], v[j+2]);
                }
            }
            for (int i : {2, 3, 4, 5}) {
                meshes[i].vertices.clear();
                box(meshes[i], {}, i == 2 ? XMFLOAT3{.85f, 1, .98f}
                                          : i == 5 ? XMFLOAT3{1.48f, 1.4f, .16f} : XMFLOAT3{.38f, .38f, .38f}, 21);
            }
        }
        return meshes;
    }
    {
        // Keep opaque receivers separate from dielectric geometry/masks. A single
        // watertight rectangular annulus avoids overlapping glass boxes/internal
        // faces at the corners (which would create fictitious optical interfaces).
        for (float x : {1.70f, 5.70f}) {
            if (!glassPit)
                box(room, {x, .6f, -3.10f}, {.10f, .6f, 1.90f}, 3);
            box(room, {x, 1.215f, -3.10f}, {.07f, .015f, 1.90f}, 5);
        }
        for (float z : {-5.f, -1.20f}) {
            if (!glassPit)
                box(room, {3.70f, .6f, z}, {1.90f, .6f, .10f}, 3);
            box(room, {3.70f, 1.215f, z}, {1.90f, .015f, .07f}, 5);
        }
        const XMFLOAT3 floor[] = {
            {1.8f, .02f, -4.9f}, {1.8f, .02f, -1.3f}, {5.6f, .02f, -1.3f}, {5.6f, .02f, -4.9f}};
        const XMFLOAT2 uv[] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
        for (int i : {0, 1, 2, 0, 2, 3})
            room.vertices.push_back({floor[i], 0, uv[i], 4, 0});
        box(room, {3.70f, 4.65f, -3.10f}, {1.45f, .06f, 1.35f}, 3);
        box(room, {3.70f, 4.58f, -3.10f}, {1.35f, .008f, 1.25f}, 6);
        box(room, {2.40f, 4.03f, -3.90f}, {.13f, .10f, .13f}, 3);
        box(room, {2.40f, 3.92f, -3.90f}, {.035f, .008f, .035f}, 9);
        if (glassPit) {
            Mesh shell;
            shell.mask = 2;
            const XMFLOAT3 outer[] = {{1.6f, 0, -5.1f}, {1.6f, 0, -1.1f}, {5.8f, 0, -1.1f}, {5.8f, 0, -5.1f}};
            const XMFLOAT3 inner[] = {{1.8f, 0, -4.9f}, {1.8f, 0, -1.3f}, {5.6f, 0, -1.3f}, {5.6f, 0, -4.9f}};
            auto top = [](XMFLOAT3 p) {
                p.y = 1.2f;
                return p;
            };
            auto quad = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, XMFLOAT3 d) {
                tri(shell, a, c, b, 10);
                tri(shell, a, d, c, 10);
            };
            for (int i = 0; i < 4; ++i) {
                int j = (i + 1) % 4;
                quad(outer[i], top(outer[i]), top(outer[j]), outer[j]);
                quad(inner[j], top(inner[j]), top(inner[i]), inner[i]);
                quad(top(outer[i]), top(inner[i]), top(inner[j]), top(outer[j]));
                quad(outer[j], inner[j], inner[i], outer[i]);
            }
            meshes.push_back(std::move(shell));
        }
        // Disabling water removes the optical medium, not its solid tank.
        if (!water)
            return meshes;
        Mesh volume;
        volume.mask = 8;
        constexpr int grid = 64;
        auto point = [&](int x, int z) {
            float px = 1.75f + 3.90f * x / grid, pz = -4.95f + 3.70f * z / grid;
            return XMFLOAT3{px, waterHeight(px, pz, flat), pz};
        };
        for (int z = 0; z < grid; ++z)
            for (int x = 0; x < grid; ++x) {
                auto a = point(x, z), b = point(x, z + 1), c = point(x + 1, z + 1), d = point(x + 1, z);
                tri(volume, a, b, c, 8);
                tri(volume, a, c, d, 8);
            }
        // Closed sides/bottom extend into the opaque tank; water can still
        // attenuate paths ending at the floor without a fictitious air exit.
        auto wall = [&](XMFLOAT3 a, XMFLOAT3 b) {
            XMFLOAT3 c{b.x, -.05f, b.z}, d{a.x, -.05f, a.z};
            tri(volume, a, b, c, 8);
            tri(volume, a, c, d, 8);
            tri(volume, d, c, {3.70f, -.05f, -3.10f}, 8);
        };
        for (int i = 0; i < grid; ++i) {
            wall(point(i, 0), point(i + 1, 0));
            wall(point(grid, i), point(grid, i + 1));
            wall(point(i + 1, grid), point(i, grid));
            wall(point(0, i + 1), point(0, i));
        }
        meshes.push_back(std::move(volume));
    }
    return meshes;
}
} // namespace lab
