#include "fluid_mac_pressure.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <functional>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidMacPressure::FluidMacPressure(ID3D12Device *device, const std::filesystem::path &folder, XMUINT4 grid,
                                   float h, float density, float rate, bool cutPressure)
    : cutProjection(cutPressure) {
    outputFolder = folder;
    D3D12_FEATURE_DATA_D3D12_OPTIONS features{};
    gpu::check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &features, sizeof(features)),
               "MGPCG precision support");
    if (!features.DoublePrecisionFloatShaderOps)
        throw std::runtime_error("Mixed MAC MGPCG requires double-precision residual/projection support; use "
                                 "relaxation on this GPU");
    constants.grid = grid;
    XMUINT4 level{(grid.x + 3) / 4, (grid.y + 3) / 4, (grid.z + 3) / 4, 0};
    do {
        if (constants.control.x == 16)
            throw std::runtime_error("Mixed MAC pressure hierarchy is too deep");
        level.w = capacity;
        constants.levels[constants.control.x++] = level;
        capacity += level.x * level.y * level.z;
        if (level.x * level.y * level.z <= 64)
            break;
        level = {(level.x + 1) / 2, (level.y + 1) / 2, (level.z + 1) / 2, 0};
    } while (true);
    // Relative residual plus absolute physical divergence. The bottom shift is
    // only a preconditioner regularization; the projected operator is unshifted.
    constants.parameters = {1e-10f, .0001f, 1 / (density * h * h * rate), 1e-4f};
    auto make = [&](uint64_t size, const wchar_t *name) {
        bytes += std::max(uint64_t(256), size);
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    uniforms = gpu::buffer(device, 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Mixed MAC MGPCG / constants");
    memcpy(uniforms.mapped, &constants, sizeof(constants));
    for (auto &b : fine)
        b = make(uint64_t(grid.w) * 4, L"Mixed MAC MGPCG / fine correction");
    vectors = make(uint64_t(grid.w) * 16, L"Mixed MAC MGPCG / Krylov vectors");
    precisePressure = make(uint64_t(grid.w) * 8, L"Mixed MAC MGPCG / precise pressure accumulator");
    rows = make(uint64_t(capacity) * sizeof(CoarseRow), L"Mixed MAC MGPCG / Galerkin operators");
    rhs = make(uint64_t(capacity) * 4, L"Mixed MAC MGPCG / restricted residuals");
    for (auto &b : error)
        b = make(uint64_t(capacity) * 4, L"Mixed MAC MGPCG / coarse corrections");
    work = make(uint64_t(capacity) * 4, L"Mixed MAC MGPCG / compact level lists");
    counts = make(256, L"Mixed MAC MGPCG / active counts and convergence");
    args = make(256, L"Mixed MAC MGPCG / GPU convergence indirect dispatch");
    partials = make(uint64_t((grid.w + 127) / 128) * 16, L"Mixed MAC MGPCG / dot reductions");
    scalars = make((4 + maxIterations) * 16, L"Mixed MAC MGPCG / CG scalars and convergence trace");
    factor = make(4096 * 4, L"Mixed MAC MGPCG / bottom Cholesky factor");
    readback = gpu::buffer(device, 16 * 320, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Mixed MAC MGPCG / deferred solve metrics");
    D3D12_ROOT_PARAMETER p[24]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {1, 0, 4};
    for (uint32_t i = 0; i < 19; ++i) {
        p[i + 2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i + 2].Descriptor.ShaderRegister = i < 13 ? i : i + 1;
    }
    p[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[21].Descriptor.ShaderRegister = 13;
    p[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[22].Descriptor.ShaderRegister = 20;
    p[23].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[23].Descriptor.ShaderRegister = 21;
    D3D12_ROOT_SIGNATURE_DESC r{24, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, failure;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &failure),
               "MGPCG root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "MGPCG root");
    const char *names[]{"MgClear",         "MgAssemble",         "MgCanonicalFaces",    "MgPrepare",
                        "MgFactor",        "MgInitialize",       "MgResetCorrection",   "MgFineSmooth",
                        "MgRestrictBase",  "MgCoarseSmooth",     "MgRestrictCoarse",    "MgBottom",
                        "MgProlongCoarse", "MgProlongFine",      "MgDotPreconditioned", "MgDirection",
                        "MgApply",         "MgUpdate",           "MgResidual",          "MgReduce",
                        "MgCoarseCycle",   "MgImportCorrection", "MgExportCorrection"};
    for (uint32_t i = 0; i < PassCount; ++i) {
        auto code =
            gpu::bytes(folder / "shaders" / (std::string(names[i]) + (cutProjection ? "Cut.dxil" : ".dxil")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC s{12, 1, &a, 0};
    gpu::check(device->CreateCommandSignature(&s, nullptr, IID_PPV_ARGS(&dispatch)),
               "MGPCG indirect signature");
}
void FluidMacPressure::beginFrame(bool validate) {
    solves = 0;
    cachedBase = {};
    audit = validate;
}
void FluidMacPressure::solve(ID3D12GraphicsCommandList *cmd, ID3D12Resource *baseRows, ID3D12Resource *map,
                             ID3D12Resource *cells, ID3D12Resource *list, ID3D12Resource *baseCounts,
                             ID3D12Resource *solution, ID3D12Resource *cutExact) {
    if (solves >= 16)
        throw std::runtime_error("MGPCG substep capacity exceeded");
    cachedBase = {baseRows, map, cells, list, baseCounts, cutExact};
    gpu::Event event(cmd, L"Fluid / mixed MAC multigrid-preconditioned CG");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(21, precisePressure.resource->GetGPUVirtualAddress());
    if (cutProjection) {
        if (!cutExact)
            throw std::runtime_error("Cut-cell MGPCG requires the precise physical operator");
        cmd->SetComputeRootUnorderedAccessView(22, cutExact->GetGPUVirtualAddress());
    }
    ID3D12Resource *resources[]{baseRows,
                                map,
                                cells,
                                list,
                                baseCounts,
                                solution,
                                fine[0].resource.Get(),
                                fine[1].resource.Get(),
                                vectors.resource.Get(),
                                rows.resource.Get(),
                                rhs.resource.Get(),
                                error[0].resource.Get(),
                                error[1].resource.Get(),
                                work.resource.Get(),
                                counts.resource.Get(),
                                args.resource.Get(),
                                partials.resource.Get(),
                                scalars.resource.Get(),
                                factor.resource.Get()};
    for (uint32_t i = 0; i < 19; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 2, resources[i]->GetGPUVirtualAddress());
    auto stage = [&](uint32_t level, uint32_t iteration = 0, uint32_t mode = 0) {
        const XMUINT4 v{level, iteration, mode, 0};
        cmd->SetComputeRoot32BitConstants(1, 4, &v, 0);
    };
    auto direct = [&](Pass pass, uint32_t groups) {
        cmd->SetPipelineState(pipelines[pass].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    auto indirect = [&](Pass pass, uint32_t record) {
        cmd->SetPipelineState(pipelines[pass].Get());
        cmd->ExecuteIndirect(dispatch.Get(), 1, args.resource.Get(), record * 12, nullptr, 0);
        gpu::uav(cmd);
    };
    auto reduce = [&](uint32_t iteration, uint32_t mode) {
        gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        stage(0, iteration, mode);
        direct(Reduce, 1);
        gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        // SetPredication snapshots the 64-bit value here. Later UAV writes do
        // not alter that snapshot until the next SetPredication. This skips
        // dispatch command processing as well as shader work after convergence.
        cmd->SetPredication(args.resource.Get(), 248, D3D12_PREDICATION_OP_EQUAL_ZERO);
    };
    stage(0);
    direct(Clear, 1);
    for (uint32_t l = 0; l < constants.control.x; ++l) {
        stage(l);
        const auto g = constants.levels[l];
        const uint32_t groups = (g.x * g.y * g.z + 127) / 128;
        direct(Assemble, groups);
        direct(Canonical, groups);
    }
    direct(Prepare, 1);
    direct(Factor, 1);
    gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    const uint32_t dotRecord = constants.control.x + 2;
    indirect(Initialize, dotRecord);
    reduce(0, 0);
    auto smooth = [&](bool base, uint32_t level) {
        stage(level);
        for (uint32_t sweep = 0; sweep < 2; ++sweep) {
            cmd->SetComputeRootUnorderedAccessView(
                base ? 8 : 13, (base ? fine[sweep] : error[sweep]).resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(
                base ? 9 : 14, (base ? fine[1 - sweep] : error[1 - sweep]).resource->GetGPUVirtualAddress());
            indirect(base ? FineSmooth : CoarseSmooth, base ? 0 : level + 1);
        }
        cmd->SetComputeRootUnorderedAccessView(base ? 8 : 13,
                                               (base ? fine[0] : error[0]).resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(base ? 9 : 14,
                                               (base ? fine[1] : error[1]).resource->GetGPUVirtualAddress());
    };
    std::function<void(uint32_t)> cycle = [&](uint32_t l) {
        stage(l);
        if (l + 1 == constants.control.x) {
            indirect(Bottom, constants.control.x + 1);
            return;
        }
        smooth(false, l);
        stage(l + 1);
        indirect(RestrictCoarse, l + 2);
        cycle(l + 1);
        stage(l);
        indirect(ProlongCoarse, l + 1);
        smooth(false, l);
    };
    for (uint32_t iteration = 0; iteration < maxIterations; ++iteration) {
        indirect(ResetCorrection, 0);
        smooth(true, 0);
        stage(0);
        indirect(RestrictBase, 1);
        if (!splitCoarse && capacity <= 4096)
            indirect(CoarseCycle, constants.control.x + 1);
        else
            cycle(0);
        indirect(ProlongFine, 0);
        smooth(true, 0);
        indirect(Dot, dotRecord);
        reduce(iteration, 1);
        indirect(Direction, 0);
        indirect(Apply, dotRecord);
        reduce(iteration, 2);
        indirect(Update, 0);
        indirect(Residual, dotRecord);
        reduce(iteration, 3);
    }
    cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto copy = [&](ID3D12Resource *dst, uint64_t offset, ID3D12Resource *src, uint64_t size) {
        gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(dst, offset, src, 0, size);
        gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    copy(readback.resource.Get(), solves * 320, counts.resource.Get(), 256);
    copy(readback.resource.Get(), solves * 320 + 256, scalars.resource.Get(), 64);
    ++solves;
    if (audit) {
        if (!snapshot.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(cmd->GetDevice(IID_PPV_ARGS(&device)), "MGPCG validation device");
            snapshot = gpu::buffer(
                device.Get(),
                uint64_t(constants.grid.w) * (sizeof(FluidMacRow) + 28) + uint64_t(capacity) * 32 + 4096 * 4 +
                    (cutProjection ? uint64_t(constants.grid.w) * sizeof(FluidCutPressureRow) : 0),
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                L"MGPCG / independent hierarchy audit");
            failureSnapshot = gpu::buffer(
                device.Get(), snapshot.resource->GetDesc().Width + (4 + maxIterations) * 16,
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                L"MGPCG / GPU-selected unconverged solve snapshot");
        }
        uint64_t offset = 0;
        const std::pair<ID3D12Resource *, uint64_t> copies[]{
            {baseRows, uint64_t(constants.grid.w) * sizeof(FluidMacRow)},
            {map, uint64_t(constants.grid.w) * 4},
            {cells, uint64_t(constants.grid.w) * 16},
            {precisePressure.resource.Get(), uint64_t(constants.grid.w) * 8},
            {rows.resource.Get(), uint64_t(capacity) * 32},
            {factor.resource.Get(), 4096 * 4}};
        for (auto [src, size] : copies) {
            copy(snapshot.resource.Get(), offset, src, size);
            offset += size;
        }
        if (cutProjection)
            copy(snapshot.resource.Get(), offset, cutExact,
                 uint64_t(constants.grid.w) * sizeof(FluidCutPressureRow));
        // Preserve the actual capped substep, even if a later substep in the
        // same frame converges. The GPU selects copies; no extra CPU wait.
        gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        cmd->SetPredication(args.resource.Get(), 248, D3D12_PREDICATION_OP_EQUAL_ZERO);
        offset = 0;
        for (auto [src, size] : copies) {
            copy(failureSnapshot.resource.Get(), offset, src, size);
            offset += size;
        }
        if (cutProjection) {
            const auto size = uint64_t(constants.grid.w) * sizeof(FluidCutPressureRow);
            copy(failureSnapshot.resource.Get(), offset, cutExact, size);
            offset += size;
        }
        copy(failureSnapshot.resource.Get(), offset, scalars.resource.Get(), (4 + maxIterations) * 16);
        cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
        gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}
void FluidMacPressure::precondition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *rhsCorrection) {
    if (!solves || !cachedBase[0] || !rhsCorrection)
        throw std::runtime_error("Capacity preconditioning requires the current assembled MAC hierarchy");
    gpu::Event event(cmd, L"Fluid / reuse mixed MAC multigrid for capacity correction");
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(21, precisePressure.resource->GetGPUVirtualAddress());
    if (cutProjection)
        cmd->SetComputeRootUnorderedAccessView(22, cachedBase[5]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(23, rhsCorrection->GetGPUVirtualAddress());
    ID3D12Resource *resources[]{
        cachedBase[0],           cachedBase[1],          cachedBase[2],          cachedBase[3],
        cachedBase[4],           fine[0].resource.Get(), fine[0].resource.Get(), fine[1].resource.Get(),
        vectors.resource.Get(),  rows.resource.Get(),    rhs.resource.Get(),     error[0].resource.Get(),
        error[1].resource.Get(), work.resource.Get(),    counts.resource.Get(),  args.resource.Get(),
        partials.resource.Get(), scalars.resource.Get(), factor.resource.Get()};
    for (uint32_t i = 0; i < 19; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 2, resources[i]->GetGPUVirtualAddress());
    auto stage = [&](uint32_t level) {
        XMUINT4 value{level, 0, 0, 0};
        cmd->SetComputeRoot32BitConstants(1, 4, &value, 0);
    };
    auto direct = [&](Pass pass, uint32_t groups) {
        cmd->SetPipelineState(pipelines[pass].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    auto indirect = [&](Pass pass, uint32_t record) {
        cmd->SetPipelineState(pipelines[pass].Get());
        cmd->ExecuteIndirect(dispatch.Get(), 1, args.resource.Get(), record * 12, nullptr, 0);
        gpu::uav(cmd);
    };
    stage(0);
    // Restore work dimensions only. Hierarchy rows, compact lists and the
    // bottom factor remain those built by the current physical solve.
    direct(Prepare, 1);
    direct(ImportCorrection, (constants.grid.w + 127) / 128);
    gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto smooth = [&](bool base, uint32_t level) {
        stage(level);
        for (uint32_t sweep = 0; sweep < 2; ++sweep) {
            cmd->SetComputeRootUnorderedAccessView(
                base ? 8 : 13, (base ? fine[sweep] : error[sweep]).resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(
                base ? 9 : 14, (base ? fine[1 - sweep] : error[1 - sweep]).resource->GetGPUVirtualAddress());
            indirect(base ? FineSmooth : CoarseSmooth, base ? 0 : level + 1);
        }
        cmd->SetComputeRootUnorderedAccessView(base ? 8 : 13,
                                               (base ? fine[0] : error[0]).resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(base ? 9 : 14,
                                               (base ? fine[1] : error[1]).resource->GetGPUVirtualAddress());
    };
    std::function<void(uint32_t)> cycle = [&](uint32_t level) {
        stage(level);
        if (level + 1 == constants.control.x) {
            indirect(Bottom, constants.control.x + 1);
            return;
        }
        smooth(false, level);
        stage(level + 1);
        indirect(RestrictCoarse, level + 2);
        cycle(level + 1);
        stage(level);
        indirect(ProlongCoarse, level + 1);
        smooth(false, level);
    };
    indirect(ResetCorrection, 0);
    smooth(true, 0);
    stage(0);
    indirect(RestrictBase, 1);
    if (!splitCoarse && capacity <= 4096)
        indirect(CoarseCycle, constants.control.x + 1);
    else
        cycle(0);
    stage(0);
    indirect(ProlongFine, 0);
    smooth(true, 0);
    direct(ExportCorrection, (constants.grid.w + 127) / 128);
    gpu::transition(cmd, args.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Preserve outer SetPredication. No physical pressure or solve metrics are
    // exported: the caller measures its nonlinear and divergence residuals.
}
void FluidMacPressure::collect() {
    if (!solves)
        return;
    void *data;
    D3D12_RANGE range{0, solves * 320}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "MGPCG deferred metrics");
    uint32_t invalid = 0;
    bool capped = false;
    for (uint32_t i = 0; i < solves; ++i) {
        auto c = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) + i * 320);
        auto s = reinterpret_cast<const XMFLOAT4 *>(c + 64);
        invalid += c[21];
        lastIterations = c[22];
        peakIterations = std::max(peakIterations, lastIterations);
        totalIterations += lastIterations;
        ++totalSolves;
        exhausted += c[20] ? 1 : 0;
        if (c[20]) {
            capped = true;
            lastCappedSolve = totalSolves;
        }
        relativeResidual = s[0].x > 0 ? std::sqrt(double(s[0].z) / s[0].x) : 0;
        maxDivergence = s[0].w;
        peakFinalDivergence = std::max(peakFinalDivergence, maxDivergence);
        if (c[20])
            cappedMaxDivergence = std::max(cappedMaxDivergence, maxDivergence);
        for (uint32_t level = 0; level < constants.control.x; ++level)
            activePerLevel[level] = c[level + 1];
        if (!std::isfinite(relativeResidual) || !std::isfinite(maxDivergence) ||
            lastIterations > maxIterations)
            ++invalid;
    }
    readback.resource->Unmap(0, &written);
    if (invalid)
        throw std::runtime_error("Mixed MAC MGPCG invalid operator/factor/iteration");
    if (audit)
        validateSnapshot();
    if (audit && capped) {
        const auto size = failureSnapshot.resource->GetDesc().Width;
        D3D12_RANGE failureRange{0, SIZE_T(size)};
        gpu::check(failureSnapshot.resource->Map(0, &failureRange, &data), "MGPCG capped snapshot");
        const uint32_t header[]{0x3150474d,
                                3,
                                uint32_t(lastCappedSolve),
                                uint32_t(lastCappedSolve >> 32),
                                sizeof(Constants),
                                capacity,
                                maxIterations,
                                cutProjection ? 1u : 0u};
        std::ofstream out(outputFolder / ("mgpcg-failure-" + std::to_string(lastCappedSolve) + ".bin"),
                          std::ios::binary);
        out.write(reinterpret_cast<const char *>(header), sizeof(header));
        out.write(reinterpret_cast<const char *>(&constants), sizeof(constants));
        out.write(static_cast<const char *>(data), std::streamsize(size));
        failureSnapshot.resource->Unmap(0, &written);
        if (!out)
            throw std::runtime_error("Could not write capped MGPCG snapshot");
    }
}
void FluidMacPressure::report(std::ostream &out) const {
    out << "{\"levels\":" << constants.control.x << ",\"capacity\":" << capacity << ",\"coarseSchedule\":\""
        << (!splitCoarse && capacity <= 4096 ? "single-group" : "distributed") << "\""
        << ",\"allocatedBytes\":" << bytes << ",\"maxIterations\":" << maxIterations
        << ",\"lastIterations\":" << lastIterations << ",\"peakIterations\":" << peakIterations
        << ",\"meanIterations\":" << double(totalIterations) / std::max(uint64_t(1), totalSolves)
        << ",\"exhaustedSolves\":" << exhausted << ",\"relativeResidual\":" << relativeResidual
        << ",\"lastCappedSolve\":" << lastCappedSolve << ",\"maxDivergence\":" << maxDivergence
        << ",\"hierarchyError\":" << hierarchyError << ",\"peakFinalDivergence\":" << peakFinalDivergence
        << ",\"cappedMaxDivergence\":" << cappedMaxDivergence << ",\"factorError\":" << factorError
        << ",\"residualError\":" << residualError << ",\"validated\":" << (validated ? "true" : "false")
        << ",\"activePerLevel\":[";
    for (uint32_t level = 0; level < constants.control.x; ++level) {
        if (level)
            out << ',';
        out << activePerLevel[level];
    }
    out << "]}";
}
} // namespace lab
