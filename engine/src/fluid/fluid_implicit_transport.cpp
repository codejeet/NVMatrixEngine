#include "fluid_implicit_transport.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

#include "fluid_numeric.h"

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace {
void copyImplicit(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, uint64_t offset, ID3D12Resource *src,
                  uint64_t bytes) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, offset, src, 0, bytes);
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
float counterFloat(uint32_t bits) {
    float value;
    memcpy(&value, &bits, 4);
    return value;
}
} // namespace
FluidImplicitTransport::FluidImplicitTransport(ID3D12Device *device, const std::filesystem::path &folder,
                                               XMUINT4 grid, bool usePrecise) {
    precise = usePrecise;
    stateBytes = precise ? 32 : 16;
    capacityBytes = precise ? 16 : 8;
    constants.grid = grid;
    constants.parameters = {0, precise ? 2e-13f : 2e-10f, 1e-20f, 0};
    faceCount = 3 * (grid.x + 1) * (grid.y + 1) * (grid.z + 1);
    auto make = [&](uint64_t size, const wchar_t *name) {
        bytes += std::max(uint64_t(256), size);
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    solution[0] = make(uint64_t(grid.w) * 32, L"Fluid implicit / FP64 concentration ping");
    solution[1] = make(uint64_t(grid.w) * 32, L"Fluid implicit / FP64 concentration pong");
    diagonal = make(uint64_t(grid.w) * 8, L"Fluid implicit / endpoint storage plus outflow");
    control = make(64, L"Fluid implicit / every-substep residual and closure counters");
    arguments = make(40, L"Fluid implicit / GPU convergence predicate and dispatch");
    readback =
        gpu::buffer(device, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid implicit / existing-fence timing and metrics");
    D3D12_ROOT_PARAMETER p[12]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, sizeof(Constants) / 4};
    for (uint32_t i = 1; i < 12; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC r{12, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Implicit transport root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Implicit transport root");
    const char *names[]{"ImplicitClear",   "ImplicitInitialize", "ImplicitIterate", "ImplicitResidual",
                        "ImplicitPrepare", "ImplicitExport",     "ImplicitFinish"};
    for (uint32_t i = 0; i < pipelines.size(); ++i) {
        auto code =
            gpu::bytes(folder / "shaders" / (std::string(names[i]) + (precise ? "-precise.dxil" : ".dxil")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC s{12, 1, &a};
    gpu::check(device->CreateCommandSignature(&s, nullptr, IID_PPV_ARGS(&indirect)),
               "Implicit transport dispatch");
    D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 32, 0};
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Implicit transport timestamps");
}
void FluidImplicitTransport::beginFrame(ID3D12GraphicsCommandList *cmd, bool validate, bool reset) {
    calls = 0;
    validateFrame = validate;
    if (reset)
        steps = iterations = 0;
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootUnorderedAccessView(10, control.resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(pipelines[0].Get());
    cmd->Dispatch(1, 1, 1);
    gpu::uav(cmd);
}
void FluidImplicitTransport::record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resident,
                                    ID3D12Resource *capacity, ID3D12Resource *rates, ID3D12Resource *output,
                                    ID3D12Resource *transfers, ID3D12Resource *limiter, float dt,
                                    bool swept) {
    if (calls >= 16 || !(dt > 0) || !std::isfinite(dt))
        throw std::runtime_error("Invalid implicit transport substep");
    constants.parameters.x = dt;
    constants.parameters.w = swept ? 1.f : 0.f;
    const uint64_t n = constants.grid.w, f = faceCount;
    const uint64_t sizes[]{n * stateBytes, n * capacityBytes, f * 8, n * 32, n * stateBytes, f * stateBytes};
    if (validateFrame && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(resident->GetDevice(IID_PPV_ARGS(&device)), "Implicit snapshot device");
        uint64_t size = 0;
        for (uint32_t i = 0; i < offsets.size(); ++i) {
            offsets[i] = size;
            size += sizes[i];
        }
        snapshot = gpu::buffer(device.Get(), size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               L"Fluid implicit / independent final-substep inputs and solution");
    }
    if (validateFrame) {
        copyImplicit(cmd, snapshot.resource.Get(), offsets[0], resident, sizes[0]);
        copyImplicit(cmd, snapshot.resource.Get(), offsets[1], capacity, sizes[1]);
        copyImplicit(cmd, snapshot.resource.Get(), offsets[2], rates, sizes[2]);
    }
    gpu::Event event(cmd, L"Fluid / implicit conservative coarse transport");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
    ID3D12Resource *buffers[]{resident,
                              capacity,
                              rates,
                              solution[0].resource.Get(),
                              solution[1].resource.Get(),
                              diagonal.resource.Get(),
                              output,
                              transfers,
                              limiter,
                              control.resource.Get(),
                              arguments.resource.Get()};
    for (uint32_t i = 0; i < 11; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
    auto pass = [&](uint32_t p, uint32_t groups) {
        cmd->SetPipelineState(pipelines[p].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    auto bindSolution = [&](uint32_t source) {
        cmd->SetComputeRootUnorderedAccessView(4, solution[source].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(5, solution[1 - source].resource->GetGPUVirtualAddress());
    };
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, calls * 2);
    pass(1, (constants.grid.w + 127) / 128);
    pass(3, (constants.grid.w + 127) / 128);
    pass(4, 1);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    for (uint32_t iteration = 0; iteration < maxIterations; iteration += 2) {
        cmd->SetPredication(arguments.resource.Get(), 32, D3D12_PREDICATION_OP_EQUAL_ZERO);
        // Two Jacobi iterations keep the final solution in ping even when
        // subsequent iterations are skipped entirely by the GPU predicate.
        for (uint32_t source = 0; source < 2; ++source) {
            bindSolution(source);
            cmd->SetPipelineState(pipelines[2].Get());
            cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
            gpu::uav(cmd);
        }
        bindSolution(0);
        cmd->SetPipelineState(pipelines[3].Get());
        cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
        gpu::uav(cmd);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pass(4, 1);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bindSolution(0);
    pass(5, (std::max(constants.grid.w, faceCount) + 127) / 128);
    pass(6, 1);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, calls * 2 + 1);
    ++calls;
    if (validateFrame) {
        copyImplicit(cmd, snapshot.resource.Get(), offsets[3], solution[0].resource.Get(), sizes[3]);
        copyImplicit(cmd, snapshot.resource.Get(), offsets[4], output, sizes[4]);
        copyImplicit(cmd, snapshot.resource.Get(), offsets[5], transfers, sizes[5]);
    }
}
void FluidImplicitTransport::finishFrame(ID3D12GraphicsCommandList *cmd) {
    copyImplicit(cmd, readback.resource.Get(), 0, control.resource.Get(), sizeof(stats));
    if (calls)
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, calls * 2,
                              readback.resource.Get(), 128);
}
void FluidImplicitTransport::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 1024}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Implicit transport metrics");
    memcpy(stats.data(), data, sizeof(stats));
    auto ticks = reinterpret_cast<const uint64_t *>(static_cast<const char *>(data) + 128);
    gpuMs = 0;
    for (uint32_t i = 0; i < calls; ++i)
        gpuMs += double(ticks[i * 2 + 1] - ticks[i * 2]) * 1000 / frequency;
    readback.resource->Unmap(0, &written);
    ++frames;
    totalMs += gpuMs;
    steps += stats[4];
    iterations += stats[3];
    closingCells += stats[7];
    closingWaterCells += stats[8];
    peakResidual = std::max(peakResidual, double(counterFloat(stats[9])));
    peakExcess = std::max(peakExcess, double(counterFloat(stats[11])));
    if (stats[5] || stats[6] || stats[4] != calls || peakResidual > constants.parameters.y * 1.001) {
        if (validateFrame && calls)
            validateSnapshot(true);
        throw std::runtime_error("Implicit bulk transport failed convergence/positivity: residual " +
                                 std::to_string(counterFloat(stats[9])) + ", iterations " +
                                 std::to_string(stats[10]));
    }
    if (validateFrame && calls)
        validateSnapshot();
    else if (validateFrame) {
        if (std::any_of(stats.begin(), stats.end(), [](uint32_t v) { return v != 0; }))
            throw std::runtime_error("Idle implicit transport frame recorded work");
        ++auditedIdleFrames;
    }
}
void FluidImplicitTransport::validateSnapshot(bool failedSolve) {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Implicit transport snapshot");
    const auto raw = static_cast<const char *>(data);
    const auto before = readFluidNumbers<FluidDouble4>(raw + offsets[0], constants.grid.w, precise);
    const auto cap = readFluidNumbers<FluidDouble2>(raw + offsets[1], constants.grid.w, precise);
    const auto after = readFluidNumbers<FluidDouble4>(raw + offsets[4], constants.grid.w, precise);
    const auto transfers = readFluidNumbers<FluidDouble4>(raw + offsets[5], faceCount, precise);
    using Value = std::array<double, 4>;
    const auto g = constants.grid;
    std::vector<Value> source(g.w), lhs(g.w), balance(g.w), solutionCpu(g.w);
    std::vector<double> diagonalCpu(g.w);
    for (uint32_t i = 0; i < g.w; ++i) {
        memcpy(solutionCpu[i].data(), raw + offsets[3] + uint64_t(i) * 32, 32);
        diagonalCpu[i] = cap[i].x;
        for (uint32_t a = 0; a < 4; ++a) {
            source[i][a] = (&before[i].x)[a];
            balance[i][a] = source[i][a];
            lhs[i][a] = cap[i].x * solutionCpu[i][a];
        }
    }
    bool valid = true;
    const uint32_t stride = faceCount / 3;
    // Independent face assembly contributes one column to both neighboring
    // rows. It does not replay the shader's per-cell source-gather routine.
    for (uint32_t id = 0; id < faceCount; ++id) {
        double rate;
        memcpy(&rate, raw + offsets[2] + uint64_t(id) * 8, 8);
        const uint32_t axis = id / stride, k = id % stride;
        uint32_t p[]{k % (g.x + 1), k / (g.x + 1) % (g.y + 1), k / ((g.x + 1) * (g.y + 1))};
        const bool live = p[0] < g.x + (axis == 0) && p[1] < g.y + (axis == 1) && p[2] < g.z + (axis == 2);
        if (!live || p[axis] == 0 || p[axis] == (&g.x)[axis]) {
            valid &= rate == 0;
            for (uint32_t a = 0; a < 4; ++a)
                valid &= (&transfers[id].x)[a] == 0;
            continue;
        }
        const uint32_t right = (p[2] * g.y + p[1]) * g.x + p[0];
        const uint32_t strideAxis[]{1, g.x, g.x * g.y};
        const uint32_t left = right - strideAxis[axis], donor = rate >= 0 ? left : right;
        const double Q = double(constants.parameters.x) * rate;
        diagonalCpu[donor] += std::abs(Q);
        for (uint32_t a = 0; a < 4; ++a) {
            const double flux = Q * solutionCpu[donor][a], stored = (&transfers[id].x)[a];
            lhs[left][a] += flux;
            lhs[right][a] -= flux;
            balance[left][a] -= stored;
            balance[right][a] += stored;
            valid &=
                std::isfinite(stored) && std::abs(stored - flux) <= std::max(1e-12, std::abs(flux) * 2e-7);
        }
    }
    Value sumBefore{}, sumAfter{}, magnitude{};
    double worstResidual = 0;
    uint32_t worstCell = 0, worstComponent = 0;
    double beforeEnergy = 0, afterEnergy = 0;
    auto energy = [](const FluidDouble4 &q) {
        return q.w > 0 ? (double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z) / (2 * q.w) : 0.;
    };
    for (uint32_t i = 0; i < g.w; ++i) {
        valid &= after[i].w >= 0 && (cap[i].x != 0 || after[i].w == 0);
        for (uint32_t a = 0; a < 4; ++a) {
            const double r =
                std::abs(lhs[i][a] - source[i][a]) /
                std::max(double(constants.parameters.z), std::abs(source[i][a]) + diagonalCpu[i]);
            residualError = std::max(residualError, r);
            if (r > worstResidual) {
                worstResidual = r;
                worstCell = i;
                worstComponent = a;
            }
            const double actual = (&after[i].x)[a], expected = cap[i].x * solutionCpu[i][a];
            valid &= std::isfinite(actual) && std::isfinite(expected) &&
                     std::abs(actual - expected) <= std::max(1e-12, std::abs(expected) * 2e-7);
            const double error = std::abs(balance[i][a] - actual);
            balanceError = std::max(balanceError, error);
            valid &= error <= std::max(1e-8, std::abs(actual) * 2e-5);
            sumBefore[a] += source[i][a];
            sumAfter[a] += actual;
            magnitude[a] += std::abs(source[i][a]);
        }
        beforeEnergy += energy(before[i]);
        afterEnergy += energy(after[i]);
    }
    for (uint32_t a = 0; a < 4; ++a) {
        const double error = std::abs(sumBefore[a] - sumAfter[a]);
        conservationError = std::max(conservationError, error);
        valid &= error <= std::max(1e-10, (magnitude[a] + sumBefore[3]) * 2e-7);
    }
    energyIncrease = std::max(energyIncrease, std::max(0., afterEnergy - beforeEnergy));
    valid &= afterEnergy <= beforeEnergy + std::max(1e-10, beforeEnergy * 1e-6);
    valid &= residualError <= constants.parameters.y * 1.01;
    std::ostringstream failure;
    if (failedSolve) {
        failure << "Implicit transport failed at frame " << frames << ", substeps " << steps << ", residual "
                << counterFloat(stats[9]) << ", iterations " << stats[10] << "; final-substep worst row ["
                << worstCell % g.x << ',' << worstCell / g.x % g.y << ',' << worstCell / (g.x * g.y)
                << "], component " << worstComponent << ", row residual " << worstResidual
                << ", old capacity " << cap[worstCell].y << ", new capacity " << cap[worstCell].x
                << ", diagonal " << diagonalCpu[worstCell] << ", source mass " << before[worstCell].w
                << ", concentration " << solutionCpu[worstCell][3];
    }
    snapshot.resource->Unmap(0, &written);
    if (failedSolve)
        throw std::runtime_error(failure.str());
    if (!valid)
        throw std::runtime_error("Implicit transport differs from independent matrix/balance/energy audit");
    validated = true;
    ++auditedFrames;
}
void FluidImplicitTransport::report(std::ostream &out) const {
    out << "{\"method\":\"backward-Euler upwind; not bounded VOF or particle ownership\",\"steps\":" << steps
        << ",\"lastFrameCalls\":" << calls << ",\"iterations\":" << iterations
        << ",\"maxIterations\":" << maxIterations << ",\"lastIterations\":" << stats[2]
        << ",\"peakFrameIterations\":" << stats[10] << ",\"invalid\":" << stats[5]
        << ",\"exhaustedSteps\":" << stats[6] << ",\"closingCellUpdates\":" << closingCells
        << ",\"closingWaterUpdates\":" << closingWaterCells << ",\"peakResidual\":" << peakResidual
        << ",\"tolerance\":" << constants.parameters.y << ",\"inventoryBits\":" << (precise ? 64 : 32)
        << ",\"peakExcessM3\":" << peakExcess << ",\"lastFrameMaxFraction\":" << counterFloat(stats[12])
        << ",\"matrixReferenceError\":" << residualError << ",\"balanceError\":" << balanceError
        << ",\"conservationError\":" << conservationError << ",\"energyIncrease\":" << energyIncrease
        << ",\"lastFrameMs\":" << gpuMs << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"allocatedGpuBufferBytes\":" << bytes << ",\"auditedFrames\":" << auditedFrames
        << ",\"auditedIdleFrames\":" << auditedIdleFrames
        << ",\"validated\":" << (validated ? "true" : "false") << "}";
}
} // namespace lab
