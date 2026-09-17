#include "fluid_work.h"
#include <cmath>
#include <vector>

namespace lab {
void FluidWork::validateFaces() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(faceSnapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(faceSnapshot.resource->Map(0, &range, &data), "Fluid work gather coverage snapshot");
    auto words = static_cast<const uint32_t *>(data);
    auto counts =
        reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) + work.resource->GetDesc().Width);
    auto quanta = counts + grid.w;
    auto faces = reinterpret_cast<const DirectX::XMFLOAT4 *>(counts + grid.w * (hasInterior ? 2 : 1));
    auto reference = faces + faceCount;
    std::vector<uint8_t> expected(faceTiles), seen(faceTiles);
    const uint32_t tilesPerAxis = faceTiles / 3, faceStride = faceCount / 3;
    std::string failure;
    // Independent inverse-support query: enumerate each face tile's complete
    // input-bin box, rather than repeating the shader's occupied-bin scatter.
    for (uint32_t tile = 0; tile < faceTiles; ++tile) {
        uint32_t axis = tile / tilesPerAxis, k = tile % tilesPerAxis;
        int base[]{int(k % faceGrid.x) * 8, int(k / faceGrid.x % faceGrid.y) * 4,
                   int(k / (faceGrid.x * faceGrid.y)) * 4};
        int size[]{8, 4, 4}, extent[]{int(grid.x), int(grid.y), int(grid.z)};
        extent[axis]++;
        if (base[0] >= extent[0] || base[1] >= extent[1] || base[2] >= extent[2])
            continue;
        int lo[3], hi[3];
        for (uint32_t a = 0; a < 3; ++a) {
            const double offset = a == axis ? 0 : .5;
            lo[a] = std::max(0, int(std::floor(base[a] + offset - 1.5)));
            hi[a] =
                std::min(int((&grid.x)[a]) - 1,
                         int(std::ceil(std::min(base[a] + size[a] - 1, extent[a] - 1) + offset + 1.5)) - 1);
        }
        for (int z = lo[2]; z <= hi[2] && !expected[tile]; ++z)
            for (int y = lo[1]; y <= hi[1] && !expected[tile]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x) {
                    uint32_t id = (z * grid.y + y) * grid.x + x;
                    if (counts[id] || (hasInterior && quanta[id])) {
                        expected[tile] = 1;
                        break;
                    }
                }
    }
    if (words[0] > faceTiles)
        failure = "Face work count exceeds capacity";
    else
        for (uint32_t i = 0; i < words[0]; ++i) {
            uint32_t tile = words[16 + faceTiles + i];
            if (tile >= faceTiles || seen[tile]++) {
                failure = "Duplicate/invalid compact face tile";
                break;
            }
        }
    for (uint32_t tile = 0; tile < faceTiles; ++tile)
        if (words[16 + tile] != expected[tile] || seen[tile] != expected[tile])
            failure = "Compact MAC work omitted or fabricated kernel coverage";
    for (uint32_t id = 0; id < faceCount; ++id) {
        uint32_t axis = id / faceStride, k = id % faceStride;
        uint32_t c[]{k % (grid.x + 1), k / (grid.x + 1) % (grid.y + 1), k / ((grid.x + 1) * (grid.y + 1))};
        uint32_t extent[]{grid.x, grid.y, grid.z};
        extent[axis]++;
        uint32_t tile = axis * tilesPerAxis + ((c[2] / 4 * faceGrid.y + c[1] / 4) * faceGrid.x + c[0] / 8);
        bool valid = c[0] < extent[0] && c[1] < extent[1] && c[2] < extent[2];
        const auto f = faces[id];
        for (uint32_t a = 0; a < 4; ++a) {
            if (!std::isfinite((&f.x)[a]))
                failure = "Nonfinite sparse P2G output";
            double difference = std::abs(double((&f.x)[a]) - (&reference[id].x)[a]);
            maxFaceDifference = std::max(maxFaceDifference, difference);
            if (!std::isfinite(difference) || difference != 0)
                failure = "Sparse P2G differs from same-state dense reference";
        }
        if ((!valid || !expected[tile]) && (f.x != 0 || f.y != 0 || f.z != 0 || f.w != 0))
            failure = "Stale face outside active kernel support";
        if (f.z < 0 || f.w != 0 || f.x != f.y)
            failure = "Sparse P2G changed pre-force velocity/weight contract";
    }
    faceSnapshot.resource->Unmap(0, &written);
    if (!failure.empty())
        throw std::runtime_error(failure);
    faceValidated = true;
}
void FluidWork::validateDensity() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(densitySnapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(densitySnapshot.resource->Map(0, &range, &data), "Fluid work density coverage snapshot");
    auto words = static_cast<const uint32_t *>(data);
    auto stencil = reinterpret_cast<const DirectX::XMFLOAT4 *>(static_cast<const char *>(data) +
                                                               work.resource->GetDesc().Width);
    auto arguments = reinterpret_cast<const uint32_t *>(stencil + grid.w);
    auto pressure = reinterpret_cast<const float *>(arguments + 64);
    auto reference = pressure + grid.w;
    const bool enabled = !densityRepair || arguments[24] != 0;
    std::vector<uint8_t> expected(densityTiles), seen(densityTiles);
    for (uint32_t id = 0; id < grid.w; ++id)
        if (enabled && stencil[id].y > 0) {
            uint32_t x = id % grid.x, y = id / grid.x % grid.y, z = id / (grid.x * grid.y);
            expected[(z / 4 * densityGrid.y + y / 4) * densityGrid.x + x / 8] = 1;
        }
    std::string failure;
    const uint32_t list = 16 + 2 * faceTiles, flags = list + densityTiles;
    if (words[1] > densityTiles)
        failure = "Density work count exceeds capacity";
    else
        for (uint32_t i = 0; i < words[1]; ++i) {
            uint32_t tile = words[list + i];
            if (tile >= densityTiles || seen[tile]++) {
                failure = "Duplicate/invalid compact density tile";
                break;
            }
        }
    for (uint32_t tile = 0; tile < densityTiles; ++tile)
        if (seen[tile] != expected[tile] || words[flags + tile] != expected[tile])
            failure = "Density work broke globally coupled coverage";
    for (uint32_t id = 0; id < grid.w; ++id) {
        double difference = std::abs(double(pressure[id]) - reference[id]);
        maxDensityDifference = std::max(maxDensityDifference, difference);
        if (!std::isfinite(difference) || difference != 0)
            failure = "Compact density solve differs from same-state scalar reference";
    }
    densitySnapshot.resource->Unmap(0, &written);
    if (!failure.empty())
        throw std::runtime_error(failure);
    densityValidated = true;
}
} // namespace lab
