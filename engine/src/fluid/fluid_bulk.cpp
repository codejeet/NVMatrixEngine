#include "fluid_bulk.h"
#include "fluid_system.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include "fluid_numeric.h"

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidBulk::FluidBulk(ID3D12Device *device, const std::filesystem::path &folder, const FluidSystemDesc &d,
                     XMUINT4 fine, float particleVolume) {
    if (d.ballisticTest || d.transferTest || d.materialTest || d.bulkFixture > 3)
        throw std::runtime_error("Bulk inventory requires the normal APIC/FLIP solver");
    projected = d.bulkProjected;
    pressureSupport = d.bulkPressure;
    coupledPressure = d.bulkCoupled;
    stateBytes = coupledPressure ? 32 : 16;
    capacityBytes = coupledPressure ? 16 : 8;
    rateBytes = projected ? 8 : 4;
    constants.minimumCell = {d.minimum.x, d.minimum.y, d.minimum.z, d.gridCellSize};
    constants.maximumDensity = {d.maximum.x, d.maximum.y, d.maximum.z, d.density};
    constants.fine = fine;
    constants.coarse = {(fine.x + 1) / 2, (fine.y + 1) / 2, (fine.z + 1) / 2, 0};
    auto &n = constants.coarse;
    n.w = n.x * n.y * n.z;
    faceStride = (n.x + 1) * (n.y + 1) * (n.z + 1);
    groups = (n.w + 127) / 128;
    if (d.bulkCapacity)
        allocator =
            std::make_unique<FluidVolumeAllocator>(device, folder, n, particleVolume, coupledPressure);
    if (d.bulkBounded)
        phaseLimiter = std::make_unique<FluidFluxLimiter>(device, folder, n);
    if (d.bulkImplicit)
        implicitTransport = std::make_unique<FluidImplicitTransport>(device, folder, n, coupledPressure);
    if (d.bulkAirExtension)
        carrierProjection = std::make_unique<FluidCarrierProjection>(
            device, folder, fine, constants.minimumCell, constants.maximumDensity, coupledPressure,
            d.capacityPressureCycles);
    constants.physical = {1 / d.simulationRate, particleVolume, float(d.bulkFixture), 0};
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        allocatedBytes += std::max(uint64_t(256), bytes);
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    state[0] = make(uint64_t(n.w) * stateBytes, L"Bulk / persistent volume and momentum ping");
    state[1] = make(uint64_t(n.w) * stateBytes, L"Bulk / persistent volume and momentum pong");
    ledger = make(uint64_t(n.w) * stateBytes, L"Bulk / cumulative injection and force ledger");
    rates = make(uint64_t(faceStride) * 3 * rateBytes, L"Bulk / area-restricted MAC volume flux");
    flux = make(uint64_t(faceStride) * 3 * stateBytes, L"Bulk / shared conservative face transfers");
    limiter = make(uint64_t(n.w) * 8, L"Bulk / donor outflow limit and CFL");
    partials = make(uint64_t(groups) * 64, L"Bulk / inventory reduction partials");
    statistics = make(256, L"Bulk / conservation and positivity metrics");
    counters = make(256, L"Bulk / frame violations and limited donors");
    uniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Bulk / frame constants");
    readback = gpu::buffer(device, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Bulk / asynchronous totals and timing");
    D3D12_ROOT_PARAMETER p[20]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 1; i < 18; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    p[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[18].Constants.ShaderRegister = 1;
    p[18].Constants.Num32BitValues = 1;
    p[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[19].Descriptor.ShaderRegister = 17;
    D3D12_ROOT_SIGNATURE_DESC r{20, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Bulk root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Bulk root");
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
        auto code = gpu::bytes(folder / "shaders" /
                               (std::string(name) +
                                (coupledPressure ? "-precise"
                                 : allocator     ? "-capacity"
                                 : projected     ? "-projected"
                                                 : "") +
                                ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&out)), name);
    };
    compute("BulkClearFrame", clear);
    compute("BulkSources", seed);
    compute("BulkRestrictFaces", restrictFaces);
    compute("BulkForces", force);
    compute("BulkLimit", limit);
    compute("BulkFlux", transfer);
    compute("BulkUpdate", update);
    compute("BulkReduce", reduce);
    compute("BulkTotals", totals);
    auto vs = gpu::bytes(folder / (coupledPressure ? "shaders/BulkVS-precise.dxil"
                                   : projected     ? "shaders/BulkVS-projected.dxil"
                                                   : "shaders/BulkVS.dxil")),
         ps = gpu::bytes(folder / (coupledPressure ? "shaders/BulkPS-precise.dxil"
                                   : projected     ? "shaders/BulkPS-projected.dxil"
                                                   : "shaders/BulkPS.dxil"));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
    g.pRootSignature = root.Get();
    g.VS = {vs.data(), vs.size()};
    g.PS = {ps.data(), ps.size()};
    g.SampleMask = UINT_MAX;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable = TRUE;
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    g.SampleDesc.Count = 1;
    gpu::check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&debug)), "Bulk debug PSO");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 40;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Bulk timestamps");
}
void FluidBulk::pass(ID3D12GraphicsCommandList *cmd, ID3D12PipelineState *pso, uint32_t count) {
    cmd->SetPipelineState(pso);
    cmd->Dispatch(count, 1, 1);
    gpu::uav(cmd);
}
void FluidBulk::startTiming(ID3D12GraphicsCommandList *cmd) {
    if (queryPairs >= 20)
        throw std::runtime_error("Bulk timestamp capacity exceeded");
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * queryPairs);
}
void FluidBulk::endTiming(ID3D12GraphicsCommandList *cmd) {
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * queryPairs + 1);
    ++queryPairs;
}
void FluidBulk::beginFrame(ID3D12GraphicsCommandList *cmd, const Camera &camera, uint32_t first,
                           uint32_t count, bool reset, bool validate, uint32_t activeParticles) {
    queryPairs = advances = 0;
    allocationPair = UINT_MAX;
    if (allocator)
        allocator->beginFrame();
    if (phaseLimiter)
        phaseLimiter->beginFrame(cmd, validate, reset);
    if (implicitTransport)
        implicitTransport->beginFrame(cmd, validate || validateImplicitThisFrame, reset);
    if (carrierProjection)
        carrierProjection->beginFrame(cmd, validate || validateImplicitThisFrame, reset);
    validateFrame = validate;
    if (reset) {
        current = 0;
        steps = sourceCalls = 0;
        ++resets;
    }
    constants.source = {first, count, reset ? 1u : 0u, activeParticles};
    constants.physical.w = float(debugMode);
    constants.viewProjection = camera.viewProjection;
    expectedVolume = double(activeParticles) * constants.physical.y;
    memcpy(uniforms.mapped, &constants, sizeof(constants));
    if (validate && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(state[0].resource->GetDevice(IID_PPV_ARGS(&device)), "Bulk validation device");
        const auto f = constants.fine;
        const uint64_t fineBytes =
            constants.physical.z == 0 ? (uint64_t(f.x + 1) * (f.y + 1) * (f.z + 1) * 3 + f.w) * 16 : 0;
        const uint64_t projectionBytes = projected
                                             ? (uint64_t(f.x + 1) * (f.y + 1) * (f.z + 1) * 3 + f.w) * 8 +
                                                   uint64_t(constants.coarse.w) * capacityBytes
                                             : 0;
        forceSnapshotOffset = uint64_t(constants.coarse.w) * (3 * stateBytes + 8) +
                              uint64_t(faceStride) * 3 * (rateBytes + stateBytes) + fineBytes +
                              projectionBytes;
        snapshot =
            gpu::buffer(device.Get(),
                        forceSnapshotOffset + (projected ? uint64_t(constants.coarse.w) * stateBytes : 0) +
                            (allocator ? uint64_t(constants.coarse.w) * stateBytes : 0),
                        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                        L"Bulk / opt-in final substep snapshot");
    }
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootUnorderedAccessView(14, counters.resource->GetGPUVirtualAddress());
    startTiming(cmd);
    pass(cmd, clear.Get(), 1);
    endTiming(cmd);
}
void FluidBulk::bind(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *solids) {
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    ID3D12Resource *buffers[] = {view.particles,
                                 view.offsets,
                                 view.indices,
                                 view.faces,
                                 solids,
                                 state[current].resource.Get(),
                                 state[1 - current].resource.Get(),
                                 ledger.resource.Get(),
                                 rates.resource.Get(),
                                 flux.resource.Get(),
                                 limiter.resource.Get(),
                                 partials.resource.Get(),
                                 statistics.resource.Get(),
                                 counters.resource.Get()};
    for (int i = 0; i < 14; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
    bindProjection(cmd);
}
void FluidBulk::setProjection(const FluidCutCellGpuView &view, ID3D12Resource *canonicalFlux) {
    if (!projected)
        return;
    if (view.preciseCapacity != coupledPressure)
        throw std::runtime_error("Bulk/cut-cell numeric format mismatch");
    if (!canonicalFlux || !view.fineVolume || !view.coarseVolume || view.fine.x != constants.fine.x ||
        view.fine.y != constants.fine.y || view.fine.z != constants.fine.z ||
        view.coarse.x != constants.coarse.x || view.coarse.y != constants.coarse.y ||
        view.coarse.z != constants.coarse.z)
        throw std::runtime_error("Projected bulk requires matching fine/coarse cut-cell capacities");
    projectedFlux = canonicalFlux;
    cutGeometry = view;
    fineVolume = view.fineVolume;
    coarseVolume = view.coarseVolume;
    coarseArea = view.coarseArea;
    swept = view.swept;
}
void FluidBulk::bindProjection(ID3D12GraphicsCommandList *cmd, bool graphics) {
    if (!projected)
        return;
    if (!projectedFlux || !fineVolume || !coarseVolume)
        throw std::runtime_error("Projected bulk geometry was not recorded before use");
    if (graphics) {
        cmd->SetGraphicsRootUnorderedAccessView(17, coarseVolume->GetGPUVirtualAddress());
        return;
    }
    cmd->SetComputeRootUnorderedAccessView(15, projectedFlux->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(16, fineVolume->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(17, coarseVolume->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstant(18, swept ? 1u : 0u, 0);
    if (allocator)
        cmd->SetComputeRootUnorderedAccessView(19, allocator->pending()->GetGPUVirtualAddress());
}
void FluidBulk::sources(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *solids) {
    gpu::Event event(cmd, L"Fluid / bulk initial and inlet inventory (not particle resampling)");
    bind(cmd, view, solids);
    startTiming(cmd);
    pass(cmd, seed.Get(), groups);
    endTiming(cmd);
    ++sourceCalls;
}
void FluidBulk::allocateSources(ID3D12GraphicsCommandList *cmd, bool advance) {
    if (!allocator || swept || !coarseVolume || !coarseArea)
        throw std::runtime_error("Source admission must run on the simulation-start capacity");
    allocationPair = queryPairs;
    startTiming(cmd);
    allocator->record(cmd, state[current].resource.Get(), coarseVolume, coarseArea, validateFrame, advance);
    endTiming(cmd);
}
namespace {
void copyBulk(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, uint64_t offset, ID3D12Resource *src,
              uint64_t bytes) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, offset, src, 0, bytes);
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace
void FluidBulk::advance(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *solids,
                        ID3D12Resource *cells) {
    gpu::Event event(cmd, L"Fluid / conservative passive bulk transport");
    if (carrierProjection && !coupledPressure)
        projectedFlux = carrierProjection->record(cmd, cutGeometry, state[current].resource.Get(), cells,
                                                  projectedFlux, constants.physical.x);
    bind(cmd, view, solids);
    startTiming(cmd);
    pass(cmd, restrictFaces.Get(), (faceStride * 3 + 127) / 128);
    if (validateFrame && projected)
        copyBulk(cmd, snapshot.resource.Get(), forceSnapshotOffset, state[current].resource.Get(),
                 uint64_t(constants.coarse.w) * stateBytes);
    pass(cmd, force.Get(), groups);
    if (implicitTransport) {
        implicitTransport->record(cmd, state[current].resource.Get(), coarseVolume, rates.resource.Get(),
                                  state[1 - current].resource.Get(), flux.resource.Get(),
                                  limiter.resource.Get(), constants.physical.x, swept);
        bind(cmd, view, solids);
    } else {
        pass(cmd, limit.Get(), groups);
        pass(cmd, transfer.Get(), (faceStride * 3 + 127) / 128);
    }
    if (phaseLimiter) {
        phaseLimiter->record(cmd, state[current].resource.Get(), coarseVolume, flux.resource.Get());
        bind(cmd, view, solids);
    }
    // Snapshot the last substep's inputs before the ping buffer can be reused.
    if (validateFrame) {
        const uint64_t n = constants.coarse.w, f = faceStride * 3;
        copyBulk(cmd, snapshot.resource.Get(), n * (2 * stateBytes), state[current].resource.Get(),
                 n * stateBytes);
        copyBulk(cmd, snapshot.resource.Get(), n * (3 * stateBytes), rates.resource.Get(), f * rateBytes);
        copyBulk(cmd, snapshot.resource.Get(), n * (3 * stateBytes) + f * rateBytes, limiter.resource.Get(),
                 n * 8);
        copyBulk(cmd, snapshot.resource.Get(), n * (3 * stateBytes + 8) + f * rateBytes, flux.resource.Get(),
                 f * stateBytes);
        if (constants.physical.z == 0) {
            const uint64_t fineBytes = view.faces->GetDesc().Width;
            copyBulk(cmd, snapshot.resource.Get(), n * (3 * stateBytes + 8) + f * (rateBytes + stateBytes),
                     view.faces, fineBytes);
            copyBulk(cmd, snapshot.resource.Get(),
                     n * (3 * stateBytes + 8) + f * (rateBytes + stateBytes) + fineBytes, solids,
                     uint64_t(constants.fine.w) * 16);
            if (projected)
                copyBulk(cmd, snapshot.resource.Get(),
                         n * (3 * stateBytes + 8) + f * (rateBytes + stateBytes) + fineBytes +
                             uint64_t(constants.fine.w) * 16,
                         projectedFlux, fineBytes / 2);
        }
    }
    if (!implicitTransport)
        pass(cmd, update.Get(), groups);
    current = 1 - current;
    ++steps;
    ++advances;
    endTiming(cmd);
}
ID3D12Resource *FluidBulk::projectCapacity(ID3D12GraphicsCommandList *cmd, ID3D12Resource *cells,
                                           const FluidMacConstraintView &mac) {
    if (!coupledPressure || !carrierProjection)
        throw std::runtime_error("Capacity/MAC projection requires coupled bulk mode");
    return carrierProjection->record(cmd, cutGeometry, state[current].resource.Get(), cells, projectedFlux,
                                     constants.physical.x, mac);
}
void FluidBulk::auditCapacityApplication(ID3D12GraphicsCommandList *cmd, ID3D12Resource *faces,
                                         ID3D12Resource *canonical) {
    carrierProjection->captureApplied(cmd, faces, canonical);
}
void FluidBulk::finishFrame(ID3D12GraphicsCommandList *cmd) {
    if (carrierProjection)
        carrierProjection->finishFrame(cmd);
    if (phaseLimiter)
        phaseLimiter->finishFrame(cmd);
    if (implicitTransport)
        implicitTransport->finishFrame(cmd);
    gpu::Event event(cmd, L"Fluid / bulk conservation reduction");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, state[current].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, ledger.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(11, limiter.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(12, partials.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(13, statistics.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(14, counters.resource->GetGPUVirtualAddress());
    bindProjection(cmd);
    startTiming(cmd);
    pass(cmd, reduce.Get(), groups);
    pass(cmd, totals.Get(), 1);
    endTiming(cmd);
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, queryPairs * 2,
                          readback.resource.Get(), 0);
    copyBulk(cmd, readback.resource.Get(), 512, statistics.resource.Get(), sizeof(Metrics));
    if (validateFrame) {
        copyBulk(cmd, snapshot.resource.Get(), 0, state[current].resource.Get(),
                 uint64_t(constants.coarse.w) * stateBytes);
        copyBulk(cmd, snapshot.resource.Get(), uint64_t(constants.coarse.w) * stateBytes,
                 ledger.resource.Get(), uint64_t(constants.coarse.w) * stateBytes);
        if (projected) {
            const auto f = constants.fine;
            const uint64_t fineFaces = uint64_t(f.x + 1) * (f.y + 1) * (f.z + 1) * 3;
            const uint64_t offset = uint64_t(constants.coarse.w) * (3 * stateBytes + 8) +
                                    uint64_t(faceStride) * 3 * (rateBytes + stateBytes) + fineFaces * 24 +
                                    uint64_t(f.w) * 16;
            copyBulk(cmd, snapshot.resource.Get(), offset, fineVolume, uint64_t(f.w) * 8);
            copyBulk(cmd, snapshot.resource.Get(), offset + uint64_t(f.w) * 8, coarseVolume,
                     uint64_t(constants.coarse.w) * capacityBytes);
        }
        if (allocator)
            copyBulk(cmd, snapshot.resource.Get(),
                     forceSnapshotOffset + uint64_t(constants.coarse.w) * stateBytes, allocator->pending(),
                     uint64_t(constants.coarse.w) * stateBytes);
    }
}
void FluidBulk::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 576}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Bulk totals map");
    auto ticks = static_cast<const uint64_t *>(data);
    gpuMs = 0;
    for (uint32_t i = 0; i < queryPairs; ++i)
        gpuMs += double(ticks[2 * i + 1] - ticks[2 * i]) * 1000 / frequency;
    memcpy(&metrics, ticks + 64, sizeof(metrics));
    const double allocationMs =
        allocationPair != UINT_MAX
            ? double(ticks[2 * allocationPair + 1] - ticks[2 * allocationPair]) * 1000 / frequency
            : 0;
    readback.resource->Unmap(0, &written);
    totalMs += gpuMs;
    ++frames;
    volumeError = std::abs(double(metrics.inventory.w) - expectedVolume) / std::max(1e-12, expectedVolume);
    momentumError = 0;
    for (int a = 0; a < 3; ++a)
        momentumError =
            std::max(momentumError, std::abs(double((&metrics.inventory.x)[a]) - (&metrics.ledger.x)[a]));
    const double momentumScale = std::max(1e-9, expectedVolume * (1 + metrics.detail.y));
    if (metrics.counts.z || !std::isfinite(volumeError) || !std::isfinite(momentumError) ||
        volumeError > 2e-4 ||
        std::abs(double(metrics.ledger.w) - expectedVolume) > std::max(1e-9, expectedVolume * 2e-4) ||
        momentumError > momentumScale * 2e-4)
        throw std::runtime_error("Bulk inventory violated mass/momentum/positivity invariants");
    if (validateFrame)
        validateSnapshot();
    if (allocator && allocationPair != UINT_MAX)
        allocator->collect(allocationMs);
    if (phaseLimiter)
        phaseLimiter->collect(frequency);
    if (carrierProjection)
        carrierProjection->collect(frequency);
    if (implicitTransport)
        implicitTransport->collect(frequency);
}
void FluidBulk::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Bulk snapshot map");
    const auto n = constants.coarse;
    const uint32_t count = n.w, faceCount = faceStride * 3;
    const auto raw = static_cast<const uint8_t *>(data);
    const auto q = readFluidNumbers<FluidDouble4>(raw, count, coupledPressure);
    const auto source =
        readFluidNumbers<FluidDouble4>(raw + uint64_t(count) * stateBytes, count, coupledPressure);
    const auto before =
        readFluidNumbers<FluidDouble4>(raw + uint64_t(count) * 2 * stateBytes, count, coupledPressure);
    auto rateData = raw + uint64_t(count) * 3 * stateBytes;
    auto rate = [&](uint32_t id) -> double {
        // Snapshot offsets are byte-packed; avoid assuming FP64 alignment.
        if (projected) {
            double v;
            memcpy(&v, rateData + uint64_t(id) * 8, 8);
            return v;
        }
        float v;
        memcpy(&v, rateData + uint64_t(id) * 4, 4);
        return v;
    };
    auto limits = reinterpret_cast<const XMFLOAT2 *>(rateData + uint64_t(faceCount) * rateBytes);
    const auto transferData = reinterpret_cast<const uint8_t *>(limits + count);
    const auto transfers = readFluidNumbers<FluidDouble4>(transferData, faceCount, coupledPressure);
    const auto fineFaces =
        reinterpret_cast<const XMFLOAT4 *>(transferData + uint64_t(faceCount) * stateBytes);
    const auto f = constants.fine;
    const uint32_t fineStride = (f.x + 1) * (f.y + 1) * (f.z + 1);
    const auto fineSolids = constants.physical.z == 0 ? fineFaces + fineStride * 3 : nullptr;
    const auto canonicalData = projected ? reinterpret_cast<const uint8_t *>(fineSolids + f.w) : nullptr;
    const auto fineCap =
        projected ? reinterpret_cast<const XMFLOAT2 *>(canonicalData + uint64_t(fineStride) * 3 * 8)
                  : nullptr;
    const auto coarseCap = readFluidNumbers<FluidDouble2>(fineCap ? fineCap + f.w : nullptr,
                                                          projected ? count : 0, coupledPressure);
    const auto forceBefore =
        readFluidNumbers<FluidDouble4>(raw + forceSnapshotOffset, projected ? count : 0, coupledPressure);
    const auto finalPending = readFluidNumbers<FluidDouble4>(
        allocator ? raw + forceSnapshotOffset + uint64_t(count) * stateBytes : nullptr, allocator ? count : 0,
        coupledPressure);
    auto canonical = [&](uint32_t id) {
        double v;
        memcpy(&v, canonicalData + uint64_t(id) * 8, 8);
        return v;
    };
    auto xyz = [&](uint32_t id) {
        return std::array<uint32_t, 3>{id % n.x, (id / n.x) % n.y, id / (n.x * n.y)};
    };
    auto cell = [&](std::array<uint32_t, 3> p) { return (p[2] * n.y + p[1]) * n.x + p[0]; };
    auto face = [&](std::array<uint32_t, 3> p, uint32_t a) {
        return a * faceStride + (p[2] * (n.y + 1) + p[1]) * (n.x + 1) + p[0];
    };
    auto volume = [&](std::array<uint32_t, 3> p) {
        double v = 1;
        for (int a = 0; a < 3; ++a)
            v *= std::max(0.,
                          std::min(2. * constants.minimumCell.w, double((&constants.maximumDensity.x)[a]) -
                                                                     (&constants.minimumCell.x)[a] -
                                                                     p[a] * (2. * constants.minimumCell.w)));
        return v;
    };
    auto capacity = [&](std::array<uint32_t, 3> p) {
        return projected ? double(coarseCap[cell(p)].x) : volume(p);
    };
    auto donorVolume = [&](std::array<uint32_t, 3> p) {
        return projected ? double(swept ? coarseCap[cell(p)].y : coarseCap[cell(p)].x) : volume(p);
    };
    bool valid = true;
    restrictionError = capacityRestrictionError = forceError = 0;
    double excess = 0;
    std::array<double, 4> sum{}, ledgerSum{};
    const bool periodic = constants.physical.z == 1 || constants.physical.z == 2;
    for (uint32_t id = 0; id < count; ++id) {
        auto p = xyz(id);
        valid &= q[id].w >= 0;
        excess += std::max(0., double(q[id].w) - capacity(p));
        if (projected) {
            double sumCapacity[2]{};
            double delta[3]{};
            for (uint32_t child = 0; child < 8; ++child) {
                const uint32_t x = p[0] * 2 + (child & 1), y = p[1] * 2 + ((child >> 1) & 1),
                               z = p[2] * 2 + (child >> 2);
                if (x >= f.x || y >= f.y || z >= f.z)
                    continue;
                const auto v = fineCap[(z * f.y + y) * f.x + x];
                valid &= std::isfinite(v.x) && std::isfinite(v.y) && v.x >= 0 && v.y >= 0;
                sumCapacity[0] += v.x;
                sumCapacity[1] += v.y;
                if (advances) {
                    for (uint32_t a = 0; a < 3; ++a) {
                        const uint32_t f0 = a * fineStride + (z * (f.y + 1) + y) * (f.x + 1) + x;
                        const uint32_t step[] = {1, f.x + 1, (f.x + 1) * (f.y + 1)};
                        const auto l = fineFaces[f0], r = fineFaces[f0 + step[a]];
                        delta[a] += .5 * (double(l.x) - l.y + double(r.x) - r.y) * v.x;
                    }
                }
            }
            for (int a = 0; a < 2; ++a) {
                const double error = std::abs(sumCapacity[a] - (&coarseCap[id].x)[a]);
                capacityRestrictionError = std::max(capacityRestrictionError, error);
                valid &= std::isfinite((&coarseCap[id].x)[a]) && error < std::max(1e-10, volume(p) * 5e-7);
            }
            if (advances) {
                valid &= forceBefore[id].w == before[id].w;
                for (int a = 0; a < 3; ++a) {
                    const double expected = (&forceBefore[id].x)[a] +
                                            forceBefore[id].w * delta[a] / std::max(sumCapacity[0], 1e-20);
                    const double error = std::abs(expected - (&before[id].x)[a]);
                    forceError = std::max(forceError, error);
                    valid &= std::isfinite(expected) && error < std::max(1e-8, std::abs(expected) * 5e-5);
                }
            }
        }
        for (int a = 0; a < 4; ++a) {
            valid &= std::isfinite((&q[id].x)[a]) && std::isfinite((&source[id].x)[a]);
            sum[a] += (&q[id].x)[a];
            if (allocator) {
                sum[a] += (&finalPending[id].x)[a];
                valid &= std::isfinite((&finalPending[id].x)[a]) && finalPending[id].w >= 0;
            }
            ledgerSum[a] += (&source[id].x)[a];
            if (constants.physical.z == 3)
                valid &= (&q[id].x)[a] == (&source[id].x)[a];
        }
        if (!advances)
            continue;
        double outflow = 0;
        std::array<double, 4> result{};
        for (int c = 0; c < 4; ++c)
            result[c] = (&before[id].x)[c];
        for (uint32_t a = 0; a < 3; ++a) {
            auto r = p;
            ++r[a];
            uint32_t lface = face(p, a), rface = face(r, a);
            outflow += std::max(0., -rate(lface)) + std::max(0., rate(rface));
            for (int c = 0; c < 4; ++c)
                result[c] += double((&transfers[lface].x)[c]) - (&transfers[rface].x)[c];
        }
        const double v = donorVolume(p);
        double cfl = v > 0 ? constants.physical.x * outflow / v : 0, scale = implicitTransport ? 1
                                                                             : v > 0
                                                                                 ? (cfl > .95 ? .95 / cfl : 1)
                                                                                 : 0;
        valid &= std::abs(limits[id].x - scale) < 2e-5 &&
                 std::abs(limits[id].y - cfl) < std::max(1e-5, cfl * 2e-5);
        for (int c = 0; c < 4; ++c)
            valid &= std::abs(result[c] - (&q[id].x)[c]) < std::max(1e-8, std::abs(result[c]) * 2e-5);
    }
    if (advances)
        for (uint32_t id = 0; id < faceCount; ++id) {
            uint32_t axis = id / faceStride, k = id % faceStride;
            std::array<uint32_t, 3> p{k % (n.x + 1), (k / (n.x + 1)) % (n.y + 1),
                                      k / ((n.x + 1) * (n.y + 1))};
            bool live = p[0] < n.x + (axis == 0) && p[1] < n.y + (axis == 1) && p[2] < n.z + (axis == 2);
            bool wall = p[axis] == 0 || p[axis] == (&n.x)[axis];
            if (!live || (!periodic && wall)) {
                valid &= rate(id) == 0;
                for (int a = 0; a < 4; ++a)
                    valid &= (&transfers[id].x)[a] == 0;
                continue;
            }
            double expectedRate = 0;
            const uint32_t b = (axis + 1) % 3, c = (axis + 2) % 3;
            auto width = [&](uint32_t i, uint32_t a, double h) {
                return std::max(0., std::min(h, double((&constants.maximumDensity.x)[a]) -
                                                    (&constants.minimumCell.x)[a] - i * h));
            };
            if (periodic) {
                const double v[] = {.7, -.2, .3};
                expectedRate = v[axis] * (constants.physical.z == 2 ? 100 : 1) *
                               width(p[b], b, 2. * constants.minimumCell.w) *
                               width(p[c], c, 2. * constants.minimumCell.w);
            } else if (constants.physical.z == 0) {
                for (uint32_t i = 0; i < 2; ++i)
                    for (uint32_t j = 0; j < 2; ++j) {
                        auto r = p;
                        for (auto &v : r)
                            v *= 2;
                        r[b] += i;
                        r[c] += j;
                        if (r[b] >= (&f.x)[b] || r[c] >= (&f.x)[c])
                            continue;
                        const uint32_t fineFace =
                            axis * fineStride + (r[2] * (f.y + 1) + r[1]) * (f.x + 1) + r[0];
                        if (projected) {
                            expectedRate += canonical(fineFace);
                            continue;
                        }
                        auto l = r;
                        --l[axis];
                        auto fid = [&](std::array<uint32_t, 3> v) {
                            return (v[2] * f.y + v[1]) * f.x + v[0];
                        };
                        if (fineSolids[fid(l)].x < 0 || fineSolids[fid(r)].x < 0)
                            continue;
                        const auto v =
                            fineFaces[axis * fineStride + (r[2] * (f.y + 1) + r[1]) * (f.x + 1) + r[0]].x;
                        expectedRate += v * width(r[b], b, constants.minimumCell.w) *
                                        width(r[c], c, constants.minimumCell.w);
                    }
            }
            const double error = std::abs(rate(id) - expectedRate);
            restrictionError = std::max(restrictionError, error);
            valid &= std::isfinite(rate(id)) &&
                     error < (projected ? std::max(1e-14, std::abs(expectedRate) * 1e-12)
                                        : std::max(1e-8, std::abs(expectedRate) * 3e-5));
            // The implicit solve has a separate independent matrix/constitutive
            // audit. Retain canonical flux restriction and cell balance here;
            // do not apply the explicit old-state donor formula to its answer.
            if (implicitTransport)
                continue;
            auto donor = p;
            if (rate(id) >= 0)
                donor[axis] = p[axis] ? p[axis] - 1 : (&n.x)[axis] - 1;
            else if (donor[axis] == (&n.x)[axis])
                donor[axis] = 0;
            uint32_t d = cell(donor);
            const double v = donorVolume(donor);
            double factor = v > 0 ? constants.physical.x * rate(id) * limits[d].x / v : 0;
            if (phaseLimiter) {
                // Independent limiter replay is separate. Here retain the
                // donor contract: one nonnegative coefficient <= the candidate
                // scales volume AND all momentum, with no reversed transport.
                const double candidate = factor * before[d].w;
                const double ratio = candidate != 0 ? transfers[id].w / candidate : 1;
                valid &= std::isfinite(ratio) && ratio >= 0 && ratio <= 1 + 2e-5;
                factor *= ratio;
            }
            for (int a = 0; a < 4; ++a) {
                double expected = factor * (&before[d].x)[a];
                valid &=
                    std::abs(expected - (&transfers[id].x)[a]) < std::max(1e-9, std::abs(expected) * 2e-5);
            }
        }
    for (int a = 0; a < 4; ++a) {
        valid &= std::abs(sum[a] - (&metrics.inventory.x)[a]) <
                 std::max(1e-8, expectedVolume * (1 + metrics.detail.y) * 2e-5);
        valid &= std::abs(ledgerSum[a] - (&metrics.ledger.x)[a]) <
                 std::max(1e-8, expectedVolume * (1 + metrics.detail.y) * 2e-5);
    }
    valid &= std::abs(excess - metrics.detail.w) < std::max(1e-8, expectedVolume * 2e-5);
    snapshot.resource->Unmap(0, &written);
    if (!valid)
        throw std::runtime_error("Bulk snapshot flux/limiter/conservation validation failed");
    validated = true;
}
void FluidBulk::drawDebug(ID3D12GraphicsCommandList *cmd) {
    if (!debugMode)
        return;
    gpu::Event event(cmd, L"Fluid / DEBUG passive bulk inventory (not optical geometry)");
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetPipelineState(debug.Get());
    cmd->SetGraphicsRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(6, state[current].resource->GetGPUVirtualAddress());
    bindProjection(cmd, true);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmd->DrawInstanced(24, constants.coarse.w, 0, 0);
}
FluidBulkGpuView FluidBulk::gpuView() const {
    auto m = constants.minimumCell;
    m.w *= 2;
    return {state[current].resource.Get(),
            ledger.resource.Get(),
            rates.resource.Get(),
            constants.coarse,
            m,
            constants.maximumDensity,
            projected,
            allocator ? allocator->pending() : nullptr,
            coupledPressure};
}
void FluidBulk::report(std::ostream &out) const {
    out << "{\"authority\":\""
        << (pressureSupport ? "pressure-support inventory; particles still own mass and free surface"
                            : "passive replica; particle solver remains authoritative")
        << "\",\"coarseCells\":" << constants.coarse.w << ",\"fixture\":" << uint32_t(constants.physical.z)
        << ",\"steps\":" << steps << ",\"sourceCalls\":" << sourceCalls << ",\"resets\":" << resets
        << ",\"volumeM3\":" << metrics.inventory.w << ",\"expectedVolumeM3\":" << expectedVolume
        << ",\"relativeVolumeError\":" << volumeError
        << ",\"massKg\":" << metrics.inventory.w * constants.maximumDensity.w
        << ",\"momentumErrorKgMps\":" << momentumError * constants.maximumDensity.w
        << ",\"activeCells\":" << metrics.counts.x << ",\"limitedDonorUpdates\":" << metrics.counts.y
        << ",\"invalid\":" << metrics.counts.z << ",\"overfilledCells\":" << metrics.counts.w
        << ",\"maxVolumeFraction\":" << metrics.detail.x << ",\"maxSpeed\":" << metrics.detail.y
        << ",\"maxOutgoingCfl\":" << metrics.detail.z << ",\"lastFrameMs\":" << gpuMs
        << ",\"inventoryBits\":" << (coupledPressure ? 64 : 32)
        << ",\"projectedFlux\":" << (projected ? "true" : "false")
        << ",\"sweptCapacity\":" << (swept ? "true" : "false") << ",\"excessVolumeM3\":" << metrics.detail.w
        << ",\"fluxRestrictionMaxError\":" << restrictionError
        << ",\"capacityRestrictionMaxError\":" << capacityRestrictionError
        << ",\"forceSourceMaxError\":" << forceError
        << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"allocatedGpuBufferBytes\":" << allocatedBytes
        << ",\"validated\":" << (validated ? "true" : "false") << ",\"sourceAllocation\":";
    if (allocator)
        allocator->report(out);
    else
        out << "null";
    out << ",\"phaseLimiter\":";
    if (phaseLimiter)
        phaseLimiter->report(out);
    else
        out << "null";
    out << ",\"implicitTransport\":";
    if (implicitTransport)
        implicitTransport->report(out);
    else
        out << "null";
    out << ",\"carrierProjection\":";
    if (carrierProjection)
        carrierProjection->report(out);
    else
        out << "null";
    out << "}";
}
} // namespace lab
