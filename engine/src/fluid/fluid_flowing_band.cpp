#include "fluid_flowing_band.h"
#include <d3dcompiler.h>
#include <cmath>

namespace lab {
using Microsoft::WRL::ComPtr;
FluidFlowingBand::FluidFlowingBand(ID3D12Device *device, const std::filesystem::path &folder, const Desc &d)
    : desc(d) {
    if (!d.fine.x || !d.fine.y || !d.fine.z || uint64_t(d.fine.x) * d.fine.y * d.fine.z != d.fine.w ||
        d.fine.w > 1048576 || !d.maxParticles || d.maxParticles > 1000000 || d.bandCells < 3 ||
        d.bandCells > 8 || d.padding > 2 || !d.promotionFrames || d.promotionFrames > 120 ||
        !std::isfinite(d.velocityError) || d.velocityError <= 0 || !std::isfinite(d.centroidError) ||
        d.centroidError <= 0 || !std::isfinite(d.covarianceError) || d.covarianceError <= 0 ||
        !std::isfinite(d.densityError) || d.densityError <= 0 || d.densityError >= .5f)
        throw std::runtime_error("Invalid flowing-band dimensions or error policy");
    D3D12_FEATURE_DATA_D3D12_OPTIONS features{};
    gpu::check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &features, sizeof(features)),
               "Flowing band FP64 support");
    if (!features.DoublePrecisionFloatShaderOps)
        throw std::runtime_error("Flowing band requires GPU FP64 ownership");
    auto &c = constants.coarse;
    c = {(d.fine.x + 1) / 2, (d.fine.y + 1) / 2, (d.fine.z + 1) / 2, 0};
    c.w = c.x * c.y * c.z;
    constants.policy = {d.bandCells, d.padding, d.promotionFrames, d.maxParticles};
    constants.tolerance = {d.velocityError, d.centroidError, d.covarianceError, d.densityError};
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    fineState = make(uint64_t(d.fine.w) * 32, L"Flowing band / joint owned fine-cell quantities");
    candidate = make(uint64_t(c.w) * sizeof(State), L"Flowing band / current error estimates");
    history = make(uint64_t(c.w) * sizeof(State), L"Flowing band / padded hysteretic decisions");
    request = make(uint64_t(c.w) * 4, L"Flowing band / exchange ownership requests");
    points = make(uint64_t(c.w) * siteStride * 16, L"Flowing band / geometry-validated restoration sites");
    pointCounts = make(uint64_t(c.w) * 4, L"Flowing band / restoration site counts");
    D3D12_ROOT_PARAMETER p[19]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {1, 0, sizeof(Constants) / 4};
    const uint32_t registers[]{0, 37, 2, 4, 13, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47};
    for (uint32_t i = 0; i < 15; i++) {
        p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i + 2].Descriptor.ShaderRegister = registers[i];
    }
    for (uint32_t i = 17; i < 19; i++) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[i].Descriptor.ShaderRegister = i - 16;
    }
    // 20 constant DWORDs + CBV + 17 root descriptors = 56 DWORDs.
    D3D12_ROOT_SIGNATURE_DESC r{19, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Flowing band root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Flowing band root");
    const char *names[]{"FlowingBandMass", "FlowingBandMeasure", "FlowingBandDecide", "FlowingBandSites",
                        "FlowingBandRestoreRequests"};
    for (uint32_t i = 0; i < pipelines.size(); i++) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
        state.pRootSignature = root.Get();
        state.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
}
void FluidFlowingBand::record(ID3D12GraphicsCommandList *cmd, const View &v, bool reset, bool force) {
    if (!initialized && !reset)
        throw std::runtime_error("Flowing-band history requires an initial reset");
    const auto size = [](ID3D12Resource *r, uint64_t bytes) {
        if (!r || r->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || r->GetDesc().Width < bytes)
            throw std::runtime_error("Missing or undersized flowing-band GPU input");
    };
    const auto &f = desc.fine;
    size(v.frame, 368);
    size(v.particles, uint64_t(desc.maxParticles) * 80);
    size(v.quantities, uint64_t(desc.maxParticles) * 32);
    size(v.offsets, (uint64_t(f.w) + 1) * 4);
    size(v.indices, uint64_t(desc.maxParticles) * 4);
    size(v.gridQuantity, uint64_t(constants.coarse.w) * 32);
    size(v.fineVolume, uint64_t(f.w) * 8);
    size(v.coarseVolume, uint64_t(constants.coarse.w) * 16);
    size(v.solidGrid, uint64_t(f.w) * 16);
    size(v.meshPhi, 4);
    if (!v.colliders)
        throw std::runtime_error("Flowing band requires the current collider binding");
    if (v.importance) {
        const auto &g = v.importanceGrid;
        if (!g.x || !g.y || !g.z || uint64_t(g.x) * g.y * g.z != g.w)
            throw std::runtime_error("Invalid flowing-band importance dimensions");
        size(v.importance, uint64_t(g.w) * 64);
    }
    constants.importanceGrid = v.importanceGrid;
    constants.control = {reset ? 1u : 0u, force ? 1u : 0u, v.importance ? 1u : 0u, 0};
    gpu::Event event(cmd, L"Fluid / flowing ownership band and reconstruction sites");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, v.frame->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    ID3D12Resource *buffers[]{v.particles,
                              v.quantities,
                              v.offsets,
                              v.indices,
                              v.solidGrid,
                              v.gridQuantity,
                              v.fineVolume,
                              v.coarseVolume,
                              v.importance ? v.importance : candidate.resource.Get(),
                              fineState.resource.Get(),
                              candidate.resource.Get(),
                              history.resource.Get(),
                              request.resource.Get(),
                              points.resource.Get(),
                              pointCounts.resource.Get()};
    for (uint32_t i = 0; i < 15; i++)
        cmd->SetComputeRootUnorderedAccessView(i + 2, buffers[i]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(17, v.colliders);
    cmd->SetComputeRootShaderResourceView(18, v.meshPhi->GetGPUVirtualAddress());
    for (uint32_t i = 0; i < (force ? 2u : 4u); i++) {
        const uint32_t stage = force ? (i == 0 ? 4 : 3) : i;
        cmd->SetPipelineState(pipelines[stage].Get());
        cmd->Dispatch(((stage == 0 ? f.w : constants.coarse.w) + 63) / 64, 1, 1);
        gpu::uav(cmd);
    }
    initialized = true;
}
} // namespace lab
