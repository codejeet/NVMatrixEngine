#include "fluid_interior.h"
#include "fluid_system.h"
#include "fluid_complexity.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidInterior::FluidInterior(ID3D12Device *device, const std::filesystem::path &folder,
                             const FluidSystemDesc &desc, XMUINT4 grid)
    : capacity(desc.maxParticles), cellSize(desc.gridCellSize), orderedAllocation(desc.deterministicBins) {
    if (desc.transfer != FluidTransfer::Apic || desc.ballisticTest || desc.transferTest || desc.materialTest)
        throw std::runtime_error("Coarse interior ownership currently requires normal APIC");
    constants.coarse = {(grid.x + 1) / 2, (grid.y + 1) / 2, (grid.z + 1) / 2, 0};
    constants.coarse.w = constants.coarse.x * constants.coarse.y * constants.coarse.z;
    constants.policy = {.025f, .05f, 0, 0};
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    cells = make(uint64_t(constants.coarse.w) * sizeof(FluidParticle),
                 L"Interior / authoritative coarse mass and affine momentum");
    counters = make(256, L"Interior / ownership and conserved mass counters");
    freeSlots = make(uint64_t(capacity) * 4, L"Interior / GPU restoration slots");
    arguments = make(256, L"Interior / conditional ownership rebin dispatch");
    active = make(uint64_t(constants.coarse.w) * 4, L"Interior / compact active coarse cells");
    readback = gpu::buffer(device, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Interior / statistics and timing");
    D3D12_ROOT_PARAMETER p[17]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {1, 0, 16};
    uint32_t registers[]{0, 2, 4, 6, 12, 13, 15, 22, 23, 24, 25, 26};
    for (uint32_t i = 0; i < 12; ++i) {
        p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i + 2].Descriptor.ShaderRegister = registers[i];
    }
    for (uint32_t i = 14; i < 16; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[i].Descriptor.ShaderRegister = i - 13;
    }
    p[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[16].Descriptor.ShaderRegister = 27;
    D3D12_ROOT_SIGNATURE_DESC r{17, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Interior root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Interior root");
    const char *names[]{"InteriorBegin",   "InteriorClearExchange", "InteriorFreeSlots", "InteriorRestore",
                        "InteriorDeposit", "InteriorAdvect",        "InteriorCount",     "InteriorPrepare"};
    if (orderedAllocation) {
        names[2] = "InteriorFreeSlotsOrdered";
        names[3] = "InteriorRestoreOrdered";
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
    q.Count = 64;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Interior timestamps");
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &a, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch)),
               "Interior indirect dispatch");
}
void FluidInterior::setImportance(const FluidComplexityGpuView &view) {
    importance = view.state;
    constants.bricks = view.grid;
}
void FluidInterior::bind(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *frame,
                         ID3D12Resource *solids) {
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, frame->GetGPUVirtualAddress());
    constants.policy.z = forcedFine ? 1.f : 0.f;
    cmd->SetComputeRoot32BitConstants(1, 16, &constants, 0);
    ID3D12Resource *buffers[]{view.particles,
                              view.offsets,
                              view.indices,
                              view.faces,
                              view.previousPositions,
                              solids,
                              view.cellQuanta,
                              cells.resource.Get(),
                              counters.resource.Get(),
                              freeSlots.resource.Get(),
                              arguments.resource.Get(),
                              importance ? importance : cells.resource.Get()};
    for (uint32_t i = 0; i < 12; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 2, buffers[i]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(14, view.colliderAddress);
    cmd->SetComputeRootShaderResourceView(15, view.meshPhi->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(16, active.resource->GetGPUVirtualAddress());
}
void FluidInterior::pass(ID3D12GraphicsCommandList *cmd, uint32_t stage, uint32_t groups) {
    cmd->SetPipelineState(pipelines[stage].Get());
    cmd->Dispatch(groups, 1, 1);
    gpu::uav(cmd);
}
void FluidInterior::begin(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *frame,
                          ID3D12Resource *solids, bool reset, uint32_t issued, bool validate, bool advancing,
                          bool solidsChanged) {
    queryCount = 0;
    validatePending = validate;
    changed = advancing || solidsChanged || reset || forcedFine != previousForcedFine;
    constants.policy.w = advancing ? 2.f : (changed ? 1.f : 0.f);
    previousForcedFine = forcedFine;
    constants.control = {issued, importance && !reset ? 1u : 0u, reset ? 1u : 0u, ++frameNumber};
    if (validate && !validation.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(cells.resource->GetDevice(IID_PPV_ARGS(&device)), "Interior device");
        snapshotStride = view.particles->GetDesc().Width + cells.resource->GetDesc().Width;
        validation =
            gpu::buffer(device.Get(), snapshotStride * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, L"Interior / opt-in ownership invariant snapshots");
    }
    gpu::Event event(cmd, L"Fluid / interior frame initialization");
    bind(cmd, view, frame, solids);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
    pass(cmd, 0, (constants.coarse.w + 63) / 64);
    // Restore the current owner totals after the per-frame statistics clear.
    pass(cmd, 6, (constants.coarse.w + 63) / 64);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
}
void FluidInterior::snapshot(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, uint32_t slot) {
    if (!validatePending)
        return;
    uint64_t offset = slot * snapshotStride;
    for (auto *r : {view.particles, cells.resource.Get()}) {
        gpu::transition(cmd, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(validation.resource.Get(), offset, r, 0, r->GetDesc().Width);
        offset += r->GetDesc().Width;
        gpu::transition(cmd, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}
void FluidInterior::restore(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *frame,
                            ID3D12Resource *solids, bool finalExchange) {
    gpu::Event event(cmd, L"Fluid / restore disturbed coarse interior to particles");
    snapshot(cmd, view, finalExchange ? 2 : 0);
    bind(cmd, view, frame, solids);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
    if (!finalExchange)
        pass(cmd, 2, orderedAllocation ? 1 : (capacity + 255) / 256);
    pass(cmd, 1, 1);
    pass(cmd, 3, orderedAllocation ? 1 : (constants.coarse.w + 63) / 64);
    pass(cmd, 6, (constants.coarse.w + 63) / 64);
    pass(cmd, 7, 1);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
    if (!finalExchange)
        snapshot(cmd, view, 1);
}
void FluidInterior::advect(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *frame,
                           ID3D12Resource *solids) {
    gpu::Event event(cmd, L"Fluid / coarse interior global-pressure velocity update");
    bind(cmd, view, frame, solids);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cmd->SetPipelineState(pipelines[5].Get());
    cmd->ExecuteIndirect(dispatch.Get(), 1, arguments.resource.Get(), 72, nullptr, 0);
    gpu::uav(cmd);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
}
void FluidInterior::deposit(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view, ID3D12Resource *frame,
                            ID3D12Resource *solids) {
    gpu::Event event(cmd, L"Fluid / conservative particle-to-coarse ownership transfer");
    bind(cmd, view, frame, solids);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
    pass(cmd, 1, 1);
    pass(cmd, 4, (constants.coarse.w + 63) / 64);
    pass(cmd, 6, (constants.coarse.w + 63) / 64);
    pass(cmd, 7, 1);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryCount++);
    snapshot(cmd, view, 3);
}
void FluidInterior::recordReadback(ID3D12GraphicsCommandList *cmd) {
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 0, counters.resource.Get(), 0, 64);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, queryCount, readback.resource.Get(),
                          64);
}
void FluidInterior::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 64 + queryCount * 8}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Interior metrics map");
    memcpy(counts.data(), data, 64);
    const auto *t = reinterpret_cast<const uint64_t *>(static_cast<const char *>(data) + 64);
    gpuMs = 0;
    for (uint32_t i = 0; i < queryCount; i += 2)
        gpuMs += double(t[i + 1] - t[i]) * 1000 / frequency;
    readback.resource->Unmap(0, &written);
    demotions += counts[2];
    promotions += counts[3];
    if (counts[8])
        throw std::runtime_error("Invalid coarse interior or insufficient conservative restoration capacity");
    if (!validatePending)
        return;
    range = {0, size_t(validation.resource->GetDesc().Width)};
    gpu::check(validation.resource->Map(0, &range, &data), "Interior invariant snapshots");
    struct Totals {
        double mass = 0, energy = 0;
        std::array<double, 3> linear{}, angular{};
    };
    auto total = [&](uint32_t slot) {
        Totals out;
        const auto *start = static_cast<const char *>(data) + slot * snapshotStride;
        for (uint32_t kind = 0; kind < 2; ++kind) {
            const auto *p = reinterpret_cast<const FluidParticle *>(
                start + (kind ? std::max(uint64_t(256), uint64_t(capacity) * 80) : 0));
            uint32_t n = kind ? constants.coarse.w : capacity;
            for (uint32_t i = 0; i < n; ++i) {
                if (!p[i].velocityFlags.w)
                    continue;
                double m = p[i].apic0.w;
                std::array<double, 3> moment{};
                for (int a = 0; a < 3; ++a) {
                    moment[a] = .25 * cellSize * cellSize;
                    if (kind) {
                        const uint32_t extent = (uint32_t(p[i].velocityFlags.w) >> (5 * a)) & 31;
                        const float spacing[] = {p[i].positionRadius.w, p[i].apic1.w, p[i].apic2.w};
                        moment[a] += double(spacing[a]) * spacing[a] * (extent * extent - 1) / 12;
                    }
                }
                out.mass += m;
                const float *x = &p[i].positionRadius.x, *v = &p[i].velocityFlags.x;
                const float *C[]{&p[i].apic0.x, &p[i].apic1.x, &p[i].apic2.x};
                for (int a = 0; a < 3; ++a) {
                    int b = (a + 1) % 3, c = (a + 2) % 3;
                    out.linear[a] += m * v[a];
                    out.angular[a] += m * (double(x[b]) * v[c] - double(x[c]) * v[b] + moment[b] * C[c][b] -
                                           moment[c] * C[b][c]);
                    out.energy +=
                        .5 * m *
                        (double(v[a]) * v[a] + moment[0] * double(C[a][0]) * C[a][0] +
                         moment[1] * double(C[a][1]) * C[a][1] + moment[2] * double(C[a][2]) * C[a][2]);
                }
            }
        }
        return out;
    };
    for (uint32_t pair = 0; pair < 4; pair += 2) {
        auto a = total(pair), b = total(pair + 1);
        if (!std::isfinite(a.mass + b.mass + a.energy + b.energy)) {
            validation.resource->Unmap(0, &written);
            throw std::runtime_error("Nonfinite coarse ownership snapshot");
        }
        massError = std::max(massError, std::abs(a.mass - b.mass));
        for (int k = 0; k < 3; ++k) {
            linearError = std::max(linearError, std::abs(a.linear[k] - b.linear[k]) / std::max(1.0, a.mass));
            angularError =
                std::max(angularError, std::abs(a.angular[k] - b.angular[k]) / std::max(1.0, a.mass));
        }
        energyIncrease = std::max(energyIncrease, (b.energy - a.energy) / std::max(1.0, a.energy));
    }
    validation.resource->Unmap(0, &written);
    validatePending = false;
    if (massError || linearError > 2e-6 || angularError > 2e-6 || energyIncrease > 5e-6)
        throw std::runtime_error("Coarse ownership violated mass/momentum/energy invariants");
    validated = true;
}
void FluidInterior::report(std::ostream &out) const {
    out << "{\"mode\":\"dormant coarse ownership with global fine pressure\",\"validated\":"
        << (validated ? "true" : "false") << ",\"ownedCells\":" << counts[0]
        << ",\"massUnits\":" << massUnits() << ",\"demotions\":" << demotions
        << ",\"promotions\":" << promotions << ",\"samplesRemovedThisFrame\":" << counts[9]
        << ",\"samplesRestoredThisFrame\":" << counts[10] << ",\"invalid\":" << counts[8]
        << ",\"latticeCandidates\":" << counts[7] << ",\"shapeRejected\":" << counts[11]
        << ",\"weightedRejected\":" << counts[13] << ",\"centroidRejected\":" << counts[14]
        << ",\"latticeRejected\":" << counts[15] << ",\"lastMs\":" << gpuMs << ",\"massError\":" << massError
        << ",\"linearErrorPerMass\":" << linearError << ",\"angularErrorPerMass\":" << angularError
        << ",\"relativeEnergyIncrease\":" << energyIncrease << '}';
}
} // namespace lab
