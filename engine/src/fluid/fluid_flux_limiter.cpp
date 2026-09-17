#include "fluid_flux_limiter.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace {
void copyLimiter(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, uint64_t offset, ID3D12Resource *src,
                 uint64_t size) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, offset, src, 0, size);
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
} // namespace
FluidFluxLimiter::FluidFluxLimiter(ID3D12Device *device, const std::filesystem::path &folder, XMUINT4 n)
    : grid(n) {
    faceCount = 3 * (n.x + 1) * (n.y + 1) * (n.z + 1);
    auto make = [&](uint64_t size, const wchar_t *name) {
        bytes += std::max(uint64_t(256), size);
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    scales = make(uint64_t(n.w) * 4, L"Fluid phase / receiver flux limits");
    control = make(64, L"Fluid phase / convergence and every-substep bounds");
    arguments = make(40, L"Fluid phase / indirect convergence work and predicate");
    readback = gpu::buffer(device, 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid phase / metrics and timestamps");
    D3D12_ROOT_PARAMETER p[7]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 4;
    for (uint32_t i = 1; i < 7; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC r{7, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Phase root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Phase root");
    const char *names[]{"FluxLimitClear", "FluxLimitBegin", "FluxLimitEvaluate", "FluxLimitPrepare",
                        "FluxLimitFaces", "FluxLimitAudit", "FluxLimitFinish"};
    for (uint32_t i = 0; i < pipelines.size(); ++i) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &arg};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&indirect)),
               "Phase indirect");
    D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 32, 0};
    gpu::check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries)), "Phase timestamps");
}
void FluidFluxLimiter::beginFrame(ID3D12GraphicsCommandList *cmd, bool validate, bool reset) {
    calls = 0;
    if (reset)
        steps = iterations = 0;
    validateFrame = validate;
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootUnorderedAccessView(5, control.resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(pipelines[0].Get());
    cmd->Dispatch(1, 1, 1);
    gpu::uav(cmd);
    if (validate && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(control.resource->GetDevice(IID_PPV_ARGS(&device)), "Phase snapshot device");
        snapshot =
            gpu::buffer(device.Get(), uint64_t(grid.w) * 24 + uint64_t(faceCount) * 32 + 64,
                        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                        L"Fluid phase / independent final-substep audit");
    }
}
void FluidFluxLimiter::record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resident,
                              ID3D12Resource *capacity, ID3D12Resource *transfers) {
    if (calls >= 16)
        throw std::runtime_error("Phase limiter timestamp capacity exceeded");
    gpu::Event event(cmd, L"Fluid / conservative phase-flux receiver limits");
    const uint64_t n = grid.w, f = faceCount;
    if (validateFrame) {
        copyLimiter(cmd, snapshot.resource.Get(), 0, resident, n * 16);
        copyLimiter(cmd, snapshot.resource.Get(), n * 16, capacity, n * 8);
        copyLimiter(cmd, snapshot.resource.Get(), n * 24, transfers, f * 16);
    }
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRoot32BitConstants(0, 4, &grid, 0);
    ID3D12Resource *buffers[]{resident,
                              capacity,
                              transfers,
                              scales.resource.Get(),
                              control.resource.Get(),
                              arguments.resource.Get()};
    for (uint32_t i = 0; i < 6; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
    auto pass = [&](uint32_t i, uint32_t groups) {
        cmd->SetPipelineState(pipelines[i].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * calls);
    pass(1, 1);
    pass(2, (grid.w + 127) / 128);
    pass(3, 1);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    for (uint32_t i = 0; i < maxIterations; ++i) {
        // Match the existing MGPCG schedule: SetPredication snapshots this
        // value, so the iteration may subsequently write new arguments. Once
        // converged, even the prepare dispatch is skipped, without CPU reads.
        cmd->SetPredication(arguments.resource.Get(), 32, D3D12_PREDICATION_OP_EQUAL_ZERO);
        cmd->SetPipelineState(pipelines[4].Get());
        cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
        gpu::uav(cmd);
        cmd->SetPipelineState(pipelines[2].Get());
        cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 12, nullptr, 0);
        gpu::uav(cmd);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pass(3, 1);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    pass(5, (grid.w + 127) / 128);
    pass(6, 1);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * calls + 1);
    ++calls;
    if (validateFrame) {
        copyLimiter(cmd, snapshot.resource.Get(), n * 24 + f * 16, transfers, f * 16);
        copyLimiter(cmd, snapshot.resource.Get(), n * 24 + f * 32, control.resource.Get(), 64);
    }
}
void FluidFluxLimiter::finishFrame(ID3D12GraphicsCommandList *cmd) {
    copyLimiter(cmd, readback.resource.Get(), 0, control.resource.Get(), 64);
    if (calls)
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, calls * 2,
                              readback.resource.Get(), 64);
}
void FluidFluxLimiter::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 512}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Phase metrics map");
    memcpy(stats.data(), data, 64);
    auto ticks = reinterpret_cast<const uint64_t *>(static_cast<const uint8_t *>(data) + 64);
    lastMs = 0;
    for (uint32_t i = 0; i < calls; ++i)
        lastMs += double(ticks[2 * i + 1] - ticks[2 * i]) * 1000 / frequency;
    readback.resource->Unmap(0, &written);
    totalMs += lastMs;
    ++frames;
    steps += stats[7];
    iterations += stats[2];
    float residual, old;
    memcpy(&residual, &stats[4], 4);
    memcpy(&old, &stats[5], 4);
    peakNewExcess = std::max(peakNewExcess, double(residual));
    peakOldExcess = std::max(peakOldExcess, double(old));
    if (stats[3] || stats[6] || stats[7] != calls)
        throw std::runtime_error("Phase flux limiting failed bounds/convergence audit");
    if (validateFrame && calls)
        validateSnapshot();
}
void FluidFluxLimiter::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Phase snapshot map");
    auto q = static_cast<const XMFLOAT4 *>(data);
    auto cap = reinterpret_cast<const XMFLOAT2 *>(q + grid.w);
    auto initial = reinterpret_cast<const XMFLOAT4 *>(cap + grid.w), result = initial + faceCount;
    auto counters = reinterpret_cast<const uint32_t *>(result + faceCount);
    using Value = std::array<double, 4>;
    std::vector<Value> flux(faceCount);
    std::vector<double> scale(grid.w);
    for (uint32_t i = 0; i < faceCount; ++i)
        for (int a = 0; a < 4; ++a)
            flux[i][a] = (&initial[i].x)[a];
    const uint32_t s = faceCount / 3;
    auto cell = [&](uint32_t x, uint32_t y, uint32_t z) { return (z * grid.y + y) * grid.x + x; };
    auto face = [&](uint32_t x, uint32_t y, uint32_t z, uint32_t a) {
        return a * s + (z * (grid.y + 1) + y) * (grid.x + 1) + x;
    };
    auto flow = [&](uint32_t id) {
        uint32_t p[]{id % grid.x, (id / grid.x) % grid.y, id / (grid.x * grid.y)};
        std::array<double, 2> sums{};
        for (uint32_t a = 0; a < 3; ++a) {
            uint32_t r[]{p[0], p[1], p[2]};
            ++r[a];
            double l = flux[face(p[0], p[1], p[2], a)][3], h = flux[face(r[0], r[1], r[2], a)][3];
            sums[0] += std::max(0., l) + std::max(0., -h);
            sums[1] += std::max(0., -l) + std::max(0., h);
        }
        return sums;
    };
    bool valid = counters[1] <= maxIterations;
    // Replay the recorded count in FP64. Termination near an FP32 threshold is
    // checked by bounds, not by demanding bit-identical CPU/GPU branch counts.
    for (uint32_t iteration = 0; iteration < counters[1]; ++iteration) {
        for (uint32_t i = 0; i < grid.w; ++i) {
            auto f = flow(i);
            double room = std::max(double(cap[i].x), double(q[i].w)) - q[i].w + f[1];
            scale[i] = f[0] > room + std::max(1e-12, double(cap[i].x) * 2e-7)
                           ? std::max(0., room) / f[0] * (1 - 2e-7)
                           : 1;
        }
        for (uint32_t i = 0; i < faceCount; ++i) {
            uint32_t a = i / s, k = i % s,
                     p[]{k % (grid.x + 1), (k / (grid.x + 1)) % (grid.y + 1),
                         k / ((grid.x + 1) * (grid.y + 1))};
            if (p[0] >= grid.x + (a == 0) || p[1] >= grid.y + (a == 1) || p[2] >= grid.z + (a == 2) ||
                p[a] == 0 || p[a] == (&grid.x)[a])
                continue;
            if (flux[i][3] < 0)
                --p[a];
            double r = scale[cell(p[0], p[1], p[2])];
            for (auto &v : flux[i])
                v *= r;
        }
    }
    auditError = 0;
    for (uint32_t i = 0; i < faceCount; ++i)
        for (int a = 0; a < 4; ++a) {
            double actual = (&result[i].x)[a], error = std::abs(flux[i][a] - actual);
            auditError = std::max(auditError, error);
            valid &= std::isfinite(actual) && error < std::max(2e-9, std::abs(flux[i][a]) * 2e-4);
            flux[i][a] = actual;
        }
    Value beforeSum{}, afterSum{};
    double beforeEnergy = 0, afterEnergy = 0;
    auto energy = [](const Value &v) {
        return v[3] > 0 ? (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) / (2 * v[3]) : 0;
    };
    for (uint32_t i = 0; i < grid.w; ++i) {
        auto f = flow(i);
        double next = q[i].w + f[0] - f[1];
        valid &= next >= 0 && next <= std::max(double(q[i].w), double(cap[i].x)) +
                                          std::max(1e-11, double(cap[i].x) * 4e-7);
        Value before{}, after{};
        for (int a = 0; a < 4; ++a)
            before[a] = after[a] = (&q[i].x)[a];
        uint32_t p[]{i % grid.x, (i / grid.x) % grid.y, i / (grid.x * grid.y)};
        for (uint32_t a = 0; a < 3; ++a) {
            uint32_t r[]{p[0], p[1], p[2]};
            ++r[a];
            for (int c = 0; c < 4; ++c)
                after[c] += flux[face(p[0], p[1], p[2], a)][c] - flux[face(r[0], r[1], r[2], a)][c];
        }
        for (int a = 0; a < 4; ++a) {
            beforeSum[a] += before[a];
            afterSum[a] += after[a];
        }
        beforeEnergy += energy(before);
        afterEnergy += energy(after);
    }
    conservationError = 0;
    for (int a = 0; a < 4; ++a)
        conservationError = std::max(conservationError, std::abs(beforeSum[a] - afterSum[a]));
    energyIncrease = std::max(0., afterEnergy - beforeEnergy);
    valid &= conservationError < std::max(1e-10, beforeSum[3] * 1e-7) &&
             energyIncrease < std::max(1e-10, beforeEnergy * 1e-6);
    snapshot.resource->Unmap(0, &written);
    if (!valid)
        throw std::runtime_error("Phase flux independent replay failed");
    validated = true;
}
void FluidFluxLimiter::report(std::ostream &out) const {
    out << "{\"iterations\":" << iterations << ",\"steps\":" << steps << ",\"lastIterations\":" << stats[1]
        << ",\"peakFrameIterations\":" << stats[9] << ",\"limitedFaceOperations\":" << stats[8]
        << ",\"invalid\":" << stats[3] << ",\"exhaustedSteps\":" << stats[6]
        << ",\"peakNewExcessM3\":" << peakNewExcess << ",\"peakPreexistingExcessM3\":" << peakOldExcess
        << ",\"snapshotMaxError\":" << auditError << ",\"lastFrameMs\":" << lastMs
        << ",\"snapshotConservationError\":" << conservationError
        << ",\"snapshotEnergyIncrease\":" << energyIncrease
        << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"allocatedGpuBufferBytes\":" << bytes << ",\"validated\":" << (validated ? "true" : "false")
        << "}";
}
} // namespace lab
