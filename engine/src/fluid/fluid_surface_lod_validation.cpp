#include "fluid_surface.h"
#include <cmath>
#include <vector>

namespace lab {
void FluidSurface::validateLod() {
    struct State {
        DirectX::XMFLOAT4 error, transition;
        DirectX::XMUINT4 state;
    };
    static_assert(sizeof(State) == 48);
    void *data;
    D3D12_RANGE range{0, size_t(lodSnapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(lodSnapshot.resource->Map(0, &range, &data), "Surface LOD independent snapshot");
    const auto *fieldValues = static_cast<const DirectX::XMFLOAT4 *>(data);
    const auto *reference = reinterpret_cast<const DirectX::XMFLOAT4 *>(static_cast<const char *>(data) +
                                                                        field.resource->GetDesc().Width);
    const auto *pages = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) +
                                                           2 * field.resource->GetDesc().Width);
    const auto *states = reinterpret_cast<const State *>(reinterpret_cast<const char *>(pages) +
                                                         map.resource->GetDesc().Width);
    std::vector<uint8_t> seen(brickGrid.w);
    std::vector<uint8_t> referenceSurface(brickGrid.w);
    lodAdmission.fill(0);
    std::string failure;
    uint32_t active = 0, coarse = 0;
    double phiError = 0, normalError = 0, boundaryError = 0, motionBoundaryError = 0;
    const auto index = [](uint32_t x, uint32_t y, uint32_t z) { return (z * 9 + y) * 9 + x; };
    // Independently construct a WORLD-node band from the unapproximated fine
    // reference, not the GPU classifier's cached masks or scalar magnitude.
    const int nx = int(brickGrid.x * 8 + 1), ny = int(brickGrid.y * 8 + 1), nz = int(brickGrid.z * 8 + 1);
    std::vector<uint8_t> band(size_t(nx) * ny * nz);
    auto bandIndex = [&](int x, int y, int z) { return (size_t(z) * ny + y) * nx + x; };
    uint32_t interfaceCells = 0;
    for (uint32_t id = 0; id < brickGrid.w; ++id) {
        const uint32_t slot = pages[id];
        if (slot == 0xffffffff || slot >= brickGrid.w)
            continue;
        int base[]{int(id % brickGrid.x * 8), int(id / brickGrid.x % brickGrid.y * 8),
                   int(id / (brickGrid.x * brickGrid.y) * 8)};
        for (uint32_t cell = 0; cell < 512; ++cell) {
            uint32_t p[]{cell % 8, cell / 8 % 8, cell / 64};
            float lo = 1e30f, hi = -1e30f;
            for (uint32_t k = 0; k < 8; ++k) {
                float value =
                    reference[slot * 729 + index(p[0] + (k & 1), p[1] + ((k >> 1) & 1), p[2] + (k >> 2))].x;
                lo = std::min(lo, value);
                hi = std::max(hi, value);
            }
            if (lo > 0 || hi < 0)
                continue;
            ++interfaceCells;
            referenceSurface[id] = 1;
            for (int dz = -1; dz <= 2; ++dz)
                for (int dy = -1; dy <= 2; ++dy)
                    for (int dx = -1; dx <= 2; ++dx) {
                        int x = base[0] + p[0] + dx, y = base[1] + p[1] + dy, z = base[2] + p[2] + dz;
                        if (x >= 0 && y >= 0 && z >= 0 && x < nx && y < ny && z < nz)
                            band[bandIndex(x, y, z)] = 1;
                    }
        }
    }
    auto sampleNode = [&](const DirectX::XMFLOAT4 *values, int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= int(brickGrid.x * 8) || y >= int(brickGrid.y * 8) ||
            z >= int(brickGrid.z * 8))
            return minimumSpacing.w * 3;
        uint32_t brick = ((z / 8) * brickGrid.y + y / 8) * brickGrid.x + x / 8;
        uint32_t slot = pages[brick];
        if (slot == 0xffffffff)
            return minimumSpacing.w * 3;
        if (slot >= brickGrid.w) {
            failure = "Invalid surface page while reconstructing a reference normal";
            return 0.f;
        }
        return values[slot * 729 + index(x % 8, y % 8, z % 8)].x;
    };
    for (uint32_t id = 0; id < brickGrid.w; ++id) {
        uint32_t slot = pages[id];
        if (slot == 0xffffffff)
            continue;
        if (slot >= brickGrid.w || seen[slot]++) {
            failure = "Invalid or duplicate adaptive surface page";
            break;
        }
        ++active;
        const State &s = states[id];
        if (referenceSurface[id]) {
            ++lodAdmission[0];
            lodAdmission[1] += s.transition.w != 0;
            lodAdmission[2] += s.error.x >= 1e5f && s.error.y >= 1e5f;
            lodAdmission[3] += s.error.x > lodPhiTolerance * s.transition.z;
            lodAdmission[4] += s.error.y > lodNormalTolerance * s.transition.z;
            const bool accurate = s.error.x < lodPhiTolerance * s.transition.z * .75f &&
                                  s.error.y < lodNormalTolerance * s.transition.z * .75f;
            lodAdmission[5] += accurate;
            lodAdmission[6] += accurate && s.transition.w == 0 && s.error.w == 0;
            lodAdmission[7] += s.error.w > 0 && s.error.w < 1;
        }
        if (!s.state.w || !std::isfinite(s.error.w) || s.error.w < 0 || s.error.w > 1)
            failure = "Invalid actual surface LOD state";
        coarse += s.error.w == 1;
        uint32_t b[]{id % brickGrid.x, id / brickGrid.x % brickGrid.y, id / (brickGrid.x * brickGrid.y)};
        for (uint32_t z = 0; z <= 8; ++z)
            for (uint32_t y = 0; y <= 8; ++y)
                for (uint32_t x = 0; x <= 8; ++x) {
                    uint32_t n = slot * 729 + index(x, y, z);
                    auto f = fieldValues[n], r = reference[n];
                    for (uint32_t a = 0; a < 4; ++a)
                        if (!std::isfinite((&f.x)[a]) || !std::isfinite((&r.x)[a]))
                            failure = "Nonfinite adaptive surface/reference/motion guide";
                    double difference = std::abs(double(f.x) - r.x) / minimumSpacing.w;
                    if (band[bandIndex(int(b[0] * 8 + x), int(b[1] * 8 + y), int(b[2] * 8 + z))]) {
                        phiError = std::max(phiError, difference);
                        int world[]{int(b[0] * 8 + x), int(b[1] * 8 + y), int(b[2] * 8 + z)};
                        double g[2][3]{};
                        for (uint32_t a = 0; a < 3; ++a) {
                            int lo[]{world[0], world[1], world[2]}, hi[]{world[0], world[1], world[2]};
                            lo[a]--;
                            hi[a]++;
                            g[0][a] = double(sampleNode(fieldValues, hi[0], hi[1], hi[2])) -
                                      sampleNode(fieldValues, lo[0], lo[1], lo[2]);
                            g[1][a] = double(sampleNode(reference, hi[0], hi[1], hi[2])) -
                                      sampleNode(reference, lo[0], lo[1], lo[2]);
                        }
                        double lengths[2]{};
                        for (uint32_t a = 0; a < 3; ++a)
                            for (uint32_t i = 0; i < 2; ++i)
                                lengths[i] += g[i][a] * g[i][a];
                        if (lengths[0] > 1e-12 && lengths[1] > 1e-12) {
                            double error = 0;
                            for (uint32_t a = 0; a < 3; ++a) {
                                double d = g[0][a] / std::sqrt(lengths[0]) - g[1][a] / std::sqrt(lengths[1]);
                                error += d * d;
                            }
                            normalError = std::max(normalError, std::sqrt(error));
                        }
                    }
                    if ((f.x < 0) != (r.x < 0) &&
                        std::max(std::abs(f.x), std::abs(r.x)) > minimumSpacing.w * 1e-6f)
                        failure = "Surface LOD changed a fine-reference nodal sign event";
                    uint32_t p[]{x, y, z};
                    for (uint32_t a = 0; a < 3; ++a) {
                        if (p[a] != 8 || b[a] + 1 >= (&brickGrid.x)[a])
                            continue;
                        uint32_t other[]{b[0], b[1], b[2]};
                        other[a]++;
                        uint32_t neighbor =
                            pages[(other[2] * brickGrid.y + other[1]) * brickGrid.x + other[0]];
                        if (neighbor == 0xffffffff || neighbor >= brickGrid.w)
                            continue;
                        uint32_t q[]{x, y, z};
                        q[a] = 0;
                        auto v = fieldValues[neighbor * 729 + index(q[0], q[1], q[2])];
                        // Scalar continuity is exact. Material guide interpolation
                        // is also shared, apart from fp32 gradient reconstruction.
                        boundaryError = std::max(boundaryError, std::abs(double(v.x) - f.x));
                        for (uint32_t axis = 1; axis < 4; ++axis)
                            motionBoundaryError =
                                std::max(motionBoundaryError, std::abs(double((&v.x)[axis]) - (&f.x)[axis]));
                    }
                }
    }
    lodSnapshot.resource->Unmap(0, &written);
    lodMaxPhiError = std::max(lodMaxPhiError, phiError);
    lodMaxNormalError = std::max(lodMaxNormalError, normalError);
    lodBoundaryError = std::max(lodBoundaryError, boundaryError);
    lodMotionBoundaryError = std::max(lodMotionBoundaryError, motionBoundaryError);
    uint32_t coarseNodes = 1;
    for (uint32_t axis = 0; axis < 3; ++axis)
        coarseNodes *= (lodCoarseAxes & (1u << axis)) ? 5 : 9;
    if (active != activeBricks || coarse != lodCounts[14] || lodCounts[0] + lodCounts[1] != active ||
        lodCounts[4] != lodCounts[0] * 729 || lodCounts[5] != lodCounts[1] * coarseNodes ||
        lodCounts[8] > lodCounts[1] || lodCounts[9] != lodCounts[8] * 729)
        failure = "Adaptive surface execution/count mismatch";
    if (interfaceCells != lodCounts[16])
        failure = "Cached fine interface masks disagree with current independent reconstruction";
    if (boundaryError > 2e-7)
        failure = "Adaptive surface developed a shared-boundary crack";
    if (motionBoundaryError > 2e-7)
        failure = "Adaptive surface developed a shared-boundary motion seam";
    if (phiError > lodPhiTolerance * 1.01 + 1e-5 || normalError > lodNormalTolerance * 1.01 + 1e-4)
        failure = "Adaptive surface exceeded the same-state reconstruction error budget";
    if (!failure.empty())
        throw std::runtime_error(failure + " (phi/h " + std::to_string(phiError) + ", normal " +
                                 std::to_string(normalError) + ", boundary " + std::to_string(boundaryError) +
                                 ", motion boundary " + std::to_string(motionBoundaryError) + ")");
    ++lodAudits;
}
} // namespace lab
