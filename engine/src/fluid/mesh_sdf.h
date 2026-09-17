#pragma once
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace lab {
// Reference import-time bake for small watertight collision meshes. Deliberately
// separate from the runtime solver: no triangle/particle CPU work per frame.
// Exact distances + solid-angle classification, not an unsigned voxel shell.
struct MeshSdfAsset {
    DirectX::XMFLOAT4 minimumSpacing{};
    DirectX::XMUINT4 dimensions{};
    std::vector<float> phi;
    static MeshSdfAsset bake(const std::vector<DirectX::XMFLOAT3> &triangles, float spacing) {
        using P = std::array<double, 3>;
        auto sub = [](P a, P b) { return P{a[0] - b[0], a[1] - b[1], a[2] - b[2]}; };
        auto dot = [](P a, P b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
        auto cross = [](P a, P b) {
            return P{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        };
        if (triangles.empty() || triangles.size() % 3 || !std::isfinite(spacing) || spacing <= 0)
            throw std::runtime_error("Invalid mesh SDF input");
        std::vector<P> vertices;
        std::map<P, uint32_t> ids;
        std::map<std::pair<uint32_t, uint32_t>, std::pair<int, int>> edges;
        P lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (const auto &v : triangles) {
            P p{v.x, v.y, v.z};
            for (int a = 0; a < 3; ++a) {
                if (!std::isfinite(p[a]))
                    throw std::runtime_error("Nonfinite SDF mesh");
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
            vertices.push_back(p);
            if (!ids.contains(p))
                ids[p] = uint32_t(ids.size());
        }
        for (size_t i = 0; i < vertices.size(); i += 3) {
            const P a = vertices[i], b = vertices[i + 1], c = vertices[i + 2];
            P n = cross(sub(b, a), sub(c, a));
            if (dot(n, n) < 1e-20)
                throw std::runtime_error("Degenerate mesh SDF triangle");
            for (int j = 0; j < 3; ++j) {
                uint32_t from = ids[vertices[i + j]], to = ids[vertices[i + (j + 1) % 3]];
                auto &e = edges[{std::min(from, to), std::max(from, to)}];
                e.first++;
                e.second += from < to ? 1 : -1;
            }
        }
        for (auto &[edge, count] : edges) {
            (void)edge;
            if (count.first != 2 || count.second != 0)
                throw std::runtime_error("Mesh SDF requires a closed consistently oriented manifold");
        }
        MeshSdfAsset out;
        for (int a = 0; a < 3; ++a) {
            (&out.minimumSpacing.x)[a] = float(lo[a] - 2 * spacing);
            (&out.dimensions.x)[a] = uint32_t(std::ceil((hi[a] - lo[a]) / spacing)) + 5;
        }
        out.minimumSpacing.w = spacing;
        const uint64_t count = uint64_t(out.dimensions.x) * out.dimensions.y * out.dimensions.z;
        if (count > 2000000 || count * (vertices.size() / 3) > 100000000)
            throw std::runtime_error("Reference mesh SDF bake budget exceeded; pre-bake a coarser asset");
        out.phi.resize(size_t(count));
        auto segment2 = [&](P p, P a, P b) {
            P e = sub(b, a), v = sub(p, a);
            double t = std::clamp(dot(v, e) / dot(e, e), 0., 1.);
            P q{v[0] - t * e[0], v[1] - t * e[1], v[2] - t * e[2]};
            return dot(q, q);
        };
        for (uint32_t z = 0; z < out.dimensions.z; ++z)
            for (uint32_t y = 0; y < out.dimensions.y; ++y)
                for (uint32_t x = 0; x < out.dimensions.x; ++x) {
                    P p{out.minimumSpacing.x + x * double(spacing),
                        out.minimumSpacing.y + y * double(spacing),
                        out.minimumSpacing.z + z * double(spacing)};
                    double distance2 = 1e30, angle = 0;
                    for (size_t i = 0; i < vertices.size(); i += 3) {
                        P a = vertices[i], b = vertices[i + 1], c = vertices[i + 2], ab = sub(b, a),
                          ac = sub(c, a), ap = sub(p, a), n = cross(ab, ac);
                        double aa = dot(ab, ab), bb = dot(ac, ac), cc = dot(ab, ac), den = aa * bb - cc * cc;
                        double u = (dot(ap, ab) * bb - dot(ap, ac) * cc) / den,
                               v = (dot(ap, ac) * aa - dot(ap, ab) * cc) / den;
                        double d2 = u >= 0 && v >= 0 && u + v <= 1
                                        ? std::pow(dot(ap, n), 2) / dot(n, n)
                                        : std::min({segment2(p, a, b), segment2(p, b, c), segment2(p, c, a)});
                        distance2 = std::min(distance2, d2);
                        a = sub(a, p);
                        b = sub(b, p);
                        c = sub(c, p);
                        double la = std::sqrt(dot(a, a)), lb = std::sqrt(dot(b, b)),
                               lc = std::sqrt(dot(c, c));
                        angle += 2 * std::atan2(dot(a, cross(b, c)), la * lb * lc + dot(a, b) * lc +
                                                                         dot(b, c) * la + dot(c, a) * lb);
                    }
                    out.phi[(z * out.dimensions.y + y) * out.dimensions.x + x] =
                        float(std::sqrt(distance2) * (std::abs(angle) > 6.283185307179586 ? -1 : 1));
                }
        return out;
    }
    float sample(DirectX::XMFLOAT3 p) const {
        uint32_t c[3];
        float f[3], outside2 = 0;
        for (int a = 0; a < 3; ++a) {
            float g = ((&p.x)[a] - (&minimumSpacing.x)[a]) / minimumSpacing.w;
            float q = std::clamp(g, 0.f, float((&dimensions.x)[a] - 1));
            outside2 += (g - q) * (g - q);
            c[a] = std::min(uint32_t(q), (&dimensions.x)[a] - 2);
            f[a] = q - c[a];
        }
        float value = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t x = i & 1, y = (i >> 1) & 1, z = i >> 2;
            value += phi[((c[2] + z) * dimensions.y + c[1] + y) * dimensions.x + c[0] + x] *
                     (x ? f[0] : 1 - f[0]) * (y ? f[1] : 1 - f[1]) * (z ? f[2] : 1 - f[2]);
        }
        return value + std::sqrt(outside2) * minimumSpacing.w;
    }
};
} // namespace lab
