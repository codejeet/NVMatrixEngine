#include "fluid_particle_grid_exchange.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidParticleGridExchange::FluidParticleGridExchange(ID3D12Device *device,
                                                     const std::filesystem::path &folder, XMUINT4 fine,
                                                     uint32_t particles, float volume, float radius,
                                                     bool cudaShared)
    : FluidParticleGridExchange(device, folder, fine, particles, volume, radius,
                                CudaSharing{cudaShared, false}) {}
FluidParticleGridExchange::FluidParticleGridExchange(ID3D12Device *device,
                                                     const std::filesystem::path &folder, XMUINT4 fine,
                                                     uint32_t particles, float volume, float radius,
                                                     CudaSharing sharing) {
    if (!fine.x || !fine.y || !fine.z || uint64_t(fine.x) * fine.y * fine.z != fine.w || fine.w > 1048576 ||
        !particles || particles > 1000000 || !std::isfinite(volume) || volume <= 0 ||
        !std::isfinite(radius) || radius <= 0)
        throw std::runtime_error("Invalid particle/grid exchange dimensions or physical scale");
    D3D12_FEATURE_DATA_D3D12_OPTIONS features{};
    gpu::check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &features, sizeof(features)),
               "Particle/grid exchange FP64 support");
    if (!features.DoublePrecisionFloatShaderOps)
        throw std::runtime_error("Particle/grid exchange requires GPU FP64");
    constants.fine = fine;
    constants.coarse = {(fine.x + 1) / 2, (fine.y + 1) / 2, (fine.z + 1) / 2, 0};
    auto &c = constants.coarse;
    c.w = c.x * c.y * c.z;
    constants.work = {particles, 0, 0, 0};
    constants.physical = {volume, radius, 0, 0};
    auto make = [&](uint64_t size, const wchar_t *name, bool shared = false) {
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name,
                           shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE);
    };
    quantities = make(uint64_t(particles) * 32,
                      L"Fluid exchange / authoritative particle volume and momentum", sharing.particles);
    references = make(uint64_t(particles) * 16, L"Fluid exchange / previous cached particle velocity",
                      sharing.particles);
    for (auto &g : grid)
        g = make(uint64_t(c.w) * 32, L"Fluid exchange / authoritative grid volume and momentum",
                 sharing.grid);
    freeIds = make(uint64_t(particles) * 4, L"Fluid exchange / compact reusable particle slots");
    control = make(256, L"Fluid exchange / transaction counters", sharing.particles || sharing.grid);
    D3D12_ROOT_PARAMETER p[14]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, sizeof(Constants) / 4};
    for (uint32_t i = 1; i < 14; i++) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{14, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Exchange root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Exchange root");
    const char *names[]{"ExchangeReset",   "ExchangeClear", "ExchangeSeed",   "ExchangeVelocityDelta",
                        "ExchangeDeposit", "ExchangeFree",  "ExchangeRestore"};
    for (uint32_t i = 0; i < pipelines.size(); i++) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
}
void FluidParticleGridExchange::bind(ID3D12GraphicsCommandList *cmd, const View &v, uint32_t transaction) {
    if (!v.particles || !v.previousPositions || v.siteStride > 1024 ||
        (transaction == 1 && (!v.offsets || !v.indices || !v.requests || !v.capacity)) ||
        (transaction == 2 && (!v.requests || !v.sites || !v.siteCounts || !v.siteStride)))
        throw std::runtime_error("Incomplete particle/grid exchange GPU view");
    if (v.reusableParticleLimit != UINT_MAX && v.reusableParticleLimit > constants.work.x)
        throw std::runtime_error("Exchange reusable particle prefix exceeds capacity");
    const auto size = [](ID3D12Resource *r, uint64_t minimum) {
        if (!r)
            return; // Seed/velocity-only integration needs no fabricated exchange geometry.
        const auto d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width < minimum)
            throw std::runtime_error("Undersized particle/grid exchange GPU buffer");
    };
    size(v.particles, uint64_t(constants.work.x) * 80);
    size(v.offsets, (uint64_t(constants.fine.w) + 1) * 4);
    size(v.indices, uint64_t(constants.work.x) * 4);
    size(v.requests, uint64_t(constants.coarse.w) * 4);
    size(v.capacity, uint64_t(constants.coarse.w) * 16);
    size(v.sites, uint64_t(constants.coarse.w) * v.siteStride * 16);
    size(v.siteCounts, uint64_t(constants.coarse.w) * 4);
    size(v.previousPositions, uint64_t(constants.work.x) * 16);
    constants.work.w = v.siteStride;
    constants.allocation.x = v.reusableParticleLimit == UINT_MAX ? constants.work.x : v.reusableParticleLimit;
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    ID3D12Resource *buffers[]{v.particles,
                              quantities.resource.Get(),
                              references.resource.Get(),
                              gridRead(),
                              v.offsets,
                              v.indices,
                              v.requests,
                              v.capacity,
                              v.sites,
                              v.siteCounts,
                              freeIds.resource.Get(),
                              control.resource.Get(),
                              v.previousPositions};
    for (uint32_t i = 0; i < 13; i++)
        if (buffers[i])
            cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
}
void FluidParticleGridExchange::pass(ID3D12GraphicsCommandList *cmd, uint32_t stage, uint32_t groups) {
    cmd->SetPipelineState(pipelines[stage].Get());
    cmd->Dispatch(groups, 1, 1);
    gpu::uav(cmd);
}
void FluidParticleGridExchange::begin(ID3D12GraphicsCommandList *cmd, const View &v, bool reset) {
    if (!initialized && !reset)
        throw std::runtime_error("Particle/grid exchange requires an initial reset");
    gpu::Event event(cmd, L"Fluid / begin particle-grid ownership exchange");
    bind(cmd, v); // validate before changing persistent ownership selection
    if (reset) {
        current = 0;
        cmd->SetComputeRootUnorderedAccessView(4, gridRead()->GetGPUVirtualAddress());
    }
    restored = deposited = false;
    pass(cmd, 1, 1);
    if (reset) {
        const uint32_t groups = (std::max(constants.work.x, constants.coarse.w) + 127) / 128;
        pass(cmd, 0, groups);
        cmd->SetComputeRootUnorderedAccessView(4, gridWrite()->GetGPUVirtualAddress());
        pass(cmd, 0, groups);
        bind(cmd, v);
    }
    initialized = true;
}
void FluidParticleGridExchange::seed(ID3D12GraphicsCommandList *cmd, const View &v, uint32_t first,
                                     uint32_t count) {
    if (!initialized || deposited || restored)
        throw std::runtime_error("Particle births must precede exchange transactions");
    if (first > constants.work.x || count > constants.work.x - first)
        throw std::runtime_error("Particle/grid exchange birth range exceeds capacity");
    if (!count)
        return;
    gpu::Event event(cmd, L"Fluid / initialize new particle ownership");
    constants.work.y = first;
    constants.work.z = count;
    bind(cmd, v);
    pass(cmd, 2, (count + 127) / 128);
}
void FluidParticleGridExchange::velocityDelta(ID3D12GraphicsCommandList *cmd, const View &v) {
    if (!initialized)
        throw std::runtime_error("Particle/grid exchange requires an initial reset");
    gpu::Event event(cmd, L"Fluid / import particle solver velocity increments");
    bind(cmd, v);
    pass(cmd, 3, (constants.work.x + 127) / 128);
}
void FluidParticleGridExchange::deposit(ID3D12GraphicsCommandList *cmd, const View &v) {
    if (!initialized || deposited || restored)
        throw std::runtime_error("Re-bin and begin another phase before depositing restored particles");
    gpu::Event event(cmd, L"Fluid / retire particles into authoritative grid inventory");
    bind(cmd, v, 1);
    pass(cmd, 4, (constants.coarse.w + 63) / 64);
    deposited = true;
}
void FluidParticleGridExchange::restore(ID3D12GraphicsCommandList *cmd, const View &v) {
    if (!initialized || restored)
        throw std::runtime_error("Particle/grid free list may only be consumed once per exchange phase");
    gpu::Event event(cmd, L"Fluid / restore particle detail from authoritative grid inventory");
    bind(cmd, v, 2);
    pass(cmd, 5, (constants.work.x + 127) / 128);
    pass(cmd, 6, (constants.coarse.w + 63) / 64);
    restored = true;
}
} // namespace lab
