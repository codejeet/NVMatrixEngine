#include "fluid_volume_allocator.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

#include "fluid_numeric.h"

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidVolumeAllocator::FluidVolumeAllocator(ID3D12Device *device, const std::filesystem::path &folder,
                                           XMUINT4 n, float particleVolume, bool usePrecise)
    : grid(n), routingTolerance(particleVolume * 1e-7f) {
    precise = usePrecise;
    stateBytes = precise ? 32 : 16;
    capacityBytes = precise ? 16 : 8;
    faceStride = (n.x + 1) * (n.y + 1) * (n.z + 1);
    groups = (n.w + 127) / 128;
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        gpuBytes += std::max(uint64_t(256), bytes);
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    pendingState = make(uint64_t(n.w) * stateBytes, L"Fluid source / pending volume and momentum");
    weights = make(uint64_t(n.w) * 16, L"Fluid source / aperture weights and admission ledger");
    transfers =
        make(uint64_t(faceStride) * 3 * stateBytes, L"Fluid source / shared conservative routing transfers");
    partials = make(uint64_t(groups) * 64, L"Fluid source / audit reduction partials");
    totals = make(64, L"Fluid source / admission metrics");
    control = make(16, L"Fluid source / GPU routing state");
    arguments = make(24, L"Fluid source / indirect face and cell dispatch");
    readback = gpu::buffer(device, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid source / asynchronous metrics");
    D3D12_ROOT_PARAMETER p[11]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 7;
    for (uint32_t i = 1; i < 11; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC r{11, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Source root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Source root");
    const char *names[]{"AllocationClear",  "AllocationAccept", "AllocationPrepare", "AllocationFlux",
                        "AllocationUpdate", "AllocationReduce", "AllocationTotals"};
    for (uint32_t i = 0; i < pipelines.size(); ++i) {
        auto code =
            gpu::bytes(folder / "shaders" / (std::string(names[i]) + (precise ? "-precise.dxil" : ".dxil")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &argument, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&indirect)),
               "Source routing dispatch");
}
namespace {
void copyAllocation(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, uint64_t offset, ID3D12Resource *src,
                    uint64_t bytes) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, offset, src, 0, bytes);
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace
void FluidVolumeAllocator::record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resident,
                                  ID3D12Resource *capacity, ID3D12Resource *aperture, bool validate,
                                  bool advance) {
    gpu::Event event(cmd, L"Fluid / capacity-bounded source admission (not advection repair)");
    validateFrame = validate;
    advanceFrame = advance;
    const uint64_t n = grid.w;
    if (validate && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(resident->GetDevice(IID_PPV_ARGS(&device)), "Source audit device");
        snapshot =
            gpu::buffer(device.Get(), n * (4 * stateBytes + capacityBytes) + uint64_t(faceStride) * 12,
                        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                        L"Fluid source / independent admission snapshot");
    }
    if (validate) {
        copyAllocation(cmd, snapshot.resource.Get(), 0, resident, n * stateBytes);
        copyAllocation(cmd, snapshot.resource.Get(), n * stateBytes, pending(), n * stateBytes);
        copyAllocation(cmd, snapshot.resource.Get(), n * (4 * stateBytes), capacity, n * capacityBytes);
        copyAllocation(cmd, snapshot.resource.Get(), n * (4 * stateBytes + capacityBytes), aperture,
                       uint64_t(faceStride) * 12);
    }
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRoot32BitConstants(0, 4, &grid, 0);
    cmd->SetComputeRoot32BitConstant(0, 0, 4);
    cmd->SetComputeRoot32BitConstant(0, advance ? 1u : 0u, 5);
    cmd->SetComputeRoot32BitConstants(0, 1, &routingTolerance, 6);
    ID3D12Resource *buffers[]{resident,
                              pending(),
                              capacity,
                              aperture,
                              weights.resource.Get(),
                              transfers.resource.Get(),
                              partials.resource.Get(),
                              totals.resource.Get(),
                              control.resource.Get(),
                              arguments.resource.Get()};
    for (uint32_t i = 0; i < 10; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
    auto pass = [&](uint32_t p, uint32_t count) {
        cmd->SetPipelineState(pipelines[p].Get());
        cmd->Dispatch(count, 1, 1);
        gpu::uav(cmd);
    };
    pass(0, 1);
    pass(1, groups);
    pass(2, 1);
    for (uint32_t i = 0; advance && i < maxIterations; ++i) {
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        cmd->SetPipelineState(pipelines[3].Get());
        cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
        gpu::uav(cmd);
        cmd->SetPipelineState(pipelines[4].Get());
        cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 12, nullptr, 0);
        gpu::uav(cmd);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRoot32BitConstant(0, i + 1, 4);
        pass(2, 1);
    }
    pass(5, groups);
    pass(6, 1);
    copyAllocation(cmd, readback.resource.Get(), 0, totals.resource.Get(), sizeof(Metrics));
    if (validate) {
        copyAllocation(cmd, snapshot.resource.Get(), n * (2 * stateBytes), resident, n * stateBytes);
        copyAllocation(cmd, snapshot.resource.Get(), n * (3 * stateBytes), pending(), n * stateBytes);
    }
}
void FluidVolumeAllocator::collect(double gpuMs) {
    void *data;
    D3D12_RANGE range{0, sizeof(Metrics)}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Source admission metrics");
    memcpy(&metrics, data, sizeof(metrics));
    readback.resource->Unmap(0, &written);
    lastMs = gpuMs;
    totalMs += gpuMs;
    if (metrics.counts.z || metrics.bounds.x > 1e-7 || !std::isfinite(metrics.pending.w) ||
        metrics.pending.w < 0)
        throw std::runtime_error("Source admission violated positivity or increased resident overfill");
    if (validateFrame)
        validateSnapshot();
}
void FluidVolumeAllocator::beginFrame() {
    ++frames;
    lastMs = 0;
    advanceFrame = false;
    metrics.activity = {};
    metrics.bounds.x = 0;
    metrics.counts.w = 0;
}
void FluidVolumeAllocator::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Source admission snapshot");
    const uint32_t n = grid.w;
    const auto raw = static_cast<const char *>(data);
    const auto residentBefore = readFluidNumbers<FluidDouble4>(raw, n, precise);
    const auto pendingBefore = readFluidNumbers<FluidDouble4>(raw + uint64_t(n) * stateBytes, n, precise);
    const auto residentAfter = readFluidNumbers<FluidDouble4>(raw + uint64_t(n) * 2 * stateBytes, n, precise);
    const auto pendingAfter = readFluidNumbers<FluidDouble4>(raw + uint64_t(n) * 3 * stateBytes, n, precise);
    const auto capacity = readFluidNumbers<FluidDouble2>(raw + uint64_t(n) * 4 * stateBytes, n, precise);
    const auto aperture =
        reinterpret_cast<const float *>(raw + uint64_t(n) * (4 * stateBytes + capacityBytes));
    using Value = std::array<double, 4>;
    std::vector<Value> r(n), p(n), next(n);
    std::vector<double> weight(n);
    auto xyz = [&](uint32_t i) {
        return std::array<uint32_t, 3>{i % grid.x, (i / grid.x) % grid.y, i / (grid.x * grid.y)};
    };
    auto index = [&](std::array<uint32_t, 3> c) { return (c[2] * grid.y + c[1]) * grid.x + c[0]; };
    auto face = [&](std::array<uint32_t, 3> c, uint32_t a) {
        return a * faceStride + (c[2] * (grid.y + 1) + c[1]) * (grid.x + 1) + c[0];
    };
    bool valid = true;
    Value beforeSum{}, afterSum{}, pendingSum{};
    double energyBefore = 0, energyAfter = 0;
    auto energy = [](const FluidDouble4 &q) {
        return q.w > 0 ? .5 * (double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z) / q.w : 0;
    };
    for (uint32_t i = 0; i < n; ++i) {
        for (int a = 0; a < 4; ++a) {
            r[i][a] = (&residentBefore[i].x)[a];
            p[i][a] = (&pendingBefore[i].x)[a];
            beforeSum[a] += r[i][a] + p[i][a];
            afterSum[a] += double((&residentAfter[i].x)[a]) + (&pendingAfter[i].x)[a];
            pendingSum[a] += (&pendingAfter[i].x)[a];
            valid &= std::isfinite(r[i][a]) && std::isfinite(p[i][a]) &&
                     std::isfinite((&residentAfter[i].x)[a]) && std::isfinite((&pendingAfter[i].x)[a]);
        }
        valid &= residentAfter[i].w >= 0 && pendingAfter[i].w >= 0 && std::isfinite(capacity[i].x) &&
                 capacity[i].x >= 0;
        valid &= residentAfter[i].w <= std::max(residentBefore[i].w, capacity[i].x) + 1e-7;
        energyBefore += energy(residentBefore[i]) + energy(pendingBefore[i]);
        energyAfter += energy(residentAfter[i]) + energy(pendingAfter[i]);
        const auto c = xyz(i);
        for (uint32_t a = 0; a < 3; ++a)
            for (int d : {-1, 1}) {
                auto neighbor = c, f = c;
                if ((d < 0 && c[a] == 0) || (d > 0 && c[a] + 1 >= (&grid.x)[a]))
                    continue;
                neighbor[a] += d;
                if (d > 0)
                    ++f[a];
                const float area = aperture[face(f, a)];
                valid &= std::isfinite(area) && area >= 0;
                if (capacity[index(neighbor)].x > 0)
                    weight[i] += area;
            }
    }
    auto accept = [&] {
        for (uint32_t i = 0; i < n; ++i) {
            const double amount = std::min(p[i][3], std::max(0., double(capacity[i].x) - r[i][3]));
            const double fraction = p[i][3] > 0 ? amount / p[i][3] : 0;
            for (int a = 0; a < 4; ++a) {
                double q = a == 3 ? amount : p[i][a] * fraction;
                r[i][a] += q;
                p[i][a] -= q;
            }
        }
    };
    if (advanceFrame)
        accept();
    for (uint32_t iteration = 0; advanceFrame && iteration < maxIterations; ++iteration) {
        bool active = false;
        for (uint32_t i = 0; i < n; ++i)
            active |= p[i][3] > routingTolerance && weight[i] > 0;
        if (!active)
            break;
        next = p;
        for (uint32_t i = 0; i < n; ++i) {
            if (weight[i] == 0)
                continue;
            const auto c = xyz(i);
            for (uint32_t a = 0; a < 3; ++a)
                for (int d : {-1, 1}) {
                    auto neighbor = c, f = c;
                    if ((d < 0 && c[a] == 0) || (d > 0 && c[a] + 1 >= (&grid.x)[a]))
                        continue;
                    neighbor[a] += d;
                    if (d > 0)
                        ++f[a];
                    uint32_t j = index(neighbor);
                    if (capacity[j].x <= 0)
                        continue;
                    double factor = .5 * aperture[face(f, a)] / weight[i];
                    for (int k = 0; k < 4; ++k) {
                        double v = p[i][k] * factor;
                        next[i][k] -= v;
                        next[j][k] += v;
                    }
                }
        }
        p.swap(next);
        accept();
    }
    auditError = auditMassError = 0;
    for (uint32_t i = 0; i < n; ++i)
        for (int a = 0; a < 4; ++a) {
            double error = std::max(std::abs(r[i][a] - (&residentAfter[i].x)[a]),
                                    std::abs(p[i][a] - (&pendingAfter[i].x)[a]));
            auditError = std::max(auditError, error);
            valid &= error < std::max(2e-8, std::max(std::abs(r[i][a]), std::abs(p[i][a])) * 3e-4);
        }
    for (int a = 0; a < 4; ++a) {
        const double scale = std::max(1e-9, std::abs(beforeSum[a]) + beforeSum[3]);
        auditMassError = std::max(auditMassError, std::abs(beforeSum[a] - afterSum[a]) / scale);
        valid &= std::abs(pendingSum[a] - (&metrics.pending.x)[a]) < std::max(1e-9, scale * 2e-5);
    }
    auditEnergyIncrease = std::max(0., energyAfter - energyBefore);
    valid &= auditMassError < 2e-5 && auditEnergyIncrease < std::max(1e-9, energyBefore * 2e-5);
    snapshot.resource->Unmap(0, &written);
    if (!valid)
        throw std::runtime_error("Source admission independent capacity/flux/momentum/energy audit failed");
    validated = true;
}
void FluidVolumeAllocator::report(std::ostream &out) const {
    out << "{\"pendingVolumeM3\":" << metrics.pending.w << ",\"admittedLastFrameM3\":" << metrics.activity.x
        << ",\"routedLastFrameM3\":" << metrics.activity.y << ",\"pendingCells\":" << metrics.counts.x
        << ",\"unroutableCells\":" << metrics.counts.y << ",\"iterations\":" << metrics.counts.w
        << ",\"invalid\":" << metrics.counts.z << ",\"newExcessM3\":" << metrics.bounds.x
        << ",\"lastFrameMs\":" << lastMs << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"allocatedGpuBufferBytes\":" << gpuBytes << ",\"snapshotMaxError\":" << auditError
        << ",\"relativeConservationError\":" << auditMassError
        << ",\"energyIncrease\":" << auditEnergyIncrease << ",\"routingToleranceM3\":" << routingTolerance
        << ",\"advanced\":" << (advanceFrame ? "true" : "false")
        << ",\"validated\":" << (validated ? "true" : "false") << "}";
}
} // namespace lab
