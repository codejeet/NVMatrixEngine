#include "fluid_bulk_pressure.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "fluid_numeric.h"

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace {
void copySupport(ID3D12GraphicsCommandList *cmd, ID3D12Resource *target, uint64_t offset,
                 ID3D12Resource *source, uint64_t bytes) {
    gpu::transition(cmd, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(target, offset, source, 0, bytes);
    gpu::transition(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace
FluidBulkPressure::FluidBulkPressure(ID3D12Device *device, const std::filesystem::path &folder, XMUINT4 fine,
                                     float particleVolume, bool supportClosing, bool supportShrinking,
                                     bool usePrecise) {
    precise = usePrecise;
    closingSupport = supportClosing;
    shrinkingSupport = supportShrinking;
    constants.fine = fine;
    constants.coarse = {(fine.x + 1) / 2, (fine.y + 1) / 2, (fine.z + 1) / 2, 0};
    constants.coarse.w = constants.coarse.x * constants.coarse.y * constants.coarse.z;
    constants.parameters = {particleVolume, 1e-6f, 0, 0};
    faceCount = (fine.x + 1) * (fine.y + 1) * (fine.z + 1) * 3;
    support = gpu::buffer(device, uint64_t(fine.w) * 4, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          L"Bulk pressure / filled-volume capillary support");
    counters = gpu::buffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Bulk pressure / coverage counters");
    readback = gpu::buffer(device, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Bulk pressure / existing-fence metrics");
    D3D12_ROOT_PARAMETER p[9]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, sizeof(Constants) / 4};
    for (uint32_t i = 1; i < 9; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC r{9, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Bulk pressure root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Bulk pressure root");
    const char *names[]{"BulkPressureClear", "BulkPressureCells", "BulkPressureFaces"};
    for (uint32_t i = 0; i < pipelines.size(); ++i) {
        const auto code =
            gpu::bytes(folder / "shaders" / (std::string(names[i]) + (precise ? "-precise.dxil" : ".dxil")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.Get();
        state.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 32;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Bulk pressure timestamps");
}
void FluidBulkPressure::beginFrame(ID3D12GraphicsCommandList *cmd, bool validate) {
    records = 0;
    validateFrame = validate;
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootUnorderedAccessView(6, counters.resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(pipelines[0].Get());
    cmd->Dispatch(1, 1, 1);
    gpu::uav(cmd);
}
void FluidBulkPressure::record(ID3D12GraphicsCommandList *cmd, const FluidBulkGpuView &bulk,
                               const FluidCutCellGpuView &cut, ID3D12Resource *cells, ID3D12Resource *faces) {
    if (bulk.preciseInventory != precise || cut.preciseCapacity != precise)
        throw std::runtime_error("Bulk pressure numeric format mismatch");
    if (records >= 16 || !bulk.preciseFaceRates || !cut.pressureArea || !bulk.pendingSources)
        throw std::runtime_error(
            "Bulk pressure requires projected, capacity-admitted liquid and bounded substeps");
    if (memcmp(&bulk.grid, &constants.coarse, sizeof(XMUINT4)) ||
        memcmp(&cut.fine, &constants.fine, sizeof(XMUINT4)))
        throw std::runtime_error("Bulk pressure grid mismatch");
    constants.control = {cut.swept ? 1u : 0u, cut.timeCentered ? 1u : 0u,
                         (closingSupport ? 1u : 0u) | (shrinkingSupport ? 2u : 0u), 0};
    constants.parameters.z = cut.minimumSpacing.w * cut.minimumSpacing.w * cut.minimumSpacing.w;
    const uint64_t n = constants.coarse.w, f = constants.fine.w, facesN = faceCount;
    const uint64_t sizes[]{n * (precise ? 32 : 16),
                           f * 8,
                           n * (precise ? 16 : 8),
                           f * 16,
                           facesN * 16,
                           f * 16,
                           facesN * 16,
                           facesN * 4,
                           f * 4};
    if (validateFrame && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(cells->GetDevice(IID_PPV_ARGS(&device)), "Bulk pressure audit device");
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < offsets.size(); ++i) {
            offsets[i] = bytes;
            bytes += sizes[i];
        }
        snapshot = gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               L"Bulk pressure / independent final-substep coverage audit");
    }
    ID3D12Resource *resources[]{
        bulk.inventory, cut.fineVolume,          cut.coarseVolume, cells,
        faces,          counters.resource.Get(), cut.pressureArea, support.resource.Get()};
    if (validateFrame) {
        for (uint32_t i = 0; i < 5; ++i)
            copySupport(cmd, snapshot.resource.Get(), offsets[i], resources[i], sizes[i]);
        copySupport(cmd, snapshot.resource.Get(), offsets[7], cut.pressureArea, sizes[7]);
    }
    gpu::Event event(cmd, L"Fluid / filled Eulerian interior pressure and velocity coverage");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    for (uint32_t i = 0; i < 8; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, resources[i]->GetGPUVirtualAddress());
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, records * 2);
    cmd->SetPipelineState(pipelines[1].Get());
    cmd->Dispatch((constants.fine.w + 127) / 128, 1, 1);
    gpu::uav(cmd);
    cmd->SetPipelineState(pipelines[2].Get());
    cmd->Dispatch((faceCount + 127) / 128, 1, 1);
    gpu::uav(cmd);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, records * 2 + 1);
    if (validateFrame) {
        copySupport(cmd, snapshot.resource.Get(), offsets[5], cells, sizes[5]);
        copySupport(cmd, snapshot.resource.Get(), offsets[6], faces, sizes[6]);
        copySupport(cmd, snapshot.resource.Get(), offsets[8], support.resource.Get(), sizes[8]);
    }
    ++records;
    ++steps;
}
void FluidBulkPressure::finishFrame(ID3D12GraphicsCommandList *cmd) {
    if (records)
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, records * 2,
                              readback.resource.Get(), 0);
    copySupport(cmd, readback.resource.Get(), 512, counters.resource.Get(), sizeof(metrics));
}
void FluidBulkPressure::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 512 + sizeof(metrics)}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Bulk pressure metrics");
    const auto ticks = static_cast<const uint64_t *>(data);
    gpuMs = 0;
    for (uint32_t i = 0; i < records; ++i)
        gpuMs += double(ticks[2 * i + 1] - ticks[2 * i]) * 1000 / frequency;
    memcpy(metrics.data(), static_cast<const char *>(data) + 512, sizeof(metrics));
    readback.resource->Unmap(0, &written);
    ++frames;
    totalMs += gpuMs;
    addedCells += metrics[0];
    filledFaces += metrics[1];
    closingCells += metrics[3];
    partialClosingCells += metrics[4];
    partialShrinkingCells += metrics[5];
    if (metrics[7])
        throw std::runtime_error("Nonfinite bulk pressure velocity coverage");
    if (validateFrame && records)
        validateSnapshot();
    else if (validateFrame) {
        if (std::any_of(metrics.begin(), metrics.end(), [](uint32_t v) { return v != 0; }))
            throw std::runtime_error("Idle bulk pressure frame unexpectedly recorded coverage work");
        ++auditedIdleFrames;
    }
}
void FluidBulkPressure::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Bulk pressure independent coverage snapshot");
    const auto bytes = static_cast<const char *>(data);
    const auto inventory = readFluidNumbers<FluidDouble4>(bytes + offsets[0], constants.coarse.w, precise);
    const auto fine = reinterpret_cast<const XMFLOAT2 *>(bytes + offsets[1]);
    const auto coarse = readFluidNumbers<FluidDouble2>(bytes + offsets[2], constants.coarse.w, precise);
    const auto beforeCells = reinterpret_cast<const XMFLOAT4 *>(bytes + offsets[3]);
    const auto beforeFaces = reinterpret_cast<const XMFLOAT4 *>(bytes + offsets[4]);
    const auto afterCells = reinterpret_cast<const XMFLOAT4 *>(bytes + offsets[5]);
    const auto afterFaces = reinterpret_cast<const XMFLOAT4 *>(bytes + offsets[6]);
    const auto area = reinterpret_cast<const float *>(bytes + offsets[7]);
    const auto colourSupport = reinterpret_cast<const float *>(bytes + offsets[8]);
    using Coord = std::array<int, 3>;
    const auto g = constants.fine, c = constants.coarse;
    auto inside = [&](Coord p) {
        return p[0] >= 0 && p[1] >= 0 && p[2] >= 0 && p[0] < int(g.x) && p[1] < int(g.y) && p[2] < int(g.z);
    };
    auto index = [&](Coord p) { return (p[2] * g.y + p[1]) * g.x + p[0]; };
    auto parent = [&](Coord p) { return (p[2] / 2 * c.y + p[1] / 2) * c.x + p[0] / 2; };
    auto coord = [&](uint32_t id) {
        return Coord{int(id % g.x), int(id / g.x % g.y), int(id / (g.x * g.y))};
    };
    auto volume = [&](Coord p) {
        auto v = fine[index(p)];
        return constants.control.y ? .5 * (double(v.x) + v.y) : double(v.x);
    };
    auto fraction = [&](Coord p) {
        uint32_t id = parent(p);
        double cap = constants.control.x ? coarse[id].y : coarse[id].x;
        if (cap <= 0)
            return 0.;
        const float fullFraction = 1.f - constants.parameters.y;
        // Decode snapshots at full precision, but reproduce the actual
        // storage-mode threshold arithmetic used by the shader.
        const double fullThreshold = precise ? cap * fullFraction : double(float(cap) * fullFraction);
        if (inventory[id].w >= fullThreshold)
            return 1.;
        // A disappearing mixture needs an evacuation pressure row even when
        // its liquid phase was not full. Optical/capillary occupancy still
        // contains only its actual liquid fraction, not an invented full cell.
        const bool evacuate =
            ((constants.control.z & 1) && coarse[id].x == 0) ||
            ((constants.control.z & 2) && coarse[id].x < cap && inventory[id].w > coarse[id].x);
        if (constants.control.x && evacuate && inventory[id].w > 0)
            return double(inventory[id].w) / cap;
        return 0.;
    };
    bool bad = false;
    for (uint32_t id = 0; id < g.w; ++id) {
        auto expected = beforeCells[id];
        const double expectedSupport = expected.z != 2 && volume(coord(id)) > 0
                                           ? volume(coord(id)) * fraction(coord(id)) / constants.parameters.z
                                           : 0;
        bad |= !std::isfinite(colourSupport[id]) || std::abs(colourSupport[id] - expectedSupport) > 2e-6;
        if (expected.z == 0 && volume(coord(id)) > 0 && fraction(coord(id)) > 0)
            expected.z = 1;
        bad |= memcmp(&expected, &afterCells[id], sizeof(expected)) != 0;
    }
    const uint32_t stride = faceCount / 3;
    for (uint32_t id = 0; id < faceCount; ++id) {
        uint32_t axis = id / stride, k = id % stride;
        Coord p{int(k % (g.x + 1)), int(k / (g.x + 1) % (g.y + 1)), int(k / ((g.x + 1) * (g.y + 1)))},
            left = p;
        --left[axis];
        auto expected = beforeFaces[id];
        bool changed = false;
        if (inside(left) && inside(p) && expected.z <= 1e-8f && area[id] > 0 && volume(left) > 0 &&
            volume(p) > 0) {
            double momentum = 0, v = 0;
            for (const auto cell : {left, p})
                if (afterCells[index(cell)].z == 1 && fraction(cell) > 0) {
                    const auto q = inventory[parent(cell)];
                    double w = .5 * volume(cell) * fraction(cell);
                    momentum += w * (&q.x)[axis] / q.w;
                    v += w;
                }
            if (v > 0) {
                const double u = momentum / v;
                const auto a = afterFaces[id];
                velocityError = std::max(velocityError, std::abs(a.x - u) / (1 + std::abs(u)));
                massWeightError = std::max(massWeightError, std::abs(a.z - v / constants.parameters.x) /
                                                                (1 + v / constants.parameters.x));
                bad |= !std::isfinite(a.x) || !std::isfinite(a.z) || a.x != a.y || a.w != 0;
                changed = true;
            }
        }
        if (!changed)
            bad |= memcmp(&expected, &afterFaces[id], sizeof(expected)) != 0;
    }
    snapshot.resource->Unmap(0, &written);
    if (bad || velocityError > 2e-6 || massWeightError > 2e-6)
        throw std::runtime_error(
            "Bulk pressure coverage differs from independent classification/velocity audit");
    validated = true;
    ++auditedFrames;
}
void FluidBulkPressure::report(std::ostream &out) const {
    out << "{\"role\":\"filled bulk pressure support; not particle-free mass ownership\",\"projectionCalls\":"
        << steps << ",\"lastFrameProjectionCalls\":" << records << ",\"addedCellUpdates\":" << addedCells
        << ",\"filledFaceUpdates\":" << filledFaces << ",\"closingCellUpdates\":" << closingCells
        << ",\"partialClosingCellUpdates\":" << partialClosingCells
        << ",\"partialClosingSupport\":" << (closingSupport ? "true" : "false")
        << ",\"shrinkingCapacitySupport\":" << (shrinkingSupport ? "true" : "false")
        << ",\"partialShrinkingCellUpdates\":" << partialShrinkingCells
        << ",\"lastAddedCells\":" << metrics[0] << ",\"lastFilledFaces\":" << metrics[1]
        << ",\"lastFilledCellVisits\":" << metrics[2] << ",\"invalid\":" << metrics[7]
        << ",\"lastFrameMs\":" << gpuMs << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"velocityReferenceError\":" << velocityError << ",\"weightReferenceError\":" << massWeightError
        << ",\"auditedFrames\":" << auditedFrames << ",\"auditedIdleFrames\":" << auditedIdleFrames
        << ",\"validated\":" << (validated ? "true" : "false") << "}";
}
} // namespace lab
