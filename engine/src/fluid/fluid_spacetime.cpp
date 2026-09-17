#include "fluid_spacetime.h"
#include "fluid_uniforms.h"
#include <d3dcompiler.h>
#include <cmath>

namespace lab {
using Microsoft::WRL::ComPtr;
FluidSpacetime::FluidSpacetime(ID3D12Device *device, const std::filesystem::path &folder,
                               DirectX::XMUINT4 dimensions, uint32_t count)
    : grid(dimensions), capacity(count) {
    if (!grid.x || !grid.y || !grid.z || uint64_t(grid.x) * grid.y * grid.z != grid.w || grid.w > 1048576 ||
        !count || count > 1000000)
        throw std::runtime_error("Invalid spacetime grid/capacity");
    const uint64_t faces = 3ull * (grid.x + 1) * (grid.y + 1) * (grid.z + 1);
    if (faces > 65535ull * 128)
        throw std::runtime_error("Spacetime face dispatch exceeds limit");
    faceCount = uint32_t(faces);
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    times = make(uint64_t(capacity) * 4, L"Fluid spacetime / particle residual seconds");
    render = make(uint64_t(capacity) * 80, L"Fluid spacetime / synchronized render cache");
    phase = make(uint64_t(faceCount) * 8, L"Fluid spacetime / face phase and inverse density");
    counters = make(16, L"Fluid spacetime / sticky numerical diagnostics");
    D3D12_ROOT_PARAMETER p[10]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {3, 0, sizeof(Constants) / 4};
    const uint32_t registers[]{0, 2, 4, 6, 38, 39, 40, 41};
    for (uint32_t i = 0; i < 8; i++) {
        p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i + 2].Descriptor.ShaderRegister = registers[i];
    }
    D3D12_ROOT_SIGNATURE_DESC d{10, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Spacetime root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Spacetime root");
    const char *names[]{"SpacetimeReset",  "SpacetimeSeed",        "FluidP2GSpacetime",
                        "SpacetimeAdvect", "SpacetimeSynchronize", "SpacetimePhaseFaces"};
    for (uint32_t i = 0; i < pipelines.size(); i++) {
        const auto bytes = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC s{};
        s.pRootSignature = root.Get();
        s.CS = {bytes.data(), bytes.size()};
        gpu::check(device->CreateComputePipelineState(&s, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
}
void FluidSpacetime::bind(ID3D12GraphicsCommandList *cmd, const View &v, bool requireBins) {
    const auto size = [](ID3D12Resource *r, uint64_t bytes) {
        if (!r || r->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || r->GetDesc().Width < bytes)
            throw std::runtime_error("Missing/undersized spacetime input");
    };
    size(v.frame, sizeof(FluidSimulationConstants));
    size(v.particles, uint64_t(capacity) * 80);
    size(v.faces, uint64_t(faceCount) * 16);
    if (requireBins) {
        size(v.offsets, (uint64_t(grid.w) + 1) * 4);
        size(v.indices, uint64_t(capacity) * 4);
    }
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, v.frame->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstants(1, sizeof(Constants) / 4, &constants, 0);
    ID3D12Resource *buffers[]{v.particles,
                              v.offsets,
                              v.indices,
                              v.faces,
                              times.resource.Get(),
                              render.resource.Get(),
                              counters.resource.Get(),
                              phase.resource.Get()};
    for (uint32_t i = 0; i < 8; i++)
        if (buffers[i])
            cmd->SetComputeRootUnorderedAccessView(i + 2, buffers[i]->GetGPUVirtualAddress());
}
void FluidSpacetime::pass(ID3D12GraphicsCommandList *cmd, uint32_t pipeline, uint32_t count) {
    cmd->SetPipelineState(pipelines[pipeline].Get());
    cmd->Dispatch((count + 127) / 128, 1, 1);
    gpu::uav(cmd);
}
void FluidSpacetime::reset(ID3D12GraphicsCommandList *cmd, const View &v) {
    gpu::Event e(cmd, L"Fluid / reset spacetime samples");
    bind(cmd, v);
    pass(cmd, 0, std::max(capacity, 4u));
    initialized = true;
}
void FluidSpacetime::seed(ID3D12GraphicsCommandList *cmd, const View &v, uint32_t first, uint32_t count) {
    if (!initialized || first > capacity || count > capacity - first)
        throw std::runtime_error("Invalid spacetime birth range/state");
    if (!count)
        return;
    constants.control.z = first;
    constants.control.w = count;
    gpu::Event e(cmd, L"Fluid / initialize newborn spacetime samples");
    bind(cmd, v);
    pass(cmd, 1, count);
}
void FluidSpacetime::deposit(ID3D12GraphicsCommandList *cmd, const View &v, float previousDt, float eta) {
    if (!initialized || !std::isfinite(previousDt) || previousDt <= 0 || !std::isfinite(eta) || eta <= 0 ||
        eta > 1)
        throw std::runtime_error("Invalid spacetime deposition time/phase scale");
    constants.time.x = previousDt;
    constants.time.w = eta;
    gpu::Event e(cmd, L"Fluid / 4D slab P2G and phase weights");
    bind(cmd, v, true);
    pass(cmd, 2, faceCount);
    pass(cmd, 5, faceCount);
}
void FluidSpacetime::advect(ID3D12GraphicsCommandList *cmd, const View &v, float dt, uint32_t step,
                            uint32_t seed, float strength) {
    if (!initialized || !std::isfinite(dt) || dt < 0 || !std::isfinite(strength) || strength < 0 ||
        strength > 1)
        throw std::runtime_error("Invalid spacetime advection time/jitter");
    if (dt == 0)
        return;
    constants.time.y = dt;
    constants.time.z = strength;
    constants.control.x = seed;
    constants.control.y = step;
    gpu::Event e(cmd, L"Fluid / residual-carrying RK3 spacetime advection");
    bind(cmd, v);
    pass(cmd, 3, capacity);
}
void FluidSpacetime::synchronize(ID3D12GraphicsCommandList *cmd, const View &v) {
    if (!initialized)
        throw std::runtime_error("Spacetime render synchronization needs reset");
    gpu::Event e(cmd, L"Fluid / synchronize render-only particle cache");
    bind(cmd, v);
    pass(cmd, 4, capacity);
}
} // namespace lab
