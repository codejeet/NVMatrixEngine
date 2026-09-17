#include "scene.h"
#include "water.h"
#include "../../shared/src/watercraft.h"
#include <algorithm>
#include <cmath>
namespace lab {
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
std::vector<Mesh> makePlayScene(bool water, bool flat, bool glassPit, bool fluidRoom, bool boat,
                                bool deepPool) {
    auto fixture = makeScene();
    std::vector<Mesh> meshes(6);
    // Room construction holds references to meshes[0]; appending the optional
    // craft must not relocate that storage before the room fixtures are finished.
    if (fluidRoom && boat)
        meshes.reserve(7);
    meshes[0] = std::move(fixture[0]);
    meshes[2] = std::move(fixture[1]);
    for (int i : {1, 3, 4, 5})
        meshes[i].mask = i == 1 ? 4 : 1;
    auto tri = [](Mesh &m, XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c, uint32_t material) {
        for (auto p : {a, b, c})
            m.vertices.push_back({p, material, {}, ~0u, 0});
    };
    auto box = [&](Mesh &m, XMFLOAT3 p, XMFLOAT3 h, uint32_t material) {
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
    // Glass rolling avatar. Closed sphere, smooth shading normals reconstructed in raygen.
    auto sphere = [](float u, float v) {
        return XMFLOAT3{.68f * std::sin(v) * std::cos(u), .68f * std::cos(v),
                        .68f * std::sin(v) * std::sin(u)};
    };
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            float u = XM_2PI * x / 64, v = XM_PI * y / 32, un = XM_2PI * (x + 1) / 64,
                  vn = XM_PI * (y + 1) / 32;
            if (y > 0)
                tri(meshes[1], sphere(u, v), sphere(un, v), sphere(un, vn), 2);
            if (y < 31)
                tri(meshes[1], sphere(u, v), sphere(un, vn), sphere(u, vn), 2);
        }
    // Enforce outward normals (important for entering/exiting dielectric medium tracking).
    auto &s = meshes[1].vertices;
    for (size_t i = 0; i < s.size(); i += 3) {
        auto a = XMLoadFloat3(&s[i].position), b = XMLoadFloat3(&s[i + 1].position),
             c = XMLoadFloat3(&s[i + 2].position);
        if (XMVectorGetX(XMVector3Dot(XMVector3Cross(b - a, c - a), a + b + c)) < 0)
            std::swap(s[i + 1], s[i + 2]);
    }
    box(meshes[3], {0, 0, 0}, {.38f, .38f, .38f}, 4);
    box(meshes[4], {0, 0, 0}, {.38f, .38f, .38f}, 5);
    box(meshes[5], {0, 0, 0}, {1.48f, 1.4f, .16f}, 3);
    for (float x : {-1.35f, 1.35f})
        box(meshes[5], {x, 0, -.17f}, {.025f, 1.35f, .025f}, 5);
    auto &room = meshes[0];
    // Portal and containment match the existing physics/game's exit convention.
    box(room, {-3.8f, 1.4f, -6.2f}, {2.2f, 1.4f, .16f}, 3);
    box(room, {3.8f, 1.4f, -6.2f}, {2.2f, 1.4f, .16f}, 3);
    box(room, {0, -.2f, -6.65f}, {1.6f, .2f, .8f}, 3);
    // Optical cradle: elevated prism rests on a real collider, not in midair.
    box(room, {0, .29f, 0}, {1.05f, .29f, 1.05f}, 7);
    for (float x : {-1.03f, 1.03f})
        box(room, {x, .61f, 0}, {.018f, .015f, 1.05f}, 5);
    for (float z : {-1.03f, 1.03f})
        box(room, {0, .61f, z}, {1.05f, .015f, .018f}, 5);
    // Neon architecture reflected in glass; receiver charts remain unmodified.
    for (float x : {-5.96f, 5.96f}) {
        box(room, {x, .10f, 1}, {.015f, .035f, 6.8f}, x < 0 ? 4 : 5);
        for (float z : {-4.f, 0.f, 4.f, 7.7f}) {
            box(room, {x, 2.6f, z}, {.025f, 1.8f, .028f}, x < 0 ? 4 : 5);
            box(room, {x, 4.45f, z}, {.025f, .025f, .35f}, 6);
        }
    }
    for (float x : {-4.f, -2.f, 0.f, 2.f, 4.f})
        box(room, {x, 4.8f, 7.97f}, {.7f, .018f, .018f}, 5);
    // White collimator fixture: its aperture is unobstructed on the emission side.
    box(room, {0, 1.2f, -5.25f}, {.30f, 1.2f, .25f}, 3);
    box(room, {0, 2, -5.035f}, {.22f, .18f, .015f}, 6);
    if (fluidRoom) {
        if (boat) {
            Mesh craft;
            craft.mask = 1;
            for (const auto &part : watercraft::hull)
                box(craft, part.center, part.half, 15);
            box(craft, {0, .40f, .30f}, {.32f, .06f, .32f}, 16);
            box(craft, {0, .67f, .61f}, {.32f, .26f, .055f}, 16);
            box(craft, {0, .55f, -.50f}, {.35f, .12f, .12f}, 17);
            box(craft, {0, .72f, -.49f}, {.20f, .035f, .05f}, 16);
            box(craft, {0, -.03f, 1.02f}, {.13f, .32f, .12f}, 17);
            for (float x : {-.72f, .72f})
                box(craft, {x, .185f, 0}, {.06f, .015f, 1.23f}, 16);
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
            box(room, {-5.80f, 1.85f, z}, {.18f, .26f, .035f}, 3);
        for (float y : {1.59f, 2.11f})
            box(room, {-5.80f, y, 2.4f}, {.18f, .035f, .26f}, 3);
        box(room, {-5.965f, .94f, 2.4f}, {.025f, .22f, .22f}, 3);
        box(room, {-5.925f, .94f, 2.4f}, {.025f, .14f, .14f}, 14);
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
