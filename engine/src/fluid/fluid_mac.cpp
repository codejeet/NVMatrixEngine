#include "fluid_mac.h"
#include "fluid_complexity.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidMac::FluidMac(ID3D12Device *device, const std::filesystem::path &folder, XMUINT4 dims, uint32_t sweeps,
                   float spacing, float density, float simulationRate, float tension, bool useMultigrid,
                   bool cutPressure)
    : grid(dims), iterations(sweeps), h(spacing), rho(density), rate(simulationRate), sigma(tension),
      cutProjection(cutPressure) {
    if (!sweeps || sweeps > 1000 || !grid.w || uint64_t(grid.x) * grid.y * grid.z != grid.w)
        throw std::runtime_error("Invalid adaptive MAC configuration");
    if (useMultigrid)
        multigrid = std::make_unique<FluidMacPressure>(device, folder, grid, spacing, density, simulationRate,
                                                       cutPressure);
    coarse = {(grid.x + 1) / 2, (grid.y + 1) / 2, (grid.z + 1) / 2, 0};
    coarse.w = coarse.x * coarse.y * coarse.z;
    faceCount = 3 * (grid.x + 1) * (grid.y + 1) * (grid.z + 1);
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        allocatedBytes += std::max(uint64_t(256), bytes);
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    map = make(uint64_t(grid.w) * 4, L"Adaptive MAC / leaf ownership map");
    state = make(uint64_t(coarse.w) * 4, L"Adaptive MAC / actual coarse LOD and hysteresis");
    list = make(uint64_t(grid.w) * 4, L"Adaptive MAC / compact pressure leaves");
    rows = make(uint64_t(grid.w) * sizeof(Row), L"Adaptive MAC / coupled pressure operator");
    if (cutProjection && multigrid)
        cutExact =
            make(uint64_t(grid.w) * sizeof(FluidCutPressureRow), L"Cut pressure / precise physical operator");
    if (cutProjection)
        cutFlux = make(uint64_t(faceCount) * 8, L"Cut pressure / canonical shared-face volume flux");
    counters = make(256, L"Adaptive MAC / leaf and junction counters");
    arguments = make(256, L"Adaptive MAC / indirect leaf dispatch");
    readback = gpu::buffer(device, 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Adaptive MAC / timing and counters");
    D3D12_ROOT_PARAMETER p[23]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {1, 0, 12};
    const uint32_t registers[]{6, 10, 9, 13, 14, 7, 8, 16, 17, 18, 19, 20, 21, 27};
    for (uint32_t i = 0; i < 14; ++i) {
        p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i + 2].Descriptor.ShaderRegister = registers[i];
    }
    p[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[16].Descriptor.ShaderRegister = 28;
    for (uint32_t i = 17; i < 19; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i + 14;
    }
    p[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[19].Constants = {2, 0, 1};
    p[20].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[20].Descriptor.ShaderRegister = 33;
    p[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[21].Descriptor.ShaderRegister = 34;
    p[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[22].Descriptor.ShaderRegister = 35;
    D3D12_ROOT_SIGNATURE_DESC r{23, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Adaptive MAC root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Adaptive MAC root");
    const char *names[]{"MacClear",      "MacClassify", "MacLeaves",       "MacPrepare",
                        "MacRestrict",   "MacAssemble", "MacSmooth",       "MacProject",
                        "MacProlongate", "MacMeasure",  "MacApplyCapacity"};
    for (uint32_t i = 0; i < pipelines.size(); ++i) {
        if (i == 10 && !cutProjection)
            continue;
        const std::string suffix = multigrid && (i == 5 || i == 7) ? "Precise.dxil" : ".dxil";
        auto code =
            gpu::bytes(folder / "shaders" / (std::string(names[i]) + (cutProjection ? "Cut" : "") + suffix));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sig{12, 1, &arg, 0};
    gpu::check(device->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&dispatch)),
               "Adaptive MAC indirect signature");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 32;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Adaptive MAC timestamps");
    auto vs = gpu::bytes(folder / "shaders/MacVS.dxil"), ps = gpu::bytes(folder / "shaders/MacPS.dxil");
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
    gpu::check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&debug)), "Adaptive MAC debug PSO");
}
void FluidMac::setImportance(const FluidComplexityGpuView &v) {
    importance = v.state;
    bricks = v.grid;
}
void FluidMac::beginFrame(bool reset, bool validate) {
    solves = 0;
    resetPending = resetPending || reset;
    validateFrame = validate;
    if (multigrid)
        multigrid->beginFrame(validate);
    if (multigrid)
        multigrid->splitCoarse = splitCoarse;
}
void FluidMac::copySnapshot(ID3D12GraphicsCommandList *cmd, ID3D12Resource *src, uint64_t &offset,
                            uint64_t bytes) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(snapshot.resource.Get(), offset, src, 0, bytes);
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    offset += bytes;
}
uint32_t FluidMac::solve(ID3D12GraphicsCommandList *cmd, ID3D12Resource *frame, ID3D12Resource *faces,
                         ID3D12Resource *scratch, ID3D12Resource *cells, ID3D12Resource *solids,
                         ID3D12Resource *material, ID3D12Resource *ping, ID3D12Resource *pong) {
    if (solves >= 16)
        throw std::runtime_error("Adaptive MAC substep capacity exceeded");
    uint64_t offset = 0;
    if (validateFrame) {
        if (!snapshot.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(faces->GetDevice(IID_PPV_ARGS(&device)), "Adaptive MAC validation device");
            snapshot =
                gpu::buffer(device.Get(),
                            uint64_t(faceCount) * 32 + uint64_t(grid.w) * (sizeof(Row) + 56) +
                                uint64_t(coarse.w) * 4 + (multigrid ? uint64_t(grid.w) * 4 : 0) +
                                (cutProjection ? uint64_t(grid.w) * 8 + uint64_t(faceCount) * 4 : 0) +
                                (cutExact.resource ? uint64_t(grid.w) * sizeof(FluidCutPressureRow) : 0) +
                                (cutProjection ? uint64_t(faceCount) * 8 : 0),
                            D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_COPY_DEST, L"Adaptive MAC / operator audit");
        }
        copySnapshot(cmd, faces, offset, uint64_t(faceCount) * 16);
    }
    gpu::Event event(cmd, L"Fluid / coupled 2:1 MAC pressure and velocity");
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, solves * 2);
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, frame->GetGPUVirtualAddress());
    if (cutExact.resource)
        cmd->SetComputeRootUnorderedAccessView(20, cutExact.resource->GetGPUVirtualAddress());
    if (cutProjection)
        cmd->SetComputeRootUnorderedAccessView(21, cutFlux.resource->GetGPUVirtualAddress());
    if (cutProjection) {
        cmd->SetComputeRootUnorderedAccessView(17, cut.fineVolume->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(18, cut.pressureArea->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstant(19, (cut.swept ? 1u : 0u) | (cut.timeCentered ? 2u : 0u), 0);
    }
    if (multigrid)
        cmd->SetComputeRootUnorderedAccessView(16, multigrid->pressureResource()->GetGPUVirtualAddress());
    XMUINT4 constants[]{coarse,
                        bricks,
                        {resetPending ? 1u : 0u, importance ? 1u : 0u, (validateFrame || multigrid) ? 1u : 0u,
                         forcedFine ? 1u : 0u}};
    cmd->SetComputeRoot32BitConstants(1, 12, constants, 0);
    ID3D12Resource *resources[]{faces,
                                scratch,
                                cells,
                                solids,
                                material,
                                ping,
                                pong,
                                map.resource.Get(),
                                state.resource.Get(),
                                list.resource.Get(),
                                rows.resource.Get(),
                                counters.resource.Get(),
                                arguments.resource.Get(),
                                importance ? importance : rows.resource.Get()};
    for (uint32_t i = 0; i < 14; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 2, resources[i]->GetGPUVirtualAddress());
    auto direct = [&](uint32_t p, uint32_t groups) {
        cmd->SetPipelineState(pipelines[p].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    direct(0, 1);
    direct(1, (coarse.w + 63) / 64);
    direct(2, (grid.w + 127) / 128);
    direct(3, 1);
    direct(4, (faceCount + 127) / 128);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto indirect = [&](uint32_t p) {
        cmd->SetPipelineState(pipelines[p].Get());
        cmd->ExecuteIndirect(dispatch.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
        gpu::uav(cmd);
    };
    indirect(5);
    ID3D12Resource *pressure[]{ping, pong};
    uint32_t current = 0;
    if (multigrid) {
        multigrid->solve(cmd, rows.resource.Get(), map.resource.Get(), cells, list.resource.Get(),
                         counters.resource.Get(), ping, cutExact.resource.Get());
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, frame->GetGPUVirtualAddress());
        if (cutProjection) {
            cmd->SetComputeRootUnorderedAccessView(17, cut.fineVolume->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(18, cut.pressureArea->GetGPUVirtualAddress());
            cmd->SetComputeRoot32BitConstant(19, (cut.swept ? 1u : 0u) | (cut.timeCentered ? 2u : 0u), 0);
            cmd->SetComputeRootUnorderedAccessView(20, cutExact.resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(21, cutFlux.resource->GetGPUVirtualAddress());
        }
        cmd->SetComputeRootUnorderedAccessView(16, multigrid->pressureResource()->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstants(1, 12, constants, 0);
        for (uint32_t i = 0; i < 14; ++i)
            cmd->SetComputeRootUnorderedAccessView(i + 2, resources[i]->GetGPUVirtualAddress());
    } else
        for (uint32_t i = 0; i < iterations; ++i) {
            cmd->SetComputeRootUnorderedAccessView(7, pressure[current]->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(8, pressure[1 - current]->GetGPUVirtualAddress());
            indirect(6);
            current = 1 - current;
        }
    cmd->SetComputeRootUnorderedAccessView(7, pressure[current]->GetGPUVirtualAddress());
    direct(7, (faceCount + 127) / 128);
    direct(8, (coarse.w + 63) / 64);
    direct(9, (grid.w + 127) / 128);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, solves * 2 + 1);
    ++solves;
    resetPending = false;
    lastCells = cells;
    lastPressure = pressure[current];
    if (validateFrame) {
        copySnapshot(cmd, faces, offset, uint64_t(faceCount) * 16);
        copySnapshot(cmd, map.resource.Get(), offset, uint64_t(grid.w) * 4);
        copySnapshot(cmd, state.resource.Get(), offset, uint64_t(coarse.w) * 4);
        copySnapshot(cmd, rows.resource.Get(), offset, uint64_t(grid.w) * sizeof(Row));
        copySnapshot(cmd, multigrid ? multigrid->pressureResource() : pressure[current], offset,
                     uint64_t(grid.w) * (multigrid ? 8 : 4));
        copySnapshot(cmd, cells, offset, uint64_t(grid.w) * 16);
        copySnapshot(cmd, solids, offset, uint64_t(grid.w) * 16);
        copySnapshot(cmd, material, offset, uint64_t(grid.w) * 16);
        if (cutProjection) {
            copySnapshot(cmd, cut.fineVolume, offset, uint64_t(grid.w) * 8);
            copySnapshot(cmd, cut.pressureArea, offset, uint64_t(faceCount) * 4);
            if (cutExact.resource)
                copySnapshot(cmd, cutExact.resource.Get(), offset,
                             uint64_t(grid.w) * sizeof(FluidCutPressureRow));
            copySnapshot(cmd, cutFlux.resource.Get(), offset, uint64_t(faceCount) * 8);
        }
    }
    return current;
}
void FluidMac::applyCapacity(ID3D12GraphicsCommandList *cmd, ID3D12Resource *frame, ID3D12Resource *faces,
                             ID3D12Resource *cells, ID3D12Resource *correctedFlux) {
    if (!cutProjection || !solves || !correctedFlux)
        throw std::runtime_error("Capacity application requires a completed cut MAC projection");
    gpu::Event event(cmd, L"Fluid / apply capacity correction to canonical MAC and APIC cache");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, frame->GetGPUVirtualAddress());
    XMUINT4 constants[]{coarse, bricks, {0, 0, 0, 0}};
    cmd->SetComputeRoot32BitConstants(1, 12, constants, 0);
    cmd->SetComputeRootUnorderedAccessView(2, faces->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, cells->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(9, map.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(10, state.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(17, cut.fineVolume->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(18, cut.pressureArea->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstant(19, (cut.swept ? 1u : 0u) | (cut.timeCentered ? 2u : 0u), 0);
    cmd->SetComputeRootUnorderedAccessView(21, cutFlux.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(22, correctedFlux->GetGPUVirtualAddress());
    cmd->SetPipelineState(pipelines[10].Get());
    cmd->Dispatch((faceCount + 127) / 128, 1, 1);
    gpu::uav(cmd);
    // Reuse the compatible 2:1 interior prolongation, not eight independent
    // child pressure corrections. FLIP's pre-force Faces.y stays unchanged.
    cmd->SetPipelineState(pipelines[8].Get());
    cmd->Dispatch((coarse.w + 63) / 64, 1, 1);
    gpu::uav(cmd);
    // Refresh displayed fine-cell divergence from the actual applied flux.
    // The pressure display remains the original physical solve's pressure.
    cmd->SetComputeRootUnorderedAccessView(7, lastPressure->GetGPUVirtualAddress());
    cmd->SetPipelineState(pipelines[9].Get());
    cmd->Dispatch((grid.w + 127) / 128, 1, 1);
    gpu::uav(cmd);
}
void FluidMac::recordReadback(ID3D12GraphicsCommandList *cmd) {
    if (!solves)
        return;
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, solves * 2, readback.resource.Get(),
                          0);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 256, counters.resource.Get(), 0, sizeof(metrics));
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void FluidMac::collect(uint64_t frequency) {
    gpuMs = 0;
    if (!solves)
        return;
    void *data;
    D3D12_RANGE range{0, 320}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Adaptive MAC readback");
    auto t = static_cast<const uint64_t *>(data);
    for (uint32_t i = 0; i < solves; ++i)
        gpuMs += double(t[i * 2 + 1] - t[i * 2]) * 1000 / frequency;
    memcpy(metrics.data(), t + 32, sizeof(metrics));
    readback.resource->Unmap(0, &written);
    if (metrics[3] || metrics[0] > grid.w || metrics[1] > coarse.w)
        throw std::runtime_error("Invalid adaptive MAC operator/counters");
    ++measuredFrames;
    totalMs += gpuMs;
    peakCoarse = std::max(peakCoarse, uint64_t(metrics[1]));
    peakJunctions = std::max(peakJunctions, uint64_t(metrics[2]));
    if (validateFrame)
        validateSnapshot();
    if (multigrid)
        multigrid->collect();
}
void FluidMac::report(std::ostream &out) const {
    out << "{\"mode\":\"mixed-mac-2to1\",\"solver\":\""
        << (multigrid ? "multigrid-pcg" : "absolute-row-relaxation") << "\",\"iterations\":" << iterations
        << ",\"cutPressure\":" << (cutProjection ? "true" : "false") << ",\"leaves\":" << metrics[0]
        << ",\"coarseLeaves\":" << metrics[1] << ",\"junctionFaces\":" << metrics[2]
        << ",\"peakCoarseLeaves\":" << peakCoarse << ",\"peakJunctionFaces\":" << peakJunctions
        << ",\"invalid\":" << metrics[3] << ",\"finePromotions\":" << metrics[4]
        << ",\"coarseDemotions\":" << metrics[5] << ",\"lastMs\":" << gpuMs
        << ",\"meanMs\":" << totalMs / std::max(uint64_t(1), measuredFrames)
        << ",\"allocatedBytes\":" << allocatedBytes << ",\"matrixError\":" << matrixError
        << ",\"rhsDivergenceError\":" << rhsDivergenceError << ",\"fluxError\":" << fluxError
        << ",\"prolongationError\":" << prolongationError << ",\"residualMismatch\":" << residualMismatch
        << ",\"velocityCacheDivergence\":" << velocityCacheDivergence
        << ",\"velocityCacheFluxError\":" << velocityCacheFluxError
        << ",\"preciseMatrixError\":" << preciseMatrixError
        << ",\"canonicalDivergence\":" << canonicalDivergence << ",\"auditedFrames\":" << auditedFrames
        << ",\"auditedClosingCellSamples\":" << closingCellSamples
        << ",\"auditedClosingLiquidSamples\":" << closingLiquidSamples
        << ",\"auditedClosingConnectedSamples\":" << closingConnectedSamples
        << ",\"auditedClosingLiquidVolumeM3\":" << closingLiquidVolumeM3
        << ",\"validated\":" << (validated ? "true" : "false");
    if (multigrid) {
        out << ",\"multigrid\":";
        multigrid->report(out);
    }
    out << '}';
}
void FluidMac::drawDebug(ID3D12GraphicsCommandList *cmd, ID3D12Resource *frame) {
    if (!debugVisible || !lastCells)
        return;
    gpu::Event event(cmd, L"Fluid / DEBUG actual coarse MAC and fine T junctions");
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetPipelineState(debug.Get());
    cmd->SetGraphicsRootConstantBufferView(0, frame->GetGPUVirtualAddress());
    XMUINT4 constants[]{coarse, bricks, {0, 0, 0, 0}};
    cmd->SetGraphicsRoot32BitConstants(1, 12, constants, 0);
    cmd->SetGraphicsRootUnorderedAccessView(4, lastCells->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(9, map.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(10, state.resource->GetGPUVirtualAddress());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    cmd->DrawInstanced(24, grid.w, 0, 0);
}
} // namespace lab
