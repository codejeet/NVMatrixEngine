#include "fluid_mac.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace lab {
using namespace DirectX;
// Independent double-precision face assembly. This does not evaluate the GPU's
// row stencil to generate its reference matrix or its expected projected flux.
void FluidMac::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Adaptive MAC operator snapshot");
    auto before = static_cast<const XMFLOAT4 *>(data), after = before + faceCount;
    auto ownership = reinterpret_cast<const uint32_t *>(after + faceCount);
    auto lod = ownership + grid.w;
    auto gpuRows = reinterpret_cast<const Row *>(lod + coarse.w);
    auto pressureData = reinterpret_cast<const char *>(gpuRows + grid.w);
    auto pressure = [&](uint32_t id) {
        if (multigrid) {
            double value;
            memcpy(&value, pressureData + uint64_t(id) * 8, 8);
            return value;
        }
        return double(reinterpret_cast<const float *>(pressureData)[id]);
    };
    auto cells = reinterpret_cast<const XMFLOAT4 *>(pressureData + uint64_t(grid.w) * (multigrid ? 8 : 4));
    auto solids = cells + grid.w, material = solids + grid.w;
    const auto cutVolume = reinterpret_cast<const XMFLOAT2 *>(material + grid.w);
    const auto cutArea = reinterpret_cast<const float *>(cutVolume + grid.w);
    const auto exactData = reinterpret_cast<const char *>(cutArea + faceCount);
    auto exactRow = [&](uint32_t id) {
        FluidCutPressureRow row;
        memcpy(&row, exactData + uint64_t(id) * sizeof(row), sizeof(row));
        return row;
    };
    const auto volumeFluxData = exactData + (multigrid ? uint64_t(grid.w) * sizeof(FluidCutPressureRow) : 0);
    auto volumeFlux = [&](uint32_t id) {
        double value;
        memcpy(&value, volumeFluxData + uint64_t(id) * 8, 8);
        return value;
    };
    using Coord = std::array<int, 3>;
    auto inside = [&](Coord p) {
        return p[0] >= 0 && p[1] >= 0 && p[2] >= 0 && p[0] < int(grid.x) && p[1] < int(grid.y) &&
               p[2] < int(grid.z);
    };
    auto index = [&](Coord p) { return uint32_t((p[2] * grid.y + p[1]) * grid.x + p[0]); };
    auto coord = [&](uint32_t i) {
        return Coord{int(i % grid.x), int(i / grid.x % grid.y), int(i / (grid.x * grid.y))};
    };
    auto pressureCapacity = [&](Coord p) {
        const auto v = cutVolume[index(p)];
        return cut.timeCentered ? .5 * (double(v.x) + v.y) : double(v.x);
    };
    auto solid = [&](Coord p) {
        return !inside(p) || (cutProjection ? pressureCapacity(p) == 0 : solids[index(p)].x < 0);
    };
    auto liquid = [&](Coord p) { return inside(p) && cells[index(p)].z == 1; };
    auto width = [&](Coord p) {
        return inside(p) && (lod[(p[2] / 2 * coarse.y + p[1] / 2) * coarse.x + p[0] / 2] & 1) ? 2 : 1;
    };
    auto face = [&](Coord p, int a) {
        return a * (faceCount / 3) + (p[2] * (grid.y + 1) + p[1]) * (grid.x + 1) + p[0];
    };
    const double k = double(1.f / rate) / (double(rho) * h);
    auto volumeUnits = [&](Coord p, int size) {
        if (!cutProjection)
            return double(size * size * size);
        double v = 0;
        for (int z = 0; z < size; ++z)
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x)
                    v += pressureCapacity({p[0] + x, p[1] + y, p[2] + z});
        return v / (double(h) * h * h);
    };
    auto source = [&](Coord p, int size) {
        if (!cutProjection || !cut.swept)
            return 0.0;
        double v = 0;
        for (int z = 0; z < size; ++z)
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    const auto q = cutVolume[index({p[0] + x, p[1] + y, p[2] + z})];
                    v += double(q.x) - q.y;
                }
        return v / (double(1.f / rate) * h * h);
    };
    auto cellWidth = [&](Coord p, int axis) {
        return cutProjection
                   ? std::min(double(h),
                              std::max(0.0, double((&cut.maximum.x)[axis]) -
                                                ((&cut.minimumSpacing.x)[axis] + double(p[axis]) * h)))
                   : double(h);
    };
    std::vector<std::map<uint32_t, double>> expected(grid.w);
    std::vector<double> rhs(grid.w), flux(grid.w), cacheFlux(grid.w);
    std::vector<uint8_t> seen(faceCount);
    uint32_t leaves = 0, coarseLeaves = 0, junctions = 0;
    std::string failure;
    std::string rhsDetail, residualDetail;
    for (uint32_t id = 0; id < grid.w; ++id) {
        auto p = coord(id), b = p;
        if (cutProjection && cut.swept && cutVolume[id].x == 0 && cutVolume[id].y > 0) {
            ++closingCellSamples;
            if (cut.timeCentered && cells[id].z == 2)
                failure = "Closing cell lost temporal pressure support";
            if (liquid(p)) {
                ++closingLiquidSamples;
                closingLiquidVolumeM3 += cutVolume[id].y;
                bool connected = false;
                for (int a = 0; a < 3; ++a)
                    for (int side = 0; side < 2; ++side) {
                        auto neighbor = p, f = p;
                        neighbor[a] += side ? 1 : -1;
                        f[a] += side;
                        connected |= !solid(neighbor) && cutArea[face(f, a)] > 0;
                    }
                closingConnectedSamples += connected;
            }
        }
        const int w = width(p);
        if (w == 2)
            for (auto &v : b)
                v &= ~1;
        if (ownership[id] != index(b))
            failure = "Invalid adaptive MAC leaf partition";
        if (liquid(p) && ownership[id] == id) {
            ++leaves;
            if (!std::isfinite(pressure(id)))
                failure = "Nonfinite adaptive pressure";
            if (w == 2) {
                ++coarseLeaves;
                for (int z = -2; z < 4; ++z)
                    for (int y = -2; y < 4; ++y)
                        for (int x = -2; x < 4; ++x) {
                            Coord q{p[0] + x, p[1] + y, p[2] + z};
                            if (!liquid(q) || solid(q))
                                failure = "Coarse MAC crossed guarded surface/solid boundary";
                            if (cutProjection && inside(q) && std::abs(volumeUnits(q, 1) - 1) > 1.1e-6)
                                failure = "Coarse MAC crossed a partial-volume cell";
                        }
            }
        }
    }
    // Refuse invalid maps before using them as host indices.
    if (failure.empty())
        for (int a = 0; a < 3; ++a) {
            Coord extent{int(grid.x), int(grid.y), int(grid.z)};
            ++extent[a];
            for (int z = 0; z < extent[2]; ++z)
                for (int y = 0; y < extent[1]; ++y)
                    for (int x = 0; x < extent[0]; ++x) {
                        Coord p{x, y, z}, left = p;
                        --left[a];
                        if (inside(left) && inside(p) && ownership[index(left)] == ownership[index(p)])
                            continue;
                        const int wl = width(left), wr = width(p), w = std::max(wl, wr);
                        Coord base = p;
                        if (w == 2) {
                            base[(a + 1) % 3] &= ~1;
                            base[(a + 2) % 3] &= ~1;
                        }
                        const auto f = face(base, a);
                        if (seen[f])
                            continue;
                        seen[f] = 1;
                        if (w == 2 && wl != wr)
                            ++junctions;
                        const double distance = cutProjection && w == 1 && inside(left) && inside(p)
                                                    ? .5 * (cellWidth(left, a) + cellWidth(p, a)) / h
                                                    : .5 * (wl + wr);
                        const double volume = w * w * distance;
                        const double aperture =
                            cutProjection && w == 1
                                ? (solid(left) || solid(p) ? 0
                                                           : double(cutArea[face(p, a)]) / (double(h) * h))
                                : 1;
                        double u = 0;
                        std::map<uint32_t, double> d;
                        for (int j = 0; j < w; ++j)
                            for (int i = 0; i < w; ++i) {
                                auto q = base;
                                q[(a + 1) % 3] += i;
                                q[(a + 2) % 3] += j;
                                u += before[face(q, a)].x / (w * w);
                                auto l = q;
                                --l[a];
                                if (inside(l))
                                    d[ownership[index(l)]] += 1;
                                if (inside(q))
                                    d[ownership[index(q)]] -= 1;
                            }
                        for (auto [id, di] : d)
                            if (cells[id].z == 1)
                                rhs[id] -= aperture * di * u / k;
                        const bool blocked = solid(left) || solid(p) || aperture == 0;
                        auto value = [&](uint32_t id) {
                            if (cells[id].z == 1)
                                return pressure(id);
                            if (sigma == 0 || blocked || (!liquid(left) && !liquid(p)))
                                return 0.;
                            const auto l = material[index(liquid(left) ? left : p)], r = material[id];
                            const double t = std::abs(l.x - r.x) > 1e-6
                                                 ? std::clamp((double(l.x) - .5) / (l.x - r.x), 0., 1.)
                                                 : .5;
                            return double(sigma) * (l.y + (r.y - l.y) * t);
                        };
                        double projected = u;
                        if (!blocked) {
                            for (auto [id, di] : d)
                                if (cells[id].z == 1)
                                    for (auto [other, dj] : d) {
                                        const double entry = aperture * di * dj / volume;
                                        if (cells[other].z == 1)
                                            expected[id][other] += entry;
                                        else
                                            rhs[id] -= entry * value(other);
                                    }
                            // Include diagonal from prescribed air pressure. It belongs to
                            // the liquid-liquid principal submatrix of the full operator.
                            for (auto [id, di] : d)
                                projected += k * di * value(id) / volume;
                        }
                        if (liquid(left) || liquid(p)) {
                            for (int j = 0; j < w; ++j)
                                for (int i = 0; i < w; ++i) {
                                    auto q = base;
                                    q[(a + 1) % 3] += i;
                                    q[(a + 2) % 3] += j;
                                    const double actual = after[face(q, a)].x;
                                    if (!std::isfinite(actual))
                                        failure = "Nonfinite adaptive MAC velocity";
                                    fluxError = std::max(fluxError, std::abs(actual - projected) /
                                                                        (1 + std::abs(projected)));
                                    if (cutProjection && aperture > 0) {
                                        const double canonical = volumeFlux(face(q, a));
                                        const double expectedFlux = projected * cutArea[face(q, a)];
                                        fluxError = std::max(fluxError, std::abs(canonical - expectedFlux) /
                                                                            (1 + std::abs(expectedFlux)));
                                        velocityCacheFluxError =
                                            std::max(velocityCacheFluxError,
                                                     std::abs(canonical - actual * cutArea[face(q, a)]));
                                        if (!std::isfinite(canonical))
                                            failure = "Nonfinite canonical cut-cell flux";
                                    } else if (cutProjection && volumeFlux(face(q, a)) != 0)
                                        failure = "Closed cut-cell face has nonzero canonical flux";
                                }
                            for (auto [id, di] : d)
                                if (cells[id].z == 1) {
                                    flux[id] += cutProjection ? di * volumeFlux(f) / (double(h) * h)
                                                              : aperture * di * after[f].x;
                                    cacheFlux[id] += aperture * di * after[f].x;
                                }
                        }
                    }
        }
    if (failure.empty())
        for (uint32_t id = 0; id < grid.w; ++id) {
            if (cells[id].z != 1 || ownership[id] != id)
                continue;
            const auto &row = gpuRows[id];
            const int w = width(coord(id));
            const double actualVolume = volumeUnits(coord(id), w);
            const double sweptSource = source(coord(id), w);
            rhs[id] -= sweptSource / k;
            flux[id] += sweptSource;
            cacheFlux[id] += sweptSource;
            if (cutProjection)
                velocityCacheDivergence =
                    std::max(velocityCacheDivergence, std::abs(cacheFlux[id]) / (actualVolume * h));
            double boundary = 0;
            for (auto [other, coefficient] : expected[id]) {
                (void)other;
                boundary += coefficient;
            }
            if (std::abs(row.volumeUnits - actualVolume) > 2e-6 * actualVolume ||
                std::abs(row.boundaryDiagonal - boundary) > 2e-5)
                failure = "Invalid explicit MAC boundary/volume metadata";
            if (row.count > 24) {
                failure = "Invalid adaptive MAC row";
                break;
            }
            std::map<uint32_t, double> actual{{id, row.diagonal}};
            for (uint32_t j = 0; j < row.count; ++j) {
                if (row.neighbor[j] >= grid.w || ownership[row.neighbor[j]] != row.neighbor[j] ||
                    cells[row.neighbor[j]].z != 1) {
                    failure = "Invalid adaptive MAC neighbor";
                    break;
                }
                actual[row.neighbor[j]] += row.coefficient[j];
            }
            if (!failure.empty())
                break;
            double absoluteSum = 0;
            for (auto [other, coefficient] : expected[id]) {
                (void)other;
                absoluteSum += std::abs(coefficient);
            }
            const double expectedStep = (metrics[1] || cutProjection)
                                            ? (absoluteSum > 0 ? 1.8 / absoluteSum : 0)
                                            : (row.diagonal > 0 ? 1. / row.diagonal : 0);
            // Sliver conductances can make 1/sum|Aij| very large. Check the
            // dimensionless relaxation factor, not an absolute inverse-weight
            // tolerance that rejects an accurately rounded FP32 step.
            double storedAbsoluteSum = row.diagonal;
            for (uint32_t j = 0; j < row.count; ++j)
                storedAbsoluteSum += std::abs(row.coefficient[j]);
            // This bound belongs to the stored FP32 preconditioner. Its matrix
            // and the precise physical operator have separate face audits.
            const double stepError = cutProjection && storedAbsoluteSum > 0
                                         ? std::abs(double(row.step) * storedAbsoluteSum - 1.8)
                                         : std::abs(row.step - expectedStep);
            if (!std::isfinite(row.step) || stepError > .000001)
                failure = "Invalid adaptive MAC relaxation bound";
            double ap = 0;
            for (auto [other, coefficient] : expected[id]) {
                matrixError = std::max(matrixError,
                                       std::abs(actual[other] - coefficient) / (1 + std::abs(coefficient)));
                ap += coefficient * pressure(other);
                actual.erase(other);
            }
            for (auto [other, coefficient] : actual) {
                (void)other;
                matrixError = std::max(matrixError, std::abs(coefficient));
            }
            if (!std::isfinite(row.rhs) || !std::isfinite(row.diagonal))
                failure = "Nonfinite adaptive MAC row";
            // RHS sums large cancelling momentum fluxes. Audit the error in physical
            // divergence units, not relative to a nearly zero pressure RHS.
            const double target = cutProjection && multigrid ? exactRow(id).rhs : double(row.rhs);
            if (cutProjection) {
                canonicalDivergence = std::max(canonicalDivergence, std::abs(flux[id]) / (actualVolume * h));
                if (multigrid) {
                    const auto e = exactRow(id);
                    if (!std::isfinite(e.rhs) || !std::isfinite(e.diagonal))
                        failure = "Nonfinite precise cut-cell target";
                    if (e.diagonal >= 0) {
                        std::map<uint32_t, double> exactMatrix;
                        double diagonal = 0;
                        auto p = coord(id);
                        for (uint32_t a = 0; a < 3; ++a)
                            for (uint32_t s = 0; s < 2; ++s) {
                                const double weight = e.conductance[a * 2 + s];
                                if (!std::isfinite(weight) || weight < 0)
                                    failure = "Invalid precise cut-cell face weight";
                                diagonal += weight;
                                auto n = p;
                                n[a] += s ? 1 : -1;
                                if (liquid(n))
                                    exactMatrix[ownership[index(n)]] -= weight;
                            }
                        exactMatrix[id] += diagonal;
                        preciseMatrixError = std::max(preciseMatrixError, std::abs(e.diagonal - diagonal) /
                                                                              (1 + std::abs(diagonal)));
                        for (auto [other, coefficient] : expected[id]) {
                            preciseMatrixError =
                                std::max(preciseMatrixError, std::abs(exactMatrix[other] - coefficient) /
                                                                 (1 + std::abs(coefficient)));
                            exactMatrix.erase(other);
                        }
                        for (auto [other, coefficient] : exactMatrix) {
                            (void)other;
                            preciseMatrixError = std::max(preciseMatrixError, std::abs(coefficient));
                        }
                    }
                }
            }
            const double rhsError = std::abs(target - rhs[id]) * k / (actualVolume * h);
            const double residualError = std::abs(flux[id] - (ap - rhs[id]) * k) / (actualVolume * h);
            auto detail = [&] {
                return " row=" + std::to_string(id) + " volume=" + std::to_string(actualVolume) +
                       " source=" + std::to_string(sweptSource) + " rhs=" + std::to_string(rhs[id]) +
                       " gpuRhs=" + std::to_string(target) + " pressure=" + std::to_string(pressure(id)) +
                       " fluxDiv=" + std::to_string(flux[id] / (actualVolume * h));
            };
            if (rhsError > rhsDivergenceError) {
                rhsDivergenceError = rhsError;
                rhsDetail = detail();
            }
            if (residualError > residualMismatch) {
                residualMismatch = residualError;
                residualDetail = detail();
            }
            if (w == 2) {
                auto base = coord(id);
                for (int j = 0; j < 8; ++j) {
                    Coord p{base[0] + (j & 1), base[1] + ((j >> 1) & 1), base[2] + ((j >> 2) & 1)};
                    double divergence = 0;
                    for (int a = 0; a < 3; ++a) {
                        auto q = p;
                        ++q[a];
                        divergence += after[face(q, a)].x - after[face(p, a)].x;
                    }
                    prolongationError = std::max(prolongationError, std::abs(divergence - flux[id] / 8) / h);
                }
            }
        }
    snapshot.resource->Unmap(0, &written);
    if (leaves != metrics[0] || coarseLeaves != metrics[1] || junctions != metrics[2])
        failure = "Adaptive MAC work-list counters disagree";
    if (matrixError > .00002)
        failure = "Adaptive MAC matrix differs from independent face assembly";
    if (rhsDivergenceError > .00002)
        failure = "Adaptive MAC RHS differs from independent flux divergence";
    if (fluxError > .00004)
        failure = "Adaptive MAC projected flux differs from pressure gradient";
    if (residualMismatch > .0002)
        failure = "Adaptive MAC residual differs from projected divergence";
    if (prolongationError > .0002)
        failure = "Adaptive MAC child divergence discontinuity";
    if (preciseMatrixError > 1e-10)
        failure = "Precise cut-cell operator differs from independent physical face assembly";
    if (!failure.empty())
        throw std::runtime_error(failure + " (matrix=" + std::to_string(matrixError) + ", rhs=" +
                                 std::to_string(rhsDivergenceError) + ", flux=" + std::to_string(fluxError) +
                                 ", residual=" + std::to_string(residualMismatch) +
                                 ", prolongation=" + std::to_string(prolongationError) +
                                 ") rhs:" + rhsDetail + " residual:" + residualDetail);
    validated = true;
    ++auditedFrames;
}
} // namespace lab
