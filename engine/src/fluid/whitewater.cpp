#include "whitewater.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

namespace lab {
using Microsoft::WRL::ComPtr;
Whitewater::Whitewater(ID3D12Device5 *device, const std::filesystem::path &folder,
                       const FluidSurface &surface, const FluidSystem &fluid) {
    auto make = [&](uint64_t size, const wchar_t *name) {
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    particles = make(capacity * sizeof(Particle), L"Whitewater foam bubble spray particles");
    aabbs = make(capacity * sizeof(D3D12_RAYTRACING_AABB), L"Bubble and spray procedural AABBs");
    sourceCapacity = fluid.description().maxParticles;
    sources = make(uint64_t(sourceCapacity) * sizeof(uint32_t), L"Whitewater live surface source IDs");
    const auto grid = surface.brickGrid;
    const uint64_t nodes = uint64_t(grid.x * 8 + 1) * (grid.y * 8 + 1) * (grid.z * 8 + 1);
    if (!nodes || nodes > 65535ull * 128)
        throw std::runtime_error("Surface foam exceeds dispatch capacity");
    foamNodes = uint32_t(nodes);
    foam = make(nodes * sizeof(uint32_t), L"Foam entrainment source / fixed point");
    for (auto &history : foamHistory)
        history = make(nodes * sizeof(DirectX::XMFLOAT4), L"Advected foam density and material displacement");
    counters = make(256, L"Whitewater phase and lifecycle diagnostics");
    constants = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                            D3D12_RESOURCE_STATE_GENERIC_READ, L"Whitewater constants");
    readback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Whitewater timings and counts");
    D3D12_ROOT_PARAMETER p[15]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (uint32_t i = 1; i <= 6; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    for (uint32_t i = 7; i <= 10; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[i].Descriptor.ShaderRegister = i - 3;
    }
    for (uint32_t i = 11; i < 15; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 5;
    }
    D3D12_ROOT_SIGNATURE_DESC r{15, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Whitewater root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Whitewater root");
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(name) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
    };
    compute("WhitewaterClear", clear);
    compute("WhitewaterSources", findSources);
    compute("WhitewaterUpdate", update);
    compute("FoamClear", foamClear);
    compute("FoamSplat", foamSplat);
    compute("FoamTransport", foamTransport);
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 3;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Whitewater timestamps");
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.AABBs.AABBCount = capacity;
    geometry.AABBs.AABBs = {aabbs.resource->GetGPUVirtualAddress(), sizeof(D3D12_RAYTRACING_AABB)};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.NumDescs = 1;
    input.pGeometryDescs = &geometry;
    input.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&input, &info);
    blas = gpu::buffer(device, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Whitewater procedural BLAS");
    scratch = make(info.ScratchDataSizeInBytes, L"Whitewater BLAS scratch");
}
void Whitewater::record(ID3D12GraphicsCommandList4 *cmd, const FluidSystem &fluid,
                        const FluidSurface &surface) {
    gpu::Event event(cmd, L"Whitewater / advect classify spawn and BLAS");
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    if (readable)
        for (auto r : {particles.resource.Get(), aabbs.resource.Get(), foamResource()})
            gpu::transition(cmd, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto &desc = fluid.description();
    const auto v = fluid.gpuView();
    Uniforms c{
        surface.minimumSpacing,
        {desc.minimum.x, desc.minimum.y, desc.minimum.z, desc.gridCellSize},
        {desc.maximum.x, desc.maximum.y, desc.maximum.z, float(v.colliderCount)},
        surface.brickGrid,
        v.grid,
        {fluid.activeParticles, uint32_t(fluid.stepCount), !recorded || fluid.resetThisFrame ? 1u : 0u,
         0},
        {fluid.advancedSeconds, desc.gravity.x, desc.gravity.y, desc.gravity.z}};
    memcpy(constants.mapped, &c, sizeof(c));
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, constants.resource->GetGPUVirtualAddress());
    ID3D12Resource *uavs[] = {particles.resource.Get(),
                              aabbs.resource.Get(),
                              counters.resource.Get(),
                              v.particles,
                              v.faces,
                              v.material};
    for (uint32_t i = 0; i < 6; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, uavs[i]->GetGPUVirtualAddress());
    ID3D12Resource *srvs[] = {surface.fieldResource(), surface.mapResource(), v.colliders, v.meshPhi};
    for (uint32_t i = 0; i < 4; ++i)
        cmd->SetComputeRootShaderResourceView(i + 7, srvs[i]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(11, foam.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(12, foamHistory[foamIndex].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(13, foamHistory[foamIndex ^ 1].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(14, sources.resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(clear.Get());
    cmd->Dispatch(1, 1, 1);
    gpu::uav(cmd);
    cmd->SetPipelineState(findSources.Get());
    cmd->Dispatch((fluid.activeParticles + 127) / 128, 1, 1);
    gpu::uav(cmd);
    cmd->SetPipelineState(update.Get());
    cmd->Dispatch(capacity / 128, 1, 1);
    gpu::uav(cmd);
    {
        gpu::Event coating(cmd, L"Foam / entrainment and persistent surface transport");
        cmd->SetPipelineState(foamClear.Get());
        cmd->Dispatch((foamNodes + 127) / 128, 1, 1);
        gpu::uav(cmd, foam.resource.Get());
        cmd->SetPipelineState(foamSplat.Get());
        cmd->Dispatch(capacity / 128, 1, 1);
        gpu::uav(cmd, foam.resource.Get());
        cmd->SetPipelineState(foamTransport.Get());
        cmd->Dispatch((foamNodes + 127) / 128, 1, 1);
        gpu::uav(cmd);
        foamIndex ^= 1;
    }
    for (auto r : {particles.resource.Get(), aabbs.resource.Get(), foamResource()})
        gpu::transition(cmd, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    readable = true;
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    {
        gpu::Event build(cmd, L"Whitewater / GPU procedural BLAS");
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        d.Inputs.NumDescs = 1;
        d.Inputs.pGeometryDescs = &geometry;
        d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        d.DestAccelerationStructureData = blas.resource->GetGPUVirtualAddress();
        d.ScratchAccelerationStructureData = scratch.resource->GetGPUVirtualAddress();
        cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        gpu::uav(cmd, blas.resource.Get());
    }
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    recorded = true;
}
void Whitewater::recordReadback(ID3D12GraphicsCommandList *cmd, bool validate) {
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, readback.resource.Get(), 0);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 24, counters.resource.Get(), 0, sizeof(counts));
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (validate) {
        if (!validation.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(particles.resource->GetDevice(IID_PPV_ARGS(&device)), "Whitewater validation device");
            validation = gpu::buffer(device.Get(), capacity * sizeof(Particle), D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                                     L"Whitewater test-only particle readback");
        }
        gpu::transition(cmd, particles.resource.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyResource(validation.resource.Get(), particles.resource.Get());
        gpu::transition(cmd, particles.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        validatePending = true;
    }
}
void Whitewater::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 24 + sizeof(counts)}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Whitewater counters map");
    auto t = static_cast<const uint64_t *>(data);
    simulationMs = double(t[1] - t[0]) * 1000 / frequency;
    blasMs = double(t[2] - t[1]) * 1000 / frequency;
    memcpy(counts.data(), static_cast<const char *>(data) + 24, sizeof(counts));
    readback.resource->Unmap(0, &written);
    if (counts[5] || counts[0] + counts[1] + counts[2] > capacity || counts[6] > foamNodes ||
        counts[7] > 4u * 65536 || counts[8] > sourceCapacity)
        throw std::runtime_error("Invalid whitewater state/capacity");
    if (validatePending) {
        range = {0, capacity * sizeof(Particle)};
        gpu::check(validation.resource->Map(0, &range, &data), "Whitewater particle validation");
        auto p = static_cast<const Particle *>(data);
        std::array<uint32_t, 3> phases{};
        bool bad = false;
        for (uint32_t i = 0; i < capacity; ++i) {
            const auto *f = reinterpret_cast<const float *>(&p[i]);
            for (uint32_t j = 0; j < 16; ++j)
                bad |= !std::isfinite(f[j]);
            if (p[i].velocityLife.w <= 0)
                continue;
            const float type = p[i].previousType.w;
            const auto &n = p[i].surfaceAge;
            if (n.w < 0 || (type == 1 && std::abs(n.x * n.x + n.y * n.y + n.z * n.z - 1) > .001f))
                bad = true;
            if (type < 1 || type > 3 || type != std::floor(type) || p[i].positionRadius.w <= 0 ||
                p[i].positionRadius.w > .04f)
                bad = true;
            else
                ++phases[uint32_t(type) - 1];
        }
        validation.resource->Unmap(0, &written);
        validatePending = false;
        if (bad || phases[0] != counts[0] || phases[1] != counts[1] || phases[2] != counts[2])
            throw std::runtime_error("Whitewater readback disagrees with finite phase/size/count contract");
        validated = true;
    }
}
void Whitewater::report(std::ostream &out) const {
    out << "{\"capacity\":" << capacity
        << ",\"foamRendering\":\"advected-grain-layer\",\"foamFieldBytes\":"
        << uint64_t(foamNodes) * (sizeof(uint32_t) + 2 * sizeof(DirectX::XMFLOAT4))
        << ",\"foamActiveNodes\":" << counts[6] << ",\"foamMaxDensity\":" << float(counts[7]) / 65536.f
        << ",\"foam\":" << counts[0] << ",\"bubbles\":" << counts[1] << ",\"spray\":" << counts[2]
        << ",\"bornLastFrame\":" << counts[3] << ",\"expiredLastFrame\":" << counts[4]
        << ",\"surfaceSourceParticles\":" << counts[8] << ",\"sourceIndexBytes\":" << uint64_t(sourceCapacity) * 4
        << ",\"invalid\":" << counts[5] << ",\"simulationMs\":" << simulationMs << ",\"blasMs\":" << blasMs
        << ",\"validated\":" << (validated ? "true" : "false") << "}";
}
} // namespace lab
