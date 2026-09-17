#include "fluid_mac_pressure.h"
#include <cmath>
#include <cstring>
#include <map>

namespace lab {
using namespace DirectX;
void FluidMacPressure::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "MGPCG hierarchy snapshot");
    auto base = static_cast<const FluidMacRow *>(data);
    auto ownership = reinterpret_cast<const uint32_t *>(base + constants.grid.w);
    auto cells = reinterpret_cast<const XMFLOAT4 *>(ownership + constants.grid.w);
    auto solutionData = reinterpret_cast<const char *>(cells + constants.grid.w);
    auto solution = [&](uint32_t id) {
        double value;
        memcpy(&value, solutionData + uint64_t(id) * 8, 8);
        return value;
    };
    auto coarse = reinterpret_cast<const CoarseRow *>(solutionData + uint64_t(constants.grid.w) * 8);
    auto lower = reinterpret_cast<const float *>(coarse + capacity);
    auto exactData = reinterpret_cast<const char *>(lower + 4096);
    auto coordinate = [](uint32_t i, XMUINT4 g) { return XMUINT3{i % g.x, i / g.x % g.y, i / (g.x * g.y)}; };
    auto index = [](XMUINT3 p, XMUINT4 g) { return (p.z * g.y + p.y) * g.x + p.x; };
    auto weight = [](const CoarseRow &r, uint32_t side) {
        return side < 4 ? (&r.xy.x)[side] : (&r.z.x)[side - 4];
    };
    using Matrix = std::vector<std::map<uint32_t, double>>;
    Matrix previous(constants.grid.w);
    std::string failure;
    double physicalMaximum = 0;
    for (uint32_t id = 0; id < constants.grid.w; ++id) {
        if (cells[id].z != 1 || ownership[id] != id)
            continue;
        if (base[id].count > 24) {
            failure = "MGPCG invalid base row size";
            break;
        }
        previous[id][id] = base[id].diagonal;
        double ap = base[id].diagonal * solution(id);
        for (uint32_t j = 0; j < base[id].count; ++j) {
            const uint32_t other = base[id].neighbor[j];
            if (other >= constants.grid.w) {
                failure = "MGPCG invalid base index";
                break;
            }
            previous[id][other] += base[id].coefficient[j];
            ap += double(base[id].coefficient[j]) * solution(other);
        }
        if (!failure.empty())
            break;
        double rowSum = base[id].diagonal;
        for (uint32_t j = 0; j < base[id].count; ++j)
            rowSum += base[id].coefficient[j];
        if (!(base[id].volumeUnits > 0) || !std::isfinite(base[id].volumeUnits) ||
            !(base[id].boundaryDiagonal >= 0) || !std::isfinite(base[id].boundaryDiagonal) ||
            std::abs(rowSum - base[id].boundaryDiagonal) > 2e-5 * (1 + base[id].diagonal))
            failure = "MGPCG invalid boundary/control-volume contract";
        double target = base[id].rhs;
        if (cutProjection) {
            FluidCutPressureRow exact;
            memcpy(&exact, exactData + uint64_t(id) * sizeof(exact), sizeof(exact));
            target = exact.rhs;
            if (exact.diagonal >= 0) {
                ap = 0;
                auto p = coordinate(id, constants.grid);
                for (uint32_t a = 0; a < 3; ++a)
                    for (uint32_t s = 0; s < 2; ++s) {
                        const double w = exact.conductance[a * 2 + s];
                        if (!std::isfinite(w) || w < 0)
                            failure = "Invalid precise cut-cell conductance";
                        if (!w)
                            continue;
                        auto q = p;
                        const int c = int((&p.x)[a]) + (s ? 1 : -1);
                        double neighbor = 0;
                        if (c >= 0 && c < int((&constants.grid.x)[a])) {
                            (&q.x)[a] = uint32_t(c);
                            const auto n = index(q, constants.grid);
                            if (cells[n].z == 1)
                                neighbor = solution(n);
                        }
                        ap += w * (solution(id) - neighbor);
                    }
            }
        }
        const double divergence = std::abs(target - ap) * constants.parameters.z / base[id].volumeUnits;
        if (!std::isfinite(divergence))
            failure = "MGPCG nonfinite true residual";
        physicalMaximum = std::max(physicalMaximum, divergence);
    }
    // Independently build P^T A P in double precision, including all positive
    // T-junction sibling coefficients. Compare every row and symmetry.
    XMUINT4 childGrid = constants.grid;
    for (uint32_t level = 0; failure.empty() && level < constants.control.x; ++level) {
        const auto g = constants.levels[level];
        const uint32_t n = g.x * g.y * g.z, scale = level ? 2 : 4;
        Matrix expected(n);
        auto parent = [&](uint32_t id) {
            auto p = coordinate(id, childGrid);
            return index({p.x / scale, p.y / scale, p.z / scale}, g);
        };
        for (uint32_t child = 0; child < previous.size(); ++child)
            for (auto [other, value] : previous[child])
                expected[parent(child)][parent(other)] += value;
        for (uint32_t id = 0; id < n; ++id) {
            const auto &r = coarse[g.w + id];
            const auto p = coordinate(id, g);
            std::map<uint32_t, double> actual{{id, r.z.z}};
            for (uint32_t axis = 0; axis < 3; ++axis)
                for (uint32_t side = 0; side < 2; ++side) {
                    const float w = weight(r, axis * 2 + side);
                    if (!std::isfinite(w) || w < 0) {
                        failure = "MGPCG invalid coarse conductance";
                        break;
                    }
                    if (!w)
                        continue;
                    auto q = p;
                    const int v = int((&p.x)[axis]) + (side ? 1 : -1);
                    if (v < 0 || v >= int((&g.x)[axis])) {
                        failure = "MGPCG conductance leaves domain";
                        break;
                    }
                    (&q.x)[axis] = uint32_t(v);
                    const auto other = index(q, g);
                    if (w != weight(coarse[g.w + other], axis * 2 + 1 - side))
                        failure = "MGPCG nonsymmetric shared face";
                    actual[other] -= w;
                }
            for (auto [other, value] : expected[id]) {
                const double mismatch = std::abs(actual[other] - value) / (1 + std::abs(value));
                if (!std::isfinite(mismatch))
                    failure = "MGPCG nonfinite hierarchy";
                hierarchyError = std::max(hierarchyError, mismatch);
                actual.erase(other);
            }
            for (auto [other, value] : actual) {
                (void)other;
                hierarchyError = std::max(hierarchyError, std::abs(value));
            }
        }
        previous = std::move(expected);
        childGrid = g;
    }
    if (failure.empty()) {
        const auto g = constants.levels[constants.control.x - 1];
        const uint32_t n = g.x * g.y * g.z;
        double diagonal = 1;
        for (uint32_t i = 0; i < n; ++i)
            diagonal = std::max(diagonal, double(coarse[g.w + i].z.z));
        const double shift = diagonal * constants.parameters.w;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = 0; j < n; ++j) {
                double a = previous[i][j] + (i == j ? shift : 0), b = 0;
                for (uint32_t k = 0; k <= std::min(i, j); ++k)
                    b += double(lower[i * 64 + k]) * lower[j * 64 + k];
                if (!std::isfinite(b))
                    failure = "MGPCG nonfinite Cholesky factor";
                factorError = std::max(factorError, std::abs(a - b) / (1 + std::abs(a)));
            }
    }
    residualError =
        std::max(residualError, std::abs(physicalMaximum - maxDivergence) / (1 + physicalMaximum));
    snapshot.resource->Unmap(0, &written);
    if (hierarchyError > 2e-5 || factorError > 2e-5 || residualError > 2e-4)
        failure = "MGPCG independent hierarchy/factor/residual mismatch: " + std::to_string(hierarchyError) +
                  " / " + std::to_string(factorError) + " / " + std::to_string(residualError);
    if (!failure.empty())
        throw std::runtime_error(failure);
    validated = true;
}
} // namespace lab
