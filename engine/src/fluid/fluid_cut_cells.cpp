#include "fluid_cut_cells.h"
#include "fluid_system.h"
#include <d3dcompiler.h>
#include <cstring>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace {
void copyCut(ID3D12GraphicsCommandList *cmd, ID3D12Resource *target, uint64_t offset, ID3D12Resource *source,
             uint64_t bytes) {
    gpu::transition(cmd, source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(target, offset, source, 0, bytes);
    gpu::transition(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
bool sameGeometry(std::span<const FluidCollider> a, std::span<const FluidCollider> b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (memcmp(&a[i].worldToLocal, &b[i].worldToLocal, sizeof(XMFLOAT4X4)) ||
            memcmp(&a[i].extentType, &b[i].extentType, sizeof(XMFLOAT4)) ||
            memcmp(&a[i].meshMinimumSpacing, &b[i].meshMinimumSpacing, sizeof(XMFLOAT4)) ||
            memcmp(&a[i].meshDimensions, &b[i].meshDimensions, sizeof(XMUINT4)))
            return false;
    return true;
}
} // namespace
FluidCutCells::FluidCutCells(ID3D12Device *device, const std::filesystem::path &folder,
                             const FluidSystemDesc &desc, XMUINT4 fine) {
    if (desc.ballisticTest || desc.transferTest || desc.materialTest)
        throw std::runtime_error("Cut cells require the normal simulated APIC/FLIP path");
    constants.minimumCell = {desc.minimum.x, desc.minimum.y, desc.minimum.z, desc.gridCellSize};
    constants.maximum = {desc.maximum.x, desc.maximum.y, desc.maximum.z, 0};
    constants.fine = fine;
    auto &c = constants.coarse;
    c = {(fine.x + 1) / 2, (fine.y + 1) / 2, (fine.z + 1) / 2, 0};
    c.w = c.x * c.y * c.z;
    constants.control.z = desc.cutCellFixture;
    constants.control.w =
        ((desc.cutPressure || desc.cutCellFixture) ? 4u : 0u) | (desc.cutKernelCache ? 0u : 8u);
    timeCentered = desc.cutTimeCentered;
    preciseBulk = desc.bulkCoupled;
    if (timeCentered)
        constants.control.w |= 16u;
    if (desc.cutCellFixture > 4)
        throw std::runtime_error("Invalid cut-cell fixture");
    simulationRate = desc.simulationRate;
    fineStride = (fine.x + 1) * (fine.y + 1) * (fine.z + 1);
    coarseStride = (c.x + 1) * (c.y + 1) * (c.z + 1);
    const uint64_t sizes[] = {uint64_t(fineStride) * 4,
                              uint64_t(fine.w) * 8,
                              uint64_t(fineStride) * 12,
                              uint64_t(c.w) * (preciseBulk ? 16 : 8),
                              uint64_t(coarseStride) * 12,
                              uint64_t((c.w + 127) / 128) * sizeof(Metrics),
                              sizeof(Metrics),
                              uint64_t((constants.control.w & 4) ? fine.w : 1) * 16,
                              uint64_t((constants.control.w & 4) ? 3 * fine.w + 1 : 1) * 4,
                              uint64_t(timeCentered ? fineStride : 1) * 4,
                              uint64_t(timeCentered ? fineStride * 3 : 1) * 4};
    const wchar_t *names[] = {L"Cut cells / shared corner solid SDF",
                              L"Cut cells / fine open volumes and previous endpoint",
                              L"Cut cells / shared fine face aperture",
                              L"Cut cells / restricted coarse open volume",
                              L"Cut cells / restricted coarse face aperture",
                              L"Cut cells / capacity reduction",
                              L"Cut cells / metrics",
                              L"Cut cells / integrated solid density kernel",
                              L"Cut cells / compact kernel work and shared voxel fingerprints",
                              L"Cut cells / preceding corner SDF endpoint",
                              L"Cut cells / time-integrated shared pressure aperture"};
    for (uint32_t i = 0; i < buffers.size(); ++i) {
        buffers[i] =
            gpu::buffer(device, sizes[i], D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, names[i]);
        allocatedBytes += buffers[i].resource->GetDesc().Width;
    }
    readback = gpu::buffer(device, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Cut cells / existing-fence timing and metrics");
    cameraUniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, L"Cut cells / debug camera");
    D3D12_ROOT_PARAMETER p[16]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, sizeof(Constants) / 4};
    for (uint32_t i = 1; i < 4; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[i].Descriptor.ShaderRegister = i;
    }
    for (uint32_t i = 4; i < 11; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 4;
    }
    p[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[11].Descriptor.ShaderRegister = 1;
    p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[12].Descriptor.ShaderRegister = 7;
    p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[13].Descriptor.ShaderRegister = 8;
    for (uint32_t i = 14; i < 16; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 5;
    }
    D3D12_ROOT_SIGNATURE_DESC r{16, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Cut-cell root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Cut-cell root");
    const char *entries[] = {"CutCorners",       "CutVolumes",        "CutAreas",        "CutRestrictVolumes",
                             "CutRestrictAreas", "CutReduce",         "CutTotals",       "CutSolidKernel",
                             "CutKernelClear",   "CutKernelClassify", "CutKernelPrepare"};
    for (uint32_t i = 0; i < pipelines.size(); ++i) {
        auto code = gpu::bytes(folder / "shaders" /
                               (std::string(entries[i]) + (preciseBulk ? "-precise.dxil" : ".dxil")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.Get();
        state.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipelines[i])), entries[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC dispatch{};
    dispatch.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &dispatch, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&kernelDispatch)),
               "Cut-cell kernel indirect dispatch");
    auto vs = gpu::bytes(folder / (preciseBulk ? "shaders/CutVS-precise.dxil" : "shaders/CutVS.dxil")),
         ps = gpu::bytes(folder / (preciseBulk ? "shaders/CutPS-precise.dxil" : "shaders/CutPS.dxil"));
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
    gpu::check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&debug)), "Cut-cell debug PSO");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 40;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Cut-cell timestamps");
}
void FluidCutCells::bind(ID3D12GraphicsCommandList *cmd) {
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    for (uint32_t i = 0; i < buffers.size(); ++i)
        cmd->SetComputeRootUnorderedAccessView(i >= 7 ? i + 5 : i + 4,
                                               buffers[i].resource->GetGPUVirtualAddress());
}
void FluidCutCells::pass(ID3D12GraphicsCommandList *cmd, uint32_t i, uint32_t count) {
    cmd->SetPipelineState(pipelines[i].Get());
    cmd->Dispatch((count + 127) / 128, 1, 1);
    gpu::uav(cmd);
}
void FluidCutCells::startTiming(ID3D12GraphicsCommandList *cmd) {
    if (queriesUsed >= 38)
        throw std::runtime_error("Cut-cell query capacity exceeded");
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queriesUsed++);
}
void FluidCutCells::endTiming(ID3D12GraphicsCommandList *cmd) {
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queriesUsed++);
}
void FluidCutCells::beginFrame(bool reset, bool validate) {
    if (reset)
        initialized = false;
    queriesUsed = frameUpdates = 0;
    validateFrame = validate;
    if (validate && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(buffers[0].resource->GetDevice(IID_PPV_ARGS(&device)), "Cut-cell validation device");
        uint64_t size = 0;
        for (uint32_t i = 0; i < 5; ++i) {
            snapshotOffsets[i] = size;
            size += buffers[i].resource->GetDesc().Width;
        }
        snapshotOffsets[5] = size;
        size += buffers[1].resource->GetDesc().Width;
        snapshotOffsets[6] = size;
        size += uint64_t(constants.coarse.w) * (preciseBulk ? 32 : 16);
        snapshotOffsets[7] = size;
        size += buffers[7].resource->GetDesc().Width;
        snapshotOffsets[8] = size;
        size += buffers[8].resource->GetDesc().Width;
        for (uint32_t i = 9; i < 11; ++i) {
            snapshotOffsets[i] = size;
            size += buffers[i].resource->GetDesc().Width;
        }
        snapshotOffsets[11] = size;
        size += timeCentered ? buffers[0].resource->GetDesc().Width : 0;
        snapshot =
            gpu::buffer(device.Get(), size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, L"Cut cells / bounded independent geometry audit");
    }
}
void FluidCutCells::record(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view,
                           std::span<const FluidCollider> colliders, const MeshSdfAsset &asset, uint64_t step,
                           bool simulated) {
    projectionSwept = false;
    XMFLOAT4 fixture{};
    const auto &lo = constants.minimumCell;
    const auto &hi = constants.maximum;
    if (constants.control.z) {
        const float x =
            lo.x +
            (hi.x - lo.x) *
                (.371f + (constants.control.z == 3 ? .1f * std::sin(float(step) / simulationRate) : 0));
        fixture = {1, 0, 0, -x};
        if (constants.control.z == 2)
            fixture = {.3f, .7f, 1.4f,
                       -(.3f * (lo.x + hi.x) + .7f * (lo.y + hi.y) + 1.4f * (lo.z + hi.z)) * .5f};
        if (constants.control.z == 4)
            fixture = {(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f, (lo.z + hi.z) * .5f,
                       .23f * std::min({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z})};
    }
    if (initialized && (constants.control.z ? !memcmp(&fixture, &constants.fixture, sizeof(fixture))
                                            : sameGeometry(colliders, lastColliders)))
        return;
    gpu::Event event(cmd, L"Fluid / geometric cut-cell capacity and shared aperture");
    projectionSwept = initialized && simulated;
    constants.fixture = fixture;
    constants.control.x = initialized ? 0 : 1;
    constants.control.y = uint32_t(colliders.size());
    lastColliders.assign(colliders.begin(), colliders.end());
    mesh = &asset;
    if (validateFrame && initialized)
        copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[5], buffers[1].resource.Get(),
                buffers[1].resource->GetDesc().Width);
    if (validateFrame && initialized && timeCentered)
        copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[11], buffers[0].resource.Get(),
                buffers[0].resource->GetDesc().Width);
    bind(cmd);
    cmd->SetComputeRootShaderResourceView(1, view.colliderAddress);
    cmd->SetComputeRootShaderResourceView(2, view.meshPhi->GetGPUVirtualAddress());
    startTiming(cmd);
    pass(cmd, 0, fineStride);
    pass(cmd, 1, constants.fine.w);
    pass(cmd, 2, fineStride * 3);
    pass(cmd, 3, constants.coarse.w);
    pass(cmd, 4, coarseStride * 3);
    if (constants.control.w & 4) {
        gpu::Event kernelEvent(cmd, L"Fluid / locally cached solid density support");
        pass(cmd, 8, 1);
        cmd->SetPipelineState(pipelines[9].Get());
        cmd->Dispatch((constants.fine.w + 63) / 64, 1, 1);
        gpu::uav(cmd);
        pass(cmd, 10, 1);
        auto *arguments = buffers[5].resource.Get();
        gpu::transition(cmd, arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        cmd->SetPipelineState(pipelines[7].Get());
        cmd->ExecuteIndirect(kernelDispatch.Get(), 1, arguments, offsetof(Metrics, fineCount), nullptr, 0);
        gpu::uav(cmd);
        gpu::transition(cmd, arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    endTiming(cmd);
    initialized = true;
    ++frameUpdates;
    ++updates;
}
void FluidCutCells::finishFrame(ID3D12GraphicsCommandList *cmd, ID3D12Resource *bulk) {
    if (!initialized)
        throw std::runtime_error("Cut-cell geometry was not initialized");
    gpu::Event event(cmd, L"Fluid / geometric capacity audit (not liquid ownership)");
    bulkConnected = bulk != nullptr;
    constants.control.w = (constants.control.w & 28u) | (bulk ? 1u : 0u) | (frameUpdates ? 2u : 0u);
    bind(cmd);
    // SRV access to the optional bulk replica requires its own state transition.
    if (bulk)
        gpu::transition(cmd, bulk, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmd->SetComputeRootShaderResourceView(3,
                                          (bulk ? bulk : buffers[0].resource.Get())->GetGPUVirtualAddress());
    startTiming(cmd);
    pass(cmd, 5, constants.coarse.w);
    pass(cmd, 6, 128);
    endTiming(cmd);
    if (bulk)
        gpu::transition(cmd, bulk, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, queriesUsed, readback.resource.Get(),
                          0);
    copyCut(cmd, readback.resource.Get(), 512, buffers[6].resource.Get(), sizeof(Metrics));
    if (validateFrame) {
        for (uint32_t i = 0; i < 5; ++i)
            copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[i], buffers[i].resource.Get(),
                    buffers[i].resource->GetDesc().Width);
        if (bulk)
            copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[6], bulk,
                    uint64_t(constants.coarse.w) * (preciseBulk ? 32 : 16));
        copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[7], buffers[7].resource.Get(),
                buffers[7].resource->GetDesc().Width);
        copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[8], buffers[8].resource.Get(),
                buffers[8].resource->GetDesc().Width);
        if (timeCentered)
            for (uint32_t i = 9; i < 11; ++i)
                copyCut(cmd, snapshot.resource.Get(), snapshotOffsets[i], buffers[i].resource.Get(),
                        buffers[i].resource->GetDesc().Width);
    }
}
void FluidCutCells::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 512 + sizeof(Metrics)}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Cut-cell metrics map");
    const auto ticks = static_cast<const uint64_t *>(data);
    gpuMs = 0;
    for (uint32_t i = 0; i < queriesUsed; i += 2)
        gpuMs += double(ticks[i + 1] - ticks[i]) * 1000 / frequency;
    memcpy(&metrics, static_cast<const char *>(data) + 512, sizeof(metrics));
    readback.resource->Unmap(0, &written);
    totalMs += gpuMs;
    ++frames;
    if (metrics.fineCount.w || metrics.coarseCount.w || metrics.kernel.w ||
        !std::isfinite(metrics.volume.x) || !std::isfinite(metrics.inventory.w) || metrics.volume.x < 0 ||
        metrics.fineCount.x + metrics.fineCount.y + metrics.fineCount.z != constants.fine.w ||
        metrics.coarseCount.x + metrics.coarseCount.y + metrics.coarseCount.z != constants.coarse.w)
        throw std::runtime_error("Cut-cell capacity invariants failed");
    if (validateFrame)
        validateSnapshot();
}
void FluidCutCells::drawDebug(ID3D12GraphicsCommandList *cmd, const XMFLOAT4X4 &camera) {
    if (!debugVisible)
        return;
    gpu::Event event(cmd, L"Fluid / cut-cell capacity wires");
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    memcpy(cameraUniforms.mapped, &camera, sizeof(camera));
    cmd->SetGraphicsRootConstantBufferView(11, cameraUniforms.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(7, buffers[3].resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(debug.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmd->DrawInstanced(24, constants.coarse.w, 0, 0);
}
FluidCutCellGpuView FluidCutCells::gpuView() const {
    return {buffers[1].resource.Get(),
            buffers[2].resource.Get(),
            buffers[3].resource.Get(),
            buffers[4].resource.Get(),
            buffers[7].resource.Get(),
            constants.fine,
            constants.coarse,
            constants.minimumCell,
            constants.maximum,
            frameUpdates != 0,
            projectionSwept,
            buffers[timeCentered && projectionSwept ? 10 : 2].resource.Get(),
            timeCentered && projectionSwept,
            preciseBulk};
}
void FluidCutCells::report(std::ostream &out) const {
    // Time-integrated pressure support is distinct from endpoint collision and
    // liquid capacity. Both representations stay explicitly inspectable.
    out << "{\"authority\":\"sampled solid geometry; not liquid mass\",\"fixture\":" << constants.control.z
        << ",\"coarseCapacityBits\":" << (preciseBulk ? 64 : 32) << ",\"updates\":" << updates
        << ",\"frameUpdates\":" << frameUpdates << ",\"openVolumeM3\":" << metrics.volume.x
        << ",\"previousOpenVolumeM3\":" << metrics.volume.y
        << ",\"absoluteChangedVolumeM3\":" << metrics.volume.z
        << ",\"maxCellChangedVolumeM3\":" << metrics.volume.w << ",\"fineCutCells\":" << metrics.fineCount.x
        << ",\"fineClosedCells\":" << metrics.fineCount.y << ",\"coarseCutCells\":" << metrics.coarseCount.x
        << ",\"bulkInventoryM3\":" << metrics.inventory.x
        << ",\"bulkExcessCapacityM3\":" << metrics.inventory.y
        << ",\"bulkInClosedCellsM3\":" << metrics.inventory.z
        << ",\"maxBulkOpenVolumeRatio\":" << metrics.inventory.w
        << ",\"invalid\":" << metrics.fineCount.w + metrics.coarseCount.w << ",\"lastFrameMs\":" << gpuMs
        << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"allocatedGpuBufferBytes\":" << allocatedBytes << ",\"maxGeometryError\":" << geometryError
        << ",\"maxRestrictionError\":" << restrictionError << ",\"maxHistoryError\":" << historyError
        << ",\"timeCenteredPressure\":" << (timeCentered ? "true" : "false")
        << ",\"lastProjectionTimeCentered\":" << (timeCentered && projectionSwept ? "true" : "false")
        << ",\"maxTimeAreaError\":" << timeAreaError
        << ",\"solidKernelEnabled\":" << ((constants.control.w & 4) ? "true" : "false")
        << ",\"solidKernelCache\":" << ((constants.control.w & 8) ? "false" : "true")
        << ",\"lastEndpointKernelRebuilt\":" << metrics.kernel.x
        << ",\"lastEndpointKernelReused\":" << metrics.kernel.y - metrics.kernel.x
        << ",\"maxSolidKernelError\":" << kernelError << ",\"auditedFrames\":" << auditedFrames
        << ",\"validated\":" << (validated ? "true" : "false") << "}";
}
} // namespace lab
