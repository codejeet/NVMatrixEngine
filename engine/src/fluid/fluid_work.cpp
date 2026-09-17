#include "fluid_work.h"
#include <cstring>
#include <cmath>

namespace lab {
using namespace DirectX;
FluidWork::FluidWork(ID3D12Device *device, const std::filesystem::path &folder, ID3D12RootSignature *root,
                     XMUINT4 g, bool interior, bool ownedParticles)
    : grid(g), hasInterior(interior) {
    faceGrid = {(g.x + 8) / 8, (g.y + 4) / 4, (g.z + 4) / 4};
    densityGrid = {(g.x + 7) / 8, (g.y + 3) / 4, (g.z + 3) / 4};
    faceTiles = 3 * faceGrid.x * faceGrid.y * faceGrid.z;
    densityTiles = densityGrid.x * densityGrid.y * densityGrid.z;
    faceCount = 3 * (g.x + 1) * (g.y + 1) * (g.z + 1);
    wordCount = 16 + 2 * faceTiles + 2 * densityTiles;
    work = gpu::buffer(device, uint64_t(wordCount) * 4, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       L"Fluid work / face flags and compact face-density tiles");
    args = gpu::buffer(device, 64, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"Fluid work / indirect tile dispatches");
    readback =
        gpu::buffer(device, maxIntervals * 16 + 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid work / deferred stage timings and counts");
    const char *names[]{
        "WorkReset",          "WorkBegin",        "WorkClearFaces",
        "WorkMarkFaces",      "WorkCompactFaces", "WorkPrepareFaces",
        "WorkClearDensity",   "WorkDensity",      "WorkDensityRepair",
        "WorkPrepareDensity", "WorkZeroPressure", ownedParticles ? "FluidP2GAuthority" : "FluidP2G",
        "FluidDensityJacobi", "WorkCompareFaces", "WorkCompareDensity"};
    for (uint32_t i = 0; i < PassCount; ++i) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root;
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &argument, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch)),
               "Fluid work dispatch");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 2 * maxIntervals;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Fluid work timestamps");
}
void FluidWork::pass(ID3D12GraphicsCommandList *cmd, Pass p, uint32_t groups) {
    cmd->SetPipelineState(pipelines[p].Get());
    cmd->Dispatch(groups, 1, 1);
    gpu::uav(cmd);
}
void FluidWork::beginFrame(ID3D12GraphicsCommandList *cmd, bool reset, bool validate) {
    if (indirect)
        throw std::runtime_error("Fluid work left an indirect scope open");
    intervalCount = 0;
    audit = validate;
    faceCaptured = densityCaptured = false;
    if (!initialized || reset) {
        pass(cmd, Reset, (wordCount + 255) / 256);
        initialized = true;
    } else
        pass(cmd, Begin, 1);
}
uint32_t FluidWork::begin(ID3D12GraphicsCommandList *cmd, Phase stage) {
    if (intervalCount == maxIntervals)
        throw std::runtime_error("Fluid work timestamp capacity exceeded");
    uint32_t id = intervalCount++;
    intervals[id] = stage;
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * id);
    return id;
}
void FluidWork::end(ID3D12GraphicsCommandList *cmd, uint32_t id) {
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * id + 1);
}
void FluidWork::prepareFaces(ID3D12GraphicsCommandList *cmd) {
    gpu::Event event(cmd, L"Fluid / compact quadratic-support MAC tiles");
    uint32_t stamp = begin(cmd, Classification);
    pass(cmd, ClearFaces, (faceCount + 255) / 256);
    pass(cmd, MarkFaces, (grid.w + 127) / 128);
    pass(cmd, CompactFaces, (faceTiles + 127) / 128);
    pass(cmd, PrepareFaces, 1);
    end(cmd, stamp);
}
void FluidWork::prepareDensity(ID3D12GraphicsCommandList *cmd, bool repair) {
    gpu::Event event(cmd, L"Fluid / compact globally coupled density tiles");
    uint32_t stamp = begin(cmd, DensityClassification);
    densitySeen = true;
    pass(cmd, ClearDensity, (densityTiles + 127) / 128);
    pass(cmd, repair ? DensityRepair : Density, (densityTiles + 127) / 128);
    pass(cmd, PrepareDensity, 1);
    end(cmd, stamp);
}
void FluidWork::execute(ID3D12GraphicsCommandList *cmd, ID3D12PipelineState *state, bool density) {
    if (!indirect) {
        gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        indirect = true;
    }
    cmd->SetPipelineState(state);
    cmd->ExecuteIndirect(dispatch.Get(), 1, args.resource.Get(), density ? 12 : 0, nullptr, 0);
    gpu::uav(cmd);
}
void FluidWork::finish(ID3D12GraphicsCommandList *cmd) {
    if (indirect) {
        gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        indirect = false;
    }
}
void FluidWork::copy(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, uint64_t &offset,
                     ID3D12Resource *src, uint64_t bytes) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, offset, src, 0, bytes);
    offset += bytes;
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void FluidWork::auditFaces(ID3D12GraphicsCommandList *cmd, ID3D12Resource *counts, ID3D12Resource *quanta,
                           ID3D12Resource *faces) {
    if (!audit)
        return;
    if (!faceSnapshot.resource) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        gpu::check(cmd->GetDevice(IID_PPV_ARGS(&device)), "Work audit device");
        faceSnapshot =
            gpu::buffer(device.Get(),
                        work.resource->GetDesc().Width + uint64_t(grid.w) * (hasInterior ? 8 : 4) +
                            uint64_t(faceCount) * 32,
                        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                        L"Fluid work / exact gather coverage audit");
        referenceFaces =
            gpu::buffer(device.Get(), uint64_t(faceCount) * 16, D3D12_HEAP_TYPE_DEFAULT,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        L"Fluid work / same-state dense P2G reference");
    }
    uint64_t offset = 0;
    copy(cmd, faceSnapshot.resource.Get(), offset, work.resource.Get(), work.resource->GetDesc().Width);
    copy(cmd, faceSnapshot.resource.Get(), offset, counts, uint64_t(grid.w) * 4);
    if (hasInterior)
        copy(cmd, faceSnapshot.resource.Get(), offset, quanta, uint64_t(grid.w) * 4);
    copy(cmd, faceSnapshot.resource.Get(), offset, faces, uint64_t(faceCount) * 16);
    cmd->SetComputeRootUnorderedAccessView(8, referenceFaces.resource->GetGPUVirtualAddress());
    pass(cmd, ReferenceP2G, (faceCount + 127) / 128);
    copy(cmd, faceSnapshot.resource.Get(), offset, referenceFaces.resource.Get(), uint64_t(faceCount) * 16);
    cmd->SetComputeRootUnorderedAccessView(8, faces->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(24, referenceFaces.resource->GetGPUVirtualAddress());
    pass(cmd, CompareFaces, (faceCount + 255) / 256);
    cmd->SetComputeRootUnorderedAccessView(24, args.resource->GetGPUVirtualAddress());
    faceCaptured = true;
}
void FluidWork::auditDensity(ID3D12GraphicsCommandList *cmd, ID3D12Resource *stencil,
                             ID3D12Resource *repairArgs, bool repair) {
    if (!audit)
        return;
    if (!densitySnapshot.resource) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        gpu::check(cmd->GetDevice(IID_PPV_ARGS(&device)), "Density work audit device");
        densitySnapshot =
            gpu::buffer(device.Get(), work.resource->GetDesc().Width + uint64_t(grid.w) * 24 + 256,
                        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                        L"Fluid work / exact density tile audit");
        for (auto &pressure : referencePressure)
            pressure =
                gpu::buffer(device.Get(), uint64_t(grid.w) * 4, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            L"Fluid work / same-state scalar density reference");
    }
    uint64_t offset = 0;
    copy(cmd, densitySnapshot.resource.Get(), offset, work.resource.Get(), work.resource->GetDesc().Width);
    copy(cmd, densitySnapshot.resource.Get(), offset, stencil, uint64_t(grid.w) * 16);
    copy(cmd, densitySnapshot.resource.Get(), offset, repairArgs, 256);
    densityCaptured = true;
    densityRepair = repair;
}
void FluidWork::auditDensityResult(ID3D12GraphicsCommandList *cmd, ID3D12Resource *result,
                                   uint32_t iterations, ID3D12Resource *repairArgs) {
    if (!audit)
        return;
    // Validation only. Original scalar Jacobi sees the exact same assembled
    // global operator, not a second simulation with nondeterministic slot IDs.
    // Caller restores both pressure root views before displacement.
    for (uint32_t i = 0; i < 2; ++i)
        cmd->SetComputeRootUnorderedAccessView(9 + i, referencePressure[i].resource->GetGPUVirtualAddress());
    pass(cmd, ZeroPressure, (grid.w + 255) / 256);
    for (uint32_t i = 0; i < iterations; ++i) {
        cmd->SetComputeRootUnorderedAccessView(9, referencePressure[i % 2].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(10,
                                               referencePressure[1 - i % 2].resource->GetGPUVirtualAddress());
        if (repairArgs) {
            // Existing repair arguments are already in INDIRECT_ARGUMENT state.
            cmd->SetPipelineState(pipelines[ReferenceDensity].Get());
            cmd->ExecuteIndirect(dispatch.Get(), 1, repairArgs, 6 * 12, nullptr, 0);
            gpu::uav(cmd);
        } else
            pass(cmd, ReferenceDensity, (grid.w + 255) / 256);
    }
    uint64_t offset = work.resource->GetDesc().Width + uint64_t(grid.w) * 16 + 256;
    copy(cmd, densitySnapshot.resource.Get(), offset, result, uint64_t(grid.w) * 4);
    copy(cmd, densitySnapshot.resource.Get(), offset, referencePressure[iterations % 2].resource.Get(),
         uint64_t(grid.w) * 4);
    cmd->SetComputeRootUnorderedAccessView(9, result->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(
        10, referencePressure[iterations % 2].resource->GetGPUVirtualAddress());
    pass(cmd, CompareDensity, (grid.w + 255) / 256);
}
void FluidWork::recordReadback(ID3D12GraphicsCommandList *cmd) {
    finish(cmd);
    if (intervalCount)
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * intervalCount,
                              readback.resource.Get(), 0);
    uint64_t offset = maxIntervals * 16;
    copy(cmd, readback.resource.Get(), offset, work.resource.Get(), 64);
}
void FluidWork::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, maxIntervals * 16 + 64}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Fluid work timing readback");
    auto ticks = static_cast<const uint64_t *>(data);
    times.fill(0);
    for (uint32_t i = 0; i < intervalCount; ++i)
        times[intervals[i]] += double(ticks[2 * i + 1] - ticks[2 * i]) * 1000 / frequency;
    memcpy(metrics.data(), static_cast<const char *>(data) + maxIntervals * 16, 64);
    readback.resource->Unmap(0, &written);
    faceComparisons += metrics[11];
    densityComparisons += metrics[12];
    if (metrics[7] || metrics[8])
        throw std::runtime_error("Fluid work changed a same-state dense operator within this frame");
    ++frames;
    for (uint32_t i = 0; i < PhaseCount; ++i)
        totals[i] += times[i];
    if (metrics[0] > faceTiles || metrics[1] > densityTiles)
        throw std::runtime_error("Invalid compact fluid work count");
    if (faceCaptured)
        validateFaces();
    if (densityCaptured)
        validateDensity();
}
void FluidWork::report(std::ostream &out) const {
    out << "{\"storage\":\"dense-transfer-cache\",\"execution\":\"compact-halo-tiles\",\"faceTileCapacity\":"
        << faceTiles << ",\"densityTileCapacity\":" << densityTiles << ",\"lastFaceTiles\":" << metrics[0]
        << ",\"lastDensityTiles\":" << metrics[1] << ",\"frameFaceTiles\":" << metrics[2]
        << ",\"frameFaceBuilds\":" << metrics[3] << ",\"frameDensityTiles\":" << metrics[4]
        << ",\"frameDensityBuilds\":" << metrics[5] << ",\"occupiedBinVisits\":" << metrics[6]
        << ",\"allocatedBytes\":" << work.resource->GetDesc().Width + args.resource->GetDesc().Width
        << ",\"validated\":" << (faceValidated && (!densitySeen || densityValidated) ? "true" : "false")
        << ",\"sameStateFaceMaxDifference\":" << maxFaceDifference
        << ",\"sameStateDensityMaxDifference\":" << maxDensityDifference
        << ",\"sameStateFaceComparisons\":" << faceComparisons
        << ",\"sameStateDensityComparisons\":" << densityComparisons
        << ",\"timingColumns\":[\"classification\",\"P2G\",\"G2P\",\"densityClassification\","
           "\"densitySolve\"],\"lastMs\":[";
    for (uint32_t i = 0; i < PhaseCount; ++i)
        out << (i ? "," : "") << times[i];
    out << "],\"meanMs\":[";
    for (uint32_t i = 0; i < PhaseCount; ++i)
        out << (i ? "," : "") << totals[i] / std::max(uint64_t(1), frames);
    out << "]}";
}
} // namespace lab
