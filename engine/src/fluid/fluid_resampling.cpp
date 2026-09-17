#include "fluid_resampling.h"
#include "fluid_system.h"
#include "fluid_complexity.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidResampling::FluidResampling(ID3D12Device *device, const std::filesystem::path &folder,
                                 const FluidSystemDesc &desc, XMUINT4 dimensions)
    : grid(dimensions), capacity(desc.maxParticles), cellSize(desc.gridCellSize),
      apic(desc.transfer == FluidTransfer::Apic), orderedAllocation(desc.deterministicBins) {
    auto make = [&](uint64_t size, const wchar_t *name) {
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    freeSlots = make(uint64_t(capacity) * 4, L"Resampling / recycled particle IDs");
    selected = make(uint64_t(grid.w) * 8, L"Resampling / race-free per-cell parent decisions");
    counters = make(256, L"Resampling / mass and sample counters");
    arguments = make(256, L"Resampling / conditional work and rebin dispatch");
    uniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Resampling / constants");
    readback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Resampling / counters and timing");
    D3D12_ROOT_PARAMETER p[18]{};
    for (uint32_t i = 0; i < 2; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[i].Descriptor.ShaderRegister = i;
    }
    const uint32_t registers[]{0, 2, 4, 12, 13, 15, 16, 17, 18, 19};
    for (uint32_t i = 0; i < 10; ++i) {
        p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i + 2].Descriptor.ShaderRegister = registers[i];
    }
    for (uint32_t i = 0; i < 2; ++i) {
        p[i + 12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[i + 12].Descriptor.ShaderRegister = i + 1;
    }
    for (uint32_t i = 14; i < 18; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i + 6;
    }
    D3D12_ROOT_SIGNATURE_DESC r{18, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Resampling root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Resampling root");
    const char *names[]{"ResampleClear", "ResampleMerge", "ResampleSelect", "ResampleFree",
                        "ResampleSplit", "ResampleCount", "ResampleRebin",  "ResamplePlan"};
    if (orderedAllocation) {
        names[3] = "ResampleFreeOrdered";
        names[4] = "ResampleSplitOrdered";
    }
    for (uint32_t i = 0; i < 8; ++i) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 2;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Resampling timestamps");
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &argument, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch)),
               "Resampling indirect dispatch");
}
void FluidResampling::setImportance(const FluidComplexityGpuView &view) {
    importance = view.state;
    importanceArguments = view.arguments;
    bricks = view.grid;
}
void FluidResampling::record(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view,
                             ID3D12Resource *frameConstants, ID3D12Resource *solids, uint32_t issued,
                             bool reset, bool advancing, bool validate) {
    gpu::Event event(cmd, L"Fluid / conservative importance-driven particle resampling");
    changed = importance && !reset && advancing;
    expectedMass = issued;
    XMUINT4 c[]{bricks,
                {issued,
                 (importance && !reset ? 1u : 0u) | (forcedFine ? 2u : 0u) | (orderedAllocation ? 4u : 0u),
                 frames && !reset ? 1u : 0u, view.interiorEnabled ? 1u : 0u}};
    memcpy(uniforms.mapped, c, sizeof(c));
    validPending = validate;
    if (validate && !validation.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(view.particles->GetDevice(IID_PPV_ARGS(&device)), "Resampling device");
        validation = gpu::buffer(device.Get(), 2 * view.particles->GetDesc().Width, D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                                 L"Resampling / opt-in before-after invariant snapshots");
    }
    auto snapshot = [&](uint64_t offset) {
        gpu::transition(cmd, view.particles, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(validation.resource.Get(), offset, view.particles, 0,
                              view.particles->GetDesc().Width);
        gpu::transition(cmd, view.particles, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    if (validate)
        snapshot(0);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, frameConstants->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(1, uniforms.resource->GetGPUVirtualAddress());
    ID3D12Resource *resources[]{view.particles,
                                view.offsets,
                                view.indices,
                                view.previousPositions,
                                solids,
                                view.cellQuanta,
                                importance ? importance : selected.resource.Get(),
                                freeSlots.resource.Get(),
                                counters.resource.Get(),
                                selected.resource.Get()};
    for (uint32_t i = 0; i < 10; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 2, resources[i]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(12, view.colliderAddress);
    cmd->SetComputeRootShaderResourceView(13, view.meshPhi->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(14, arguments.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(
        15, (importanceArguments ? importanceArguments : arguments.resource.Get())->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(16, view.interior->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(17, view.interiorTotals->GetGPUVirtualAddress());
    auto pass = [&](uint32_t stage, uint32_t groups) {
        cmd->SetPipelineState(pipelines[stage].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    if (changed)
        pass(7, 1);
    pass(0, 1);
    if (changed) {
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        for (uint32_t stage = 1; stage <= 4; ++stage) {
            cmd->SetPipelineState(pipelines[stage].Get());
            cmd->ExecuteIndirect(dispatch.Get(), 1, arguments.resource.Get(), 72 + (stage - 1) * 12, nullptr,
                                 0);
            gpu::uav(cmd);
        }
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    pass(5, (capacity + 255) / 256);
    pass(6, 1);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    if (validate)
        snapshot(view.particles->GetDesc().Width);
}
void FluidResampling::recordReadback(ID3D12GraphicsCommandList *cmd) {
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 0, counters.resource.Get(), 0, 64);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.resource.Get(), 64);
}
void FluidResampling::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE r{0, 80}, written{0, 0};
    gpu::check(readback.resource->Map(0, &r, &data), "Resampling metrics map");
    memcpy(counts.data(), data, 64);
    const auto *t = reinterpret_cast<const uint64_t *>(static_cast<const char *>(data) + 64);
    lastMs = double(t[1] - t[0]) * 1000 / frequency;
    totalMs += lastMs;
    ++frames;
    merges += counts[2];
    splits += counts[3];
    rejected += counts[4];
    starved += counts[5];
    binRebuilds += counts[13];
    readback.resource->Unmap(0, &written);
    if (counts[7] + counts[14] != expectedMass * 16 || counts[6] > capacity ||
        counts[8] + counts[9] + counts[10] + counts[11] != counts[6])
        throw std::runtime_error("Adaptive particle mass/sample GPU accounting failed");
    if (!validPending)
        return;
    D3D12_RANGE range{0, size_t(validation.resource->GetDesc().Width)};
    gpu::check(validation.resource->Map(0, &range, &data), "Resampling invariants map");
    // Double-precision independent reduction, not an in-shader assertion that
    // merely repeats the implementation. APIC angular moment includes affine D.
    struct Totals {
        double mass = 0, energy = 0;
        std::array<double, 3> linear{}, angular{};
    };
    auto totals = [&](const FluidParticle *p) {
        Totals sum;
        const double D = apic ? .25 * cellSize * cellSize : 0;
        for (uint32_t i = 0; i < capacity; ++i) {
            if (!p[i].velocityFlags.w)
                continue;
            const double m = p[i].apic0.w;
            const float *x = &p[i].positionRadius.x, *v = &p[i].velocityFlags.x;
            const float *C[]{&p[i].apic0.x, &p[i].apic1.x, &p[i].apic2.x};
            sum.mass += m;
            for (int a = 0; a < 3; ++a) {
                int b = (a + 1) % 3, c = (a + 2) % 3;
                sum.linear[a] += m * v[a];
                sum.angular[a] += m * (double(x[b]) * v[c] - double(x[c]) * v[b] + D * (C[c][b] - C[b][c]));
                sum.energy +=
                    .5 * m *
                    (double(v[a]) * v[a] +
                     D * (double(C[a][0]) * C[a][0] + double(C[a][1]) * C[a][1] + double(C[a][2]) * C[a][2]));
            }
        }
        return sum;
    };
    uint64_t stride = std::max(uint64_t(256), uint64_t(capacity) * sizeof(FluidParticle));
    const auto before = totals(static_cast<const FluidParticle *>(data));
    const auto after =
        totals(reinterpret_cast<const FluidParticle *>(static_cast<const char *>(data) + stride));
    massError = std::abs(after.mass - before.mass);
    linearError = angularError = 0;
    for (int a = 0; a < 3; ++a) {
        linearError =
            std::max(linearError, std::abs(after.linear[a] - before.linear[a]) / std::max(1.0, before.mass));
        angularError = std::max(angularError,
                                std::abs(after.angular[a] - before.angular[a]) / std::max(1.0, before.mass));
    }
    energyIncrease = std::max(0.0, after.energy - before.energy) / std::max(1.0, before.energy);
    validation.resource->Unmap(0, &written);
    validPending = false;
    if (massError != 0 || linearError > 2e-6 || angularError > 2e-6 || energyIncrease > 5e-6)
        throw std::runtime_error("Adaptive particle resampling failed mass/momentum/energy invariants");
    validated = true;
}
void FluidResampling::report(std::ostream &out) const {
    out << "{\"enabled\":true,\"validated\":" << (validated ? "true" : "false") << ",\"frames\":" << frames
        << ",\"activeSamples\":" << counts[6] << ",\"massUnits\":" << (counts[7] + counts[14]) / 16.0
        << ",\"particleMassUnits\":" << counts[7] / 16.0 << ",\"merged\":" << merges
        << ",\"split\":" << splits << ",\"rejected\":" << rejected << ",\"starved\":" << starved
        << ",\"lastMerged\":" << counts[2] << ",\"lastSplit\":" << counts[3]
        << ",\"binRebuilds\":" << binRebuilds << ",\"protectedCells\":" << counts[12]
        << ",\"samplesByRequestedLod\":[" << counts[8] << ',' << counts[9] << ',' << counts[10] << ','
        << counts[11] << ']' << ",\"lastMs\":" << lastMs << ",\"meanMs\":" << (frames ? totalMs / frames : 0)
        << ",\"massError\":" << massError << ",\"linearErrorPerMass\":" << linearError
        << ",\"angularErrorPerMass\":" << angularError << ",\"relativeEnergyIncrease\":" << energyIncrease
        << '}';
}
} // namespace lab
