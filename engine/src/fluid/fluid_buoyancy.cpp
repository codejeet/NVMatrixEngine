#include "fluid_buoyancy.h"
#include <d3dcompiler.h>
#include <cstring>
namespace lab {
FluidBuoyancy::FluidBuoyancy(ID3D12Device *device, const std::filesystem::path &folder) {
    constants = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
    queries = gpu::buffer(device, capacity * 16, D3D12_HEAP_TYPE_UPLOAD);
    samples = gpu::buffer(device, capacity * 16, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          L"Buoyancy / water height and MAC flow");
    readback = gpu::buffer(device, capacity * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Buoyancy / small body samples only");
    D3D12_ROOT_PARAMETER p[6]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[2].ParameterType = p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[3].Descriptor.ShaderRegister = 1;
    p[4].ParameterType = p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[4].Descriptor.ShaderRegister = 4;
    p[5].Descriptor.ShaderRegister = 5;
    D3D12_ROOT_SIGNATURE_DESC desc{6, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Buoyancy root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Buoyancy root");
    auto code = gpu::bytes(folder / "shaders/BuoyancySample.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get();
    pipeline.CS = {code.data(), code.size()};
    gpu::check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&sample)), "Buoyancy sample PSO");
}
void FluidBuoyancy::record(ID3D12GraphicsCommandList *cmd, const FluidSystem &fluid,
                           const FluidSurface &surface, const std::vector<DirectX::XMFLOAT4> &positions) {
    if (positions.size() > capacity)
        throw std::runtime_error("Buoyancy query capacity exceeded");
    count = uint32_t(positions.size());
    if (!count)
        return;
    gpu::Event event(cmd, L"Buoyancy / canonical surface and MAC flow probes");
    const auto &d = fluid.description();
    auto view = fluid.gpuView();
    struct Constants {
        DirectX::XMFLOAT4 surface, minimum, maximum;
        DirectX::XMUINT4 bricks, grid, control;
    };
    Constants c{surface.minimumSpacing,
                {d.minimum.x, d.minimum.y, d.minimum.z, d.gridCellSize},
                {d.maximum.x, d.maximum.y, d.maximum.z, 0},
                surface.brickGrid,
                view.grid,
                {count, 0, 0, 0}};
    memcpy(constants.mapped, &c, sizeof(c));
    memcpy(queries.mapped, positions.data(), count * 16);
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, constants.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(1, queries.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, samples.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, view.faces->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(4, surface.fieldResource()->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(5, surface.mapResource()->GetGPUVirtualAddress());
    cmd->SetPipelineState(sample.Get());
    cmd->Dispatch((count + 31) / 32, 1, 1);
    gpu::transition(cmd, samples.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 0, samples.resource.Get(), 0, count * 16);
    gpu::transition(cmd, samples.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
std::vector<DirectX::XMFLOAT4> FluidBuoyancy::collect() {
    std::vector<DirectX::XMFLOAT4> result(count);
    if (!count)
        return result;
    void *data = nullptr;
    D3D12_RANGE range{0, count * 16};
    gpu::check(readback.resource->Map(0, &range, &data), "Buoyancy samples map");
    memcpy(result.data(), data, count * 16);
    D3D12_RANGE written{0, 0};
    readback.resource->Unmap(0, &written);
    for (const auto &s : result)
        if (!std::isfinite(s.x) || !std::isfinite(s.y) || !std::isfinite(s.z) || !std::isfinite(s.w))
            throw std::runtime_error("Non-finite GPU buoyancy sample");
    return result;
}
} // namespace lab
