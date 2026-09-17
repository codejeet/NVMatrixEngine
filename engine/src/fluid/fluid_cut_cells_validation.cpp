#include "fluid_cut_cells.h"
#include <numeric>

#include "fluid_numeric.h"

namespace lab {
using namespace DirectX;
namespace {
struct KernelState {
    float mass;
    uint32_t fingerprint[2], rebuilt;
};
static_assert(sizeof(KernelState) == 16);
double kernelCDF(double x) {
    const double a = std::abs(x);
    const double tail = a >= 1.5 ? 0 : (a >= .5 ? std::pow(1.5 - a, 3) / 6 : .5 - .75 * a + a * a * a / 3);
    return x < 0 ? tail : 1 - tail;
}
double kernelIntegral(double a, double b) {
    return a >= 0 ? kernelCDF(-a) - kernelCDF(-b) : kernelCDF(b) - kernelCDF(a);
}
double kernelValue(double x) {
    x = std::abs(x);
    return x < .5 ? .75 - x * x : (x < 1.5 ? .5 * std::pow(1.5 - x, 2) : 0);
}
// Higher-order FP64 reference, independently reconstructing the simplex from
// sorted coordinates. Different quadrature from the six-point GPU integral.
double solidKernelReference(const std::array<double, 8> &phi, const std::array<double, 3> &offset,
                            const std::array<double, 3> &width) {
    uint32_t axis = 0;
    double largest = -1;
    for (uint32_t a = 0; a < 3; ++a) {
        double gradient = 0;
        for (uint32_t i = 0; i < 8; ++i)
            gradient += ((i >> a) & 1 ? 1 : -1) * phi[i] / width[a];
        if (std::abs(gradient) > largest) {
            largest = std::abs(gradient);
            axis = a;
        }
    }
    constexpr double nodes[] = {.0198550717512319, .101666761293187, .237233795041836, .408282678752175,
                                .591717321247825,  .762766204958164, .898333238706813, .980144928248768};
    constexpr double weights[] = {.0506142681451881, .111190517226687, .156853322938944, .181341891689181,
                                  .181341891689181,  .156853322938944, .111190517226687, .0506142681451881};
    const uint32_t b = (axis + 1) % 3, c = (axis + 2) % 3;
    auto sample = [&](std::array<double, 3> p) {
        std::array<uint32_t, 3> order{0, 1, 2};
        std::sort(order.begin(), order.end(), [&](auto i, auto j) { return p[i] > p[j]; });
        const double u = p[order[0]], v = p[order[1]], w = p[order[2]];
        return (1 - u) * phi[0] + (u - v) * phi[size_t(1) << order[0]] +
               (v - w) * phi[(size_t(1) << order[0]) | (size_t(1) << order[1])] + w * phi[7];
    };
    double sum = 0;
    for (uint32_t i = 0; i < 8; ++i)
        for (uint32_t j = 0; j < 8; ++j) {
            std::array<double, 3> p{};
            p[b] = nodes[i];
            p[c] = nodes[j];
            const double splits[] = {0, std::min(p[b], p[c]), std::max(p[b], p[c]), 1};
            double line = 0;
            for (uint32_t k = 0; k < 3; ++k) {
                double lo = splits[k], hi = splits[k + 1];
                p[axis] = lo;
                const double left = sample(p);
                p[axis] = hi;
                const double right = sample(p);
                if (left >= 0 && right >= 0)
                    continue;
                if (left >= 0 || right >= 0) {
                    const double crossing = lo + (hi - lo) * left / (left - right);
                    if (left >= 0)
                        lo = crossing;
                    else
                        hi = crossing;
                }
                line += kernelIntegral(offset[axis] + lo * width[axis], offset[axis] + hi * width[axis]);
            }
            sum += line * weights[i] * weights[j] * kernelValue(offset[b] + nodes[i] * width[b]) *
                   kernelValue(offset[c] + nodes[j] * width[c]) * width[b] * width[c];
        }
    return sum;
}
double simplex(std::span<const double> phi) {
    double p[4], n[4];
    uint32_t pi = 0, ni = 0;
    for (double d : phi)
        if (d >= 0)
            p[pi++] = d;
        else
            n[ni++] = d;
    if (!pi)
        return 0;
    if (!ni)
        return 1;
    if (pi == 1) {
        double v = 1;
        for (uint32_t i = 0; i < ni; ++i)
            v *= p[0] / (p[0] - n[i]);
        return v;
    }
    if (ni == 1) {
        double v = 1;
        for (uint32_t i = 0; i < pi; ++i)
            v *= n[0] / (n[0] - p[i]);
        return 1 - v;
    }
    const double a = p[0] / (p[0] - n[0]), b = p[0] / (p[0] - n[1]), c = p[1] / (p[1] - n[0]),
                 d = p[1] / (p[1] - n[1]);
    return a * b + a * d * (1 - b) + c * d * (1 - a);
}
double timeAreaReference(const std::array<double, 4> &a, const std::array<double, 4> &b) {
    if (*std::min_element(a.begin(), a.end()) >= 0 && *std::min_element(b.begin(), b.end()) >= 0)
        return 1;
    if (*std::max_element(a.begin(), a.end()) < 0 && *std::max_element(b.begin(), b.end()) < 0)
        return 0;
    std::vector<double> times{0, 1};
    for (uint32_t i = 0; i < 4; ++i)
        if ((a[i] < 0) != (b[i] < 0)) {
            double t = a[i] / (a[i] - b[i]);
            if (t > 0 && t < 1)
                times.push_back(t);
        }
    std::sort(times.begin(), times.end());
    constexpr double nodes[]{.0198550717512319, .101666761293187, .237233795041836, .408282678752175,
                             .591717321247825,  .762766204958164, .898333238706813, .980144928248768};
    constexpr double weights[]{.0506142681451881, .111190517226687, .156853322938944, .181341891689181,
                               .181341891689181,  .156853322938944, .111190517226687, .0506142681451881};
    double area = 0;
    for (size_t j = 1; j < times.size(); ++j)
        for (uint32_t k = 0; k < 8; ++k) {
            double dt = times[j] - times[j - 1], t = times[j - 1] + dt * nodes[k], p[4];
            for (uint32_t i = 0; i < 4; ++i)
                p[i] = a[i] + t * (b[i] - a[i]);
            const double tri0[]{p[0], p[1], p[3]}, tri1[]{p[0], p[2], p[3]};
            area += dt * weights[k] * .5 * (simplex(tri0) + simplex(tri1));
        }
    return area;
}
double solidReference(const FluidCollider &c, XMFLOAT3 world, const MeshSdfAsset &mesh) {
    XMFLOAT3 p;
    XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&world), XMLoadFloat4x4(&c.worldToLocal)));
    const auto e = c.extentType;
    const uint32_t type = uint32_t(e.w);
    if (type == 0)
        return std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z) - e.x;
    if (type == 1) {
        const double x = std::abs(p.x) - e.x, y = std::abs(p.y) - e.y, z = std::abs(p.z) - e.z;
        return std::sqrt(std::pow(std::max(0., x), 2) + std::pow(std::max(0., y), 2) +
                         std::pow(std::max(0., z), 2)) +
               std::min(std::max({x, y, z}), 0.);
    }
    if (type == 2) {
        p.y -= std::clamp(p.y, -e.y, e.y);
        return std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z) - e.x;
    }
    if (type == 3) {
        const double x = std::hypot(p.x, p.z) - e.x, y = std::abs(p.y) - e.y;
        return std::hypot(std::max(0., x), std::max(0., y)) + std::min(std::max(x, y), 0.);
    }
    if (type == 4)
        return p.y;
    double outside = 0;
    for (uint32_t a = 0; a < 3; ++a) {
        const double q = ((&p.x)[a] - (&c.meshMinimumSpacing.x)[a]) / c.meshMinimumSpacing.w;
        outside += std::pow(q - std::clamp(q, 0., double((&c.meshDimensions.x)[a] - 1)), 2);
    }
    if (outside)
        return (2 + std::sqrt(outside)) * c.meshMinimumSpacing.w;
    return mesh.sample(p);
}
} // namespace
void FluidCutCells::validateSnapshot() {
    void *mapped;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &mapped), "Cut-cell independent snapshot");
    const auto bytes = static_cast<const char *>(mapped);
    const auto phi = reinterpret_cast<const float *>(bytes + snapshotOffsets[0]);
    const auto volume = reinterpret_cast<const XMFLOAT2 *>(bytes + snapshotOffsets[1]);
    const auto area = reinterpret_cast<const float *>(bytes + snapshotOffsets[2]);
    const auto coarseVolume =
        readFluidNumbers<FluidDouble2>(bytes + snapshotOffsets[3], constants.coarse.w, preciseBulk);
    const auto coarseArea = reinterpret_cast<const float *>(bytes + snapshotOffsets[4]);
    const auto prior = reinterpret_cast<const XMFLOAT2 *>(bytes + snapshotOffsets[5]);
    const auto inventory = readFluidNumbers<FluidDouble4>(
        bytes + snapshotOffsets[6], bulkConnected ? constants.coarse.w : 0, preciseBulk);
    const auto solidKernel = reinterpret_cast<const KernelState *>(bytes + snapshotOffsets[7]);
    const auto kernelWork = reinterpret_cast<const uint32_t *>(bytes + snapshotOffsets[8]);
    const auto previousPhi = reinterpret_cast<const float *>(bytes + snapshotOffsets[9]);
    const auto integratedArea = reinterpret_cast<const float *>(bytes + snapshotOffsets[10]);
    const auto priorPhi = reinterpret_cast<const float *>(bytes + snapshotOffsets[11]);
    const auto f = constants.fine, c = constants.coarse;
    const auto lo = constants.minimumCell, hi = constants.maximum;
    using Coord = std::array<uint32_t, 3>;
    auto coord = [](uint32_t id, XMUINT4 n) { return Coord{id % n.x, (id / n.x) % n.y, id / (n.x * n.y)}; };
    auto index = [](Coord p, XMUINT4 n) { return (p[2] * n.y + p[1]) * n.x + p[0]; };
    const XMUINT4 nodes{f.x + 1, f.y + 1, f.z + 1, 0};
    auto width = [&](Coord p, uint32_t scale) {
        std::array<double, 3> w{};
        for (uint32_t a = 0; a < 3; ++a)
            w[a] = std::max(0., std::min(double(scale) * lo.w,
                                         double((&hi.x)[a]) - ((&lo.x)[a] + double(p[a]) * scale * lo.w)));
        return w;
    };
    auto capacity = [&](Coord p, uint32_t scale) {
        auto w = width(p, scale);
        return w[0] * w[1] * w[2];
    };
    auto face = [&](Coord p, uint32_t axis, XMUINT4 n) {
        uint32_t s = (n.x + 1) * (n.y + 1) * (n.z + 1);
        return axis * s + index(p, {n.x + 1, n.y + 1, n.z + 1, 0});
    };
    bool bad = false;
    double geom = 0, restrictError = 0, history = 0;
    double temporalError = 0;
    const double h = lo.w, h3 = h * h * h;
    double kernel = 0;
    uint32_t kernelSamples = 0;
    if (constants.control.w & 4) {
        std::vector<uint8_t> visited(f.w);
        const uint32_t count = kernelWork[0];
        bad |= count > f.w || metrics.kernel.x != (frameUpdates ? count : 0) || metrics.kernel.y != f.w;
        for (uint32_t i = 0; i < std::min(count, f.w); ++i) {
            const uint32_t id = kernelWork[i + 1];
            if (id >= f.w) {
                bad = true;
                continue;
            }
            bad |= visited[id] != 0;
            visited[id] = 1;
        }
        for (uint32_t id = 0; id < f.w; ++id)
            bad |= solidKernel[id].rebuilt != visited[id];
    }
    for (uint32_t id = 0; (constants.control.w & 4) && id < f.w; ++id) {
        const float mass = solidKernel[id].mass;
        bad |= !std::isfinite(mass) || mass < 0 || mass > 1;
        const auto center = coord(id, f);
        if (constants.control.z == 1 || constants.control.z == 3) {
            double expectedInside = 1;
            for (uint32_t a = 0; a < 3; ++a) {
                const double start =
                    a == 0 ? std::max(double(lo.x), -double(constants.fixture.w)) : (&lo.x)[a];
                expectedInside *= kernelIntegral((start - (&lo.x)[a]) / h - center[a] - .5,
                                                 ((&hi.x)[a] - double((&lo.x)[a])) / h - center[a] - .5);
            }
            kernel = std::max(kernel, std::abs(mass - (1 - expectedInside)));
            continue;
        }
        // Bounded rotating spatial samples; all entries still get range checks.
        if (kernelSamples >= 8 || ((id * 2654435761u + uint32_t(auditedFrames) * 2246822519u) & 255u) ||
            mass < 1e-6 || mass > 1 - 1e-6)
            continue;
        ++kernelSamples;
        double inside = 1;
        for (uint32_t a = 0; a < 3; ++a)
            inside *= kernelIntegral(-double(center[a]) - .5,
                                     ((&hi.x)[a] - double((&lo.x)[a])) / h - center[a] - .5);
        double expected = 1 - inside;
        for (int z = -1; z <= 1; ++z)
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x) {
                    const int delta[] = {x, y, z};
                    Coord cell{};
                    bool valid = true;
                    for (uint32_t a = 0; a < 3; ++a) {
                        const int q = int(center[a]) + delta[a];
                        valid &= q >= 0 && q < int((&f.x)[a]);
                        cell[a] = uint32_t(q);
                    }
                    if (!valid)
                        continue;
                    std::array<double, 3> offset{double(x) - .5, double(y) - .5, double(z) - .5};
                    auto w = width(cell, 1);
                    for (auto &v : w)
                        v /= h;
                    std::array<double, 8> distances{};
                    for (uint32_t i = 0; i < 8; ++i)
                        distances[i] = phi[index(
                            {cell[0] + (i & 1), cell[1] + ((i >> 1) & 1), cell[2] + (i >> 2)}, nodes)];
                    if (*std::min_element(distances.begin(), distances.end()) >= 0)
                        continue;
                    if (*std::max_element(distances.begin(), distances.end()) <= 0) {
                        double full = 1;
                        for (uint32_t a = 0; a < 3; ++a)
                            full *= kernelIntegral(offset[a], offset[a] + w[a]);
                        expected += full;
                    } else
                        expected += solidKernelReference(distances, offset, w);
                }
        kernel = std::max(kernel, std::abs(mass - expected));
    }
    for (uint32_t id = 0; id < fineStride; ++id) {
        if (timeCentered && frameUpdates)
            history = std::max(
                history,
                std::abs(double(previousPhi[id]) - (constants.control.x ? phi[id] : priorPhi[id])) / h);
        auto p = coord(id, nodes);
        XMFLOAT3 world;
        for (uint32_t a = 0; a < 3; ++a)
            (&world.x)[a] = std::min((&hi.x)[a], (&lo.x)[a] + float(p[a]) * lo.w);
        double expected = 1e6;
        if (constants.control.z == 4) {
            auto s = constants.fixture;
            expected = std::sqrt(std::pow(world.x - s.x, 2) + std::pow(world.y - s.y, 2) +
                                 std::pow(world.z - s.z, 2)) -
                       s.w;
        } else if (constants.control.z) {
            auto s = constants.fixture;
            expected = double(world.x) * s.x + double(world.y) * s.y + double(world.z) * s.z + s.w;
        } else
            for (const auto &collider : lastColliders)
                expected = std::min(expected, solidReference(collider, world, *mesh));
        geom = std::max(geom, std::abs(phi[id] - expected) / h);
        bad |= !std::isfinite(phi[id]);
    }
    static constexpr uint32_t tet[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7},
                                           {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};
    double total = 0, oldTotal = 0;
    for (uint32_t id = 0; id < f.w; ++id) {
        auto p = coord(id, f);
        double distances[8];
        for (uint32_t i = 0; i < 8; ++i)
            distances[i] = phi[index({p[0] + (i & 1), p[1] + ((i >> 1) & 1), p[2] + (i >> 2)}, nodes)];
        double fraction = 0;
        for (const auto &t : tet) {
            double values[4] = {distances[t[0]], distances[t[1]], distances[t[2]], distances[t[3]]};
            fraction += simplex(values) / 6;
        }
        const double cap = capacity(p, 1);
        geom = std::max(geom, std::abs(volume[id].x - fraction * cap) / h3);
        if (frameUpdates) {
            const double expected = constants.control.x ? volume[id].x : prior[id].x;
            history = std::max(history, std::abs(volume[id].y - expected) / h3);
        }
        bad |= !std::isfinite(volume[id].x) || !std::isfinite(volume[id].y) || volume[id].x < 0 ||
               volume[id].x > cap + h3 * 1e-5;
        total += volume[id].x;
        oldTotal += volume[id].y;
    }
    for (uint32_t id = 0; id < 3 * fineStride; ++id) {
        uint32_t a = id / fineStride, b = (a + 1) % 3, d = (a + 2) % 3;
        auto p = coord(id % fineStride, nodes);
        bool valid = true;
        for (uint32_t k = 0; k < 3; ++k)
            valid &= p[k] < (&f.x)[k] + (k == a ? 1u : 0u);
        double expected = 0;
        if (valid) {
            double values[4];
            for (uint32_t i = 0; i < 4; ++i) {
                auto q = p;
                q[b] += i & 1;
                q[d] += i >> 1;
                values[i] = phi[index(q, nodes)];
            }
            double t0[3] = {values[0], values[1], values[3]}, t1[3] = {values[0], values[2], values[3]};
            auto w = width(p, 1);
            expected = .5 * (simplex(t0) + simplex(t1)) * w[b] * w[d];
        }
        geom = std::max(geom, std::abs(area[id] - expected) / (h * h));
        bad |= !std::isfinite(area[id]) || area[id] < 0;
        if (timeCentered) {
            double average = 0;
            if (valid) {
                std::array<double, 4> a0{}, a1{};
                for (uint32_t i = 0; i < 4; ++i) {
                    auto q = p;
                    q[b] += i & 1;
                    q[d] += i >> 1;
                    a0[i] = previousPhi[index(q, nodes)];
                    a1[i] = phi[index(q, nodes)];
                }
                auto w = width(p, 1);
                average = timeAreaReference(a0, a1) * w[b] * w[d];
            }
            temporalError = std::max(temporalError, std::abs(integratedArea[id] - average) / (h * h));
            bad |= !std::isfinite(integratedArea[id]) || integratedArea[id] < 0;
        }
    }
    double restricted = 0, changed = 0, maxDelta = 0, qTotal = 0, excess = 0, solidMass = 0, maxRatio = 0;
    for (uint32_t id = 0; id < c.w; ++id) {
        auto p = coord(id, c);
        double sum = 0, previous = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            Coord q{2 * p[0] + (i & 1), 2 * p[1] + ((i >> 1) & 1), 2 * p[2] + (i >> 2)};
            if (q[0] >= f.x || q[1] >= f.y || q[2] >= f.z)
                continue;
            auto v = volume[index(q, f)];
            sum += v.x;
            previous += v.y;
        }
        restrictError = std::max({restrictError, std::abs(coarseVolume[id].x - sum) / h3,
                                  std::abs(coarseVolume[id].y - previous) / h3});
        restricted += coarseVolume[id].x;
        const double delta = frameUpdates ? std::abs(double(coarseVolume[id].x) - coarseVolume[id].y) : 0;
        changed += delta;
        maxDelta = std::max(maxDelta, delta);
        if (bulkConnected) {
            double v = coarseVolume[id].x, q = inventory[id].w;
            qTotal += q;
            excess += std::max(0., q - v);
            if (v == 0)
                solidMass += q;
            else if (v > 1e-12)
                maxRatio = std::max(maxRatio, q / v);
        }
    }
    const XMUINT4 coarseNodes{c.x + 1, c.y + 1, c.z + 1, 0};
    for (uint32_t id = 0; id < 3 * coarseStride; ++id) {
        uint32_t a = id / coarseStride, b = (a + 1) % 3, d = (a + 2) % 3;
        auto p = coord(id % coarseStride, coarseNodes);
        bool valid = true;
        for (uint32_t k = 0; k < 3; ++k)
            valid &= p[k] < (&c.x)[k] + (k == a ? 1u : 0u);
        double sum = 0;
        if (valid) {
            for (auto &v : p)
                v *= 2;
            p[a] = std::min(p[a], (&f.x)[a]);
            for (uint32_t i = 0; i < 4; ++i) {
                auto q = p;
                q[b] += i & 1;
                q[d] += i >> 1;
                if (q[b] < (&f.x)[b] && q[d] < (&f.x)[d])
                    sum += area[face(q, a, f)];
            }
        }
        restrictError = std::max(restrictError, std::abs(coarseArea[id] - sum) / (h * h));
        bad |= !std::isfinite(coarseArea[id]) || coarseArea[id] < 0;
    }
    auto checkSum = [&](double a, double b) { bad |= std::abs(a - b) > std::max(1e-8, std::abs(b) * 3e-5); };
    checkSum(restricted, total);
    checkSum(metrics.volume.x, total);
    checkSum(metrics.volume.y, oldTotal);
    checkSum(metrics.volume.z, changed);
    checkSum(metrics.volume.w, maxDelta);
    checkSum(metrics.inventory.x, qTotal);
    checkSum(metrics.inventory.y, excess);
    checkSum(metrics.inventory.z, solidMass);
    checkSum(metrics.inventory.w, maxRatio);
    // Analytic plane fixtures and a contained sphere catch errors that replaying
    // the sampled triangulation alone would miss. Curved SDFs are approximate.
    double domain = double(hi.x - lo.x) * (hi.y - lo.y) * (hi.z - lo.z);
    if (constants.control.z == 1 || constants.control.z == 3) {
        double plane = -constants.fixture.w;
        checkSum(total,
                 (hi.x - std::clamp(plane, double(lo.x), double(hi.x))) * (hi.y - lo.y) * (hi.z - lo.z));
    }
    if (constants.control.z == 2)
        checkSum(total, domain * .5);
    if (constants.control.z == 4) {
        double exact = 4 * 3.141592653589793 * std::pow(constants.fixture.w, 3) / 3;
        bad |= std::abs((domain - total) - exact) > exact * .04;
    }
    snapshot.resource->Unmap(0, &written);
    geometryError = std::max(geometryError, geom);
    restrictionError = std::max(restrictionError, restrictError);
    historyError = std::max(historyError, history);
    kernelError = std::max(kernelError, kernel);
    timeAreaError = std::max(timeAreaError, temporalError);
    const double kernelTolerance = (constants.control.z == 1 || constants.control.z == 3) ? 1e-5 : .003;
    if (bad || geom > 1e-4 || restrictError > 1e-5 || history != 0 || kernel > kernelTolerance ||
        temporalError > 1e-4)
        throw std::runtime_error(
            "Cut-cell independent geometry/face/history audit failed: geometry=" + std::to_string(geom) +
            ", restriction=" + std::to_string(restrictError) + ", history=" + std::to_string(history) +
            ", kernel=" + std::to_string(kernel) + ", time area=" + std::to_string(temporalError) +
            ", totals=" + std::to_string(bad));
    validated = true;
    ++auditedFrames;
}
} // namespace lab
