#include "fluid_carrier_projection.h"
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
void copyCarrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *dst, uint64_t at, ID3D12Resource *src,
                 uint64_t size) {
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(dst, at, src, 0, size);
    gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
float metric(uint32_t bits) {
    float x;
    memcpy(&x, &bits, 4);
    return x;
}
} // namespace
FluidCarrierProjection::FluidCarrierProjection(ID3D12Device *device, const std::filesystem::path &folder,
                                               XMUINT4 fine, XMFLOAT4 minimum, XMFLOAT4 maximum, bool couple,
                                               uint32_t pressureCycles)
    : coupled(couple), capacityPressureCycles(pressureCycles) {
    if (!pressureCycles || pressureCycles > 4)
        throw std::runtime_error("Capacity pressure cycles must be between 1 and 4");
    constants.fine = fine;
    constants.coarse = {(fine.x + 1) / 2, (fine.y + 1) / 2, (fine.z + 1) / 2, 0};
    auto &c = constants.coarse;
    c.w = c.x * c.y * c.z;
    constants.minimumCell = minimum;
    constants.maximum = maximum;
    // The carrier feasibility tolerance accounts for the existing physical
    // pressure residual in legacy mode. Coupled mode also requires a signed
    // phase supersolution; its inventory/capacity storage is FP64, never clipped.
    constants.parameters = {0, 5e-7f, 1e-20f, 0};
    fineFaces = 3 * (fine.x + 1) * (fine.y + 1) * (fine.z + 1);
    coarseFaces = 3 * (c.x + 1) * (c.y + 1) * (c.z + 1);
    auto make = [&](uint64_t size, const wchar_t *name) {
        bytes += std::max(uint64_t(256), size);
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    extended = make(uint64_t(fineFaces) * 8, L"Fluid carrier / extended fine face volume flux");
    weights =
        make(uint64_t(coarseFaces) * (coupled ? 16 : 8), L"Fluid carrier / adjustable face conductance");
    rates = make(uint64_t(coarseFaces) * 8, L"Fluid carrier / fixed coarse pressure flux");
    rows = make(uint64_t(c.w) * 32, L"Fluid carrier / capacity inequality rows");
    potential = make(uint64_t(c.w) * 8, L"Fluid carrier / nonnegative dual potential");
    fraction = make(uint64_t(c.w) * 8, L"Fluid carrier / bounded trial liquid fraction");
    if (coupled) {
        lambda[0] = make(uint64_t(fine.w) * 8, L"Fluid capacity / mixed MAC multiplier ping");
        lambda[1] = make(uint64_t(fine.w) * 8, L"Fluid capacity / mixed MAC multiplier pong");
        pressureRates = make(uint64_t(coarseFaces) * 8, L"Fluid capacity / harmonic pressure flux");
    }
    controlBytes = coupled ? (18 + 2 * (maxIterations + 1)) * 4 : 64;
    control = make(controlBytes, L"Fluid carrier / convergence and work counters");
    arguments = make(40, L"Fluid carrier / GPU predicate and indirect dispatch");
    readback =
        gpu::buffer(device, 1024, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid carrier / existing-fence counters and timing");
    D3D12_ROOT_PARAMETER p[23]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants = {0, 0, sizeof(Constants) / 4};
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants = {1, 0, 1};
    const uint32_t rootCount = coupled ? 23u : 16u;
    for (uint32_t i = 2; i < rootCount; i++) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 2;
    }
    D3D12_ROOT_SIGNATURE_DESC desc{rootCount, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Carrier root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Carrier root");
    const char *names[]{"CarrierClear",       "CarrierFaces",         "CarrierRows",
                        "CarrierSweep",       "CarrierResidual",      "CarrierPrepare",
                        "CarrierExport",      "CarrierFinish",        "CarrierMacInitialize",
                        "CarrierLiquidSweep", "CarrierPressureFaces", "CarrierLiquidResidual",
                        "CarrierLiquidRhs",   "CarrierApplyMultigrid"};
    for (uint32_t i = 0; i < (coupled ? pipelines.size() : 8); i++) {
        auto code =
            gpu::bytes(folder / "shaders" / (std::string(names[i]) + (coupled ? "-coupled.dxil" : ".dxil")));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC s{12, 1, &a};
    gpu::check(device->CreateCommandSignature(&s, nullptr, IID_PPV_ARGS(&indirect)), "Carrier dispatch");
    D3D12_QUERY_HEAP_DESC q{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 32, 0};
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Carrier timestamps");
}
void FluidCarrierProjection::beginFrame(ID3D12GraphicsCommandList *cmd, bool validate, bool reset) {
    calls = 0;
    appliedMask = 0;
    validateFrame = validate;
    if (reset)
        steps = iterations = 0;
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootUnorderedAccessView(13, control.resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(pipelines[0].Get());
    cmd->Dispatch(1, 1, 1);
    gpu::uav(cmd);
}
ID3D12Resource *FluidCarrierProjection::record(ID3D12GraphicsCommandList *cmd, const FluidCutCellGpuView &cut,
                                               ID3D12Resource *resident, ID3D12Resource *cells,
                                               ID3D12Resource *canonical, float dt,
                                               const FluidMacConstraintView &mac) {
    if (cut.preciseCapacity != coupled)
        throw std::runtime_error("Carrier/cut-cell numeric format mismatch");
    if (calls >= 16 || !canonical || !cells || !(dt > 0) || !std::isfinite(dt))
        throw std::runtime_error("Invalid carrier projection call");
    if (coupled && (!mac.map || !mac.state || !mac.rows || !mac.preciseRows || !mac.faces || !mac.solver))
        throw std::runtime_error("Coupled capacity needs the actual mixed cut-pressure operator");
    constants.parameters.x = dt;
    constants.parameters.w = float((cut.timeCentered ? 1u : 0u) | (cut.swept ? 2u : 0u));
    auto &snapshot = snapshots[calls];
    snapshotConstants[calls] = constants;
    snapshotSwept[calls] = cut.swept;
    const uint64_t n = constants.coarse.w, f = constants.fine.w, cf = coarseFaces, ff = fineFaces;
    const uint64_t sizes[]{n * (coupled ? 32 : 16),
                           n * (coupled ? 16 : 8),
                           f * 8,
                           f * 16,
                           ff * 4,
                           ff * 8,
                           ff * 8,
                           cf * (coupled ? 16 : 8),
                           cf * 8,
                           n * 32,
                           n * 8,
                           n * 8,
                           f * 4,
                           n * 4,
                           f * sizeof(FluidMacRow),
                           f * sizeof(FluidCutPressureRow),
                           f * 8,
                           cf * 8,
                           ff * 16,
                           ff * 8,
                           ff * 16};
    if (validateFrame && !snapshot.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(resident->GetDevice(IID_PPV_ARGS(&device)), "Carrier snapshot device");
        uint64_t size = 0;
        for (uint32_t i = 0; i < (coupled ? offsets.size() : 12); i++) {
            offsets[i] = size;
            size += sizes[i];
        }
        snapshotDataBytes = size;
        snapshot =
            gpu::buffer(device.Get(), size + controlBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid carrier / independent immutable snapshot");
    }
    ID3D12Resource *buffers[]{resident,
                              cut.coarseVolume,
                              cut.fineVolume,
                              cells,
                              cut.pressureArea,
                              canonical,
                              extended.resource.Get(),
                              weights.resource.Get(),
                              rates.resource.Get(),
                              rows.resource.Get(),
                              potential.resource.Get(),
                              control.resource.Get(),
                              arguments.resource.Get(),
                              fraction.resource.Get(),
                              mac.map,
                              mac.state,
                              mac.rows,
                              mac.preciseRows,
                              lambda[0].resource.Get(),
                              lambda[1].resource.Get(),
                              pressureRates.resource.Get()};
    if (validateFrame)
        for (uint32_t i = 0; i < 6; i++)
            copyCarrier(cmd, snapshot.resource.Get(), offsets[i], buffers[i], sizes[i]);
    if (validateFrame && coupled)
        for (uint32_t i = 0; i < 4; ++i)
            copyCarrier(cmd, snapshot.resource.Get(), offsets[12 + i], buffers[14 + i], sizes[12 + i]);
    if (validateFrame && coupled)
        copyCarrier(cmd, snapshot.resource.Get(), offsets[20], mac.faces, sizes[20]);
    gpu::Event event(cmd, coupled ? L"Fluid / coupled capacity and mixed MAC pressure"
                                  : L"Fluid / capacity-constrained air carrier extension");
    auto bind = [&] {
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRoot32BitConstants(0, sizeof(Constants) / 4, &constants, 0);
        cmd->SetComputeRoot32BitConstant(1, 0, 0);
        for (uint32_t i = 0; i < (coupled ? 21u : 14u); i++)
            cmd->SetComputeRootUnorderedAccessView(i + 2, buffers[i]->GetGPUVirtualAddress());
    };
    bind();
    auto pass = [&](uint32_t p, uint32_t groups) {
        cmd->SetPipelineState(pipelines[p].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, calls * 2);
    pass(1, (coarseFaces + 127) / 128);
    if (coupled) {
        pass(8, (constants.fine.w + 127) / 128);
        pass(10, (coarseFaces + 127) / 128);
    }
    pass(2, (constants.coarse.w + 127) / 128);
    cmd->SetComputeRoot32BitConstant(1, 0, 0);
    pass(4, (constants.coarse.w + 127) / 128);
    if (coupled)
        pass(11, (constants.fine.w + 127) / 128);
    pass(5, 1);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    for (uint32_t iteration = 0; iteration < maxIterations; iteration++) {
        cmd->SetPredication(arguments.resource.Get(), 32, D3D12_PREDICATION_OP_EQUAL_ZERO);
        if (coupled) {
            // Inner work budget only: final physical and signed phase residuals
            // still gate export. Keep the four-cycle reference selectable for
            // controlled profiling and difficult coupled configurations.
            for (uint32_t cycle = 0; cycle < capacityPressureCycles; ++cycle) {
                pass(12, (constants.fine.w + 127) / 128);
                mac.solver->precondition(cmd, lambda[1].resource.Get());
                bind();
                pass(13, (constants.fine.w + 127) / 128);
            }
            pass(10, (coarseFaces + 127) / 128);
        }
        cmd->SetPipelineState(pipelines[3].Get());
        for (uint32_t colour = 0; colour < 2; colour++) {
            cmd->SetComputeRoot32BitConstant(1, colour, 0);
            cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
            gpu::uav(cmd);
        }
        cmd->SetPipelineState(pipelines[4].Get());
        cmd->ExecuteIndirect(indirect.Get(), 1, arguments.resource.Get(), 0, nullptr, 0);
        gpu::uav(cmd);
        if (coupled)
            pass(11, (constants.fine.w + 127) / 128);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        pass(5, 1);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    cmd->SetPredication(nullptr, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->SetComputeRoot32BitConstant(1, 2, 0);
    pass(4, (constants.coarse.w + 127) / 128);
    if (coupled)
        pass(11, (constants.fine.w + 127) / 128);
    pass(6, (fineFaces + 127) / 128);
    pass(7, 1);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, calls * 2 + 1);
    ++calls;
    if (validateFrame) {
        for (uint32_t i = 6; i < 11; i++)
            copyCarrier(cmd, snapshot.resource.Get(), offsets[i], buffers[i], sizes[i]);
        copyCarrier(cmd, snapshot.resource.Get(), offsets[11], fraction.resource.Get(), sizes[11]);
        if (coupled) {
            copyCarrier(cmd, snapshot.resource.Get(), offsets[16], lambda[0].resource.Get(), sizes[16]);
            copyCarrier(cmd, snapshot.resource.Get(), offsets[17], pressureRates.resource.Get(), sizes[17]);
        }
        copyCarrier(cmd, snapshot.resource.Get(), snapshotDataBytes, control.resource.Get(), controlBytes);
    }
    return extended.resource.Get();
}
void FluidCarrierProjection::captureApplied(ID3D12GraphicsCommandList *cmd, ID3D12Resource *faces,
                                            ID3D12Resource *canonical) {
    if (!coupled || !calls || (appliedMask & (1u << (calls - 1))))
        throw std::runtime_error("Capacity application audit must follow exactly one correction");
    appliedMask |= 1u << (calls - 1);
    if (validateFrame) {
        auto target = snapshots[calls - 1].resource.Get();
        copyCarrier(cmd, target, offsets[18], faces, uint64_t(fineFaces) * 16);
        copyCarrier(cmd, target, offsets[19], canonical, uint64_t(fineFaces) * 8);
    }
}
void FluidCarrierProjection::finishFrame(ID3D12GraphicsCommandList *cmd) {
    copyCarrier(cmd, readback.resource.Get(), 0, control.resource.Get(), 64);
    if (calls)
        cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, calls * 2,
                              readback.resource.Get(), 128);
}
void FluidCarrierProjection::collect(uint64_t frequency) {
    if (coupled && appliedMask != (1u << calls) - 1)
        throw std::runtime_error("Capacity correction was not applied before particle transfer");
    void *data;
    D3D12_RANGE read{0, 1024}, written{0, 0};
    gpu::check(readback.resource->Map(0, &read, &data), "Carrier readback");
    memcpy(stats.data(), data, 64);
    const auto ticks = reinterpret_cast<const uint64_t *>(static_cast<const char *>(data) + 128);
    gpuMs = 0;
    for (uint32_t i = 0; i < calls; i++)
        gpuMs += double(ticks[2 * i + 1] - ticks[2 * i]) * 1000 / frequency;
    readback.resource->Unmap(0, &written);
    ++frames;
    totalMs += gpuMs;
    steps += stats[4];
    totalSubsteps += stats[4];
    iterations += stats[3];
    eligibleFaces += stats[7];
    changedFaces += stats[8];
    peakResidual = std::max(peakResidual, double(metric(stats[9])));
    const bool failed = stats[5] || stats[6] || stats[15] || stats[4] != calls ||
                        peakResidual > constants.parameters.y * 1.001;
    if (validateFrame && calls) {
        for (uint32_t call = 0; call < calls; call++)
            validateSnapshot(call);
        ++auditedFrames;
    } else if (validateFrame) {
        if (std::any_of(stats.begin(), stats.end(), [](uint32_t v) { return v != 0; }))
            throw std::runtime_error("Idle carrier frame recorded work");
        ++auditedIdleFrames;
    }
    if (failed)
        throw std::runtime_error("Air carrier capacity projection did not converge: " +
                                 std::to_string(metric(stats[9])));
}
void FluidCarrierProjection::validateSnapshot(uint32_t call) {
    auto &snapshot = snapshots[call];
    const auto &config = snapshotConstants[call];
    void *data;
    D3D12_RANGE read{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &read, &data), "Carrier snapshot");
    const auto raw = static_cast<const char *>(data);
    std::array<uint32_t, 16> solveStats{};
    memcpy(solveStats.data(), raw + snapshotDataBytes, 64);
    const bool failed = solveStats[5] || solveStats[13] || solveStats[15];
    const auto source = readFluidNumbers<FluidDouble4>(raw + offsets[0], config.coarse.w, coupled);
    const auto cap = readFluidNumbers<FluidDouble2>(raw + offsets[1], config.coarse.w, coupled);
    const auto fineVolume = reinterpret_cast<const XMFLOAT2 *>(raw + offsets[2]);
    const auto cells = reinterpret_cast<const XMFLOAT4 *>(raw + offsets[3]);
    const auto area = reinterpret_cast<const float *>(raw + offsets[4]);
    auto value = [&](uint32_t buffer, uint64_t i) {
        double x;
        memcpy(&x, raw + offsets[buffer] + i * 8, 8);
        return x;
    };
    const auto f = config.fine, c = config.coarse;
    const uint32_t fs = fineFaces / 3, cs = coarseFaces / 3;
    using Coord = std::array<uint32_t, 3>;
    auto index = [](Coord p, XMUINT4 g) { return (p[2] * g.y + p[1]) * g.x + p[0]; };
    auto coord = [](uint32_t i, XMUINT4 g) { return Coord{i % g.x, i / g.x % g.y, i / (g.x * g.y)}; };
    auto widths = [&](Coord p, uint32_t axis) {
        return std::max(
            0., std::min(2 * double(config.minimumCell.w),
                         double((&config.maximum.x)[axis]) - (double((&config.minimumCell.x)[axis]) +
                                                              p[axis] * 2 * double(config.minimumCell.w))));
    };
    auto support = [&](uint32_t id) {
        return (uint32_t(config.parameters.w) & 1) ? double(fineVolume[id].x) + fineVolume[id].y
                                                   : double(fineVolume[id].x);
    };
    std::vector<double> w(coarseFaces), flow(coarseFaces), diagonal(c.w), rhs(c.w), ap(c.w), scale(c.w);
    std::vector<double> rightWeight(coarseFaces);
    std::vector<uint32_t> airChildren(c.w);
    std::vector<double> harmonic(coarseFaces), liquidDelta(f.w), appliedDelta(f.w);
    std::vector<double> beforePhysical(f.w), afterPhysical(f.w);
    const auto leafMap = coupled ? reinterpret_cast<const uint32_t *>(raw + offsets[12]) : nullptr;
    const auto macState = coupled ? reinterpret_cast<const uint32_t *>(raw + offsets[13]) : nullptr;
    const auto macRows = coupled ? reinterpret_cast<const FluidMacRow *>(raw + offsets[14]) : nullptr;
    const auto beforeFaces = coupled ? reinterpret_cast<const XMFLOAT4 *>(raw + offsets[20]) : nullptr;
    const auto afterFaces = coupled ? reinterpret_cast<const XMFLOAT4 *>(raw + offsets[18]) : nullptr;
    auto parentId = [&](Coord p) { return index(Coord{p[0] / 2, p[1] / 2, p[2] / 2}, c); };
    if (coupled)
        for (uint32_t id = 0; id < f.w; ++id)
            if (cells[id].z != 1 && support(id) > 0)
                ++airChildren[parentId(coord(id, f))];
    auto leafSize = [&](Coord p) { return macState[parentId(p)] & 1 ? 2u : 1u; };
    auto fineWidth = [&](Coord p, uint32_t a) {
        return std::max(0., std::min(double(config.minimumCell.w),
                                     double((&config.maximum.x)[a]) - (double((&config.minimumCell.x)[a]) +
                                                                       p[a] * double(config.minimumCell.w))));
    };
    auto patchGradient = [&](Coord p, uint32_t a, bool capacity) {
        auto l = p;
        --l[a];
        const uint32_t width = std::max(leafSize(l), leafSize(p)), b = (a + 1) % 3, d = (a + 2) % 3;
        if (width == 2) {
            p[b] &= ~1u;
            p[d] &= ~1u;
        }
        double sum = 0;
        for (uint32_t y = 0; y < width; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                auto right = p;
                right[b] += x;
                right[d] += y;
                auto left = right;
                --left[a];
                for (uint32_t side = 0; side < 2; ++side) {
                    auto at = side ? right : left;
                    const uint32_t id = index(at, f);
                    const double v = capacity           ? (cells[id].z == 1 ? 0 : value(10, parentId(at)))
                                     : cells[id].z == 1 ? value(16, leafMap[id])
                                                        : 0;
                    sum += (side ? -1 : 1) * v;
                }
            }
        return sum / (width * width);
    };
    auto patchAir = [&](Coord p, uint32_t a) {
        auto l = p;
        --l[a];
        const uint32_t width = std::max(leafSize(l), leafSize(p)), b = (a + 1) % 3, d = (a + 2) % 3;
        if (width == 2) {
            p[b] &= ~1u;
            p[d] &= ~1u;
        }
        std::array<double, 2> air{};
        for (uint32_t y = 0; y < width; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                auto r = p;
                r[b] += x;
                r[d] += y;
                auto lo = r;
                --lo[a];
                air[0] += cells[index(lo, f)].z != 1;
                air[1] += cells[index(r, f)].z != 1;
            }
        air[0] /= width * width;
        air[1] /= width * width;
        return air;
    };
    std::vector<uint32_t> parent(c.w);
    for (uint32_t i = 0; i < c.w; i++)
        parent[i] = i;
    auto component = [&](uint32_t i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    };
    bool bad = false;
    // Assemble from individual fine faces, independently of the GPU coarse-face gather.
    for (uint32_t id = 0; id < fineFaces; id++) {
        const uint32_t a = id / fs;
        auto p = coord(id % fs, {f.x + 1, f.y + 1, f.z + 1, 0});
        const bool live = p[0] < f.x + (a == 0) && p[1] < f.y + (a == 1) && p[2] < f.z + (a == 2);
        const double original = value(5, id), actual = value(6, id);
        double expected = original;
        if (coupled && live && p[a] > 0 && p[a] < (&f.x)[a]) {
            auto l = p;
            --l[a];
            const uint32_t left = index(l, f), right = index(p, f);
            const bool open = support(left) > 0 && support(right) > 0 && area[id] > 0;
            double weight = 0, lambdaVolume = 0, deltaVolume = 0;
            if (open) {
                const uint32_t wl = leafSize(l), wr = leafSize(p);
                const double distance = std::max(wl, wr) > 1 ? .5 * (wl + wr) * config.minimumCell.w
                                                             : .5 * (fineWidth(l, a) + fineWidth(p, a));
                weight = double(config.parameters.x) * area[id] / distance;
                lambdaVolume = weight * patchGradient(p, a, false);
                deltaVolume = lambdaVolume + weight * patchGradient(p, a, true);
                if (!failed)
                    expected += deltaVolume / config.parameters.x;
                if (parentId(l) == parentId(p) && (cells[left].z == 1) != (cells[right].z == 1))
                    diagonal[parentId(p)] += weight;
            }
            if (p[a] % 2 == 0) {
                Coord coarseFace{p[0] / 2, p[1] / 2, p[2] / 2};
                const uint32_t fi = a * cs + index(coarseFace, {c.x + 1, c.y + 1, c.z + 1, 0});
                flow[fi] += original;
                const auto air = patchAir(p, a);
                w[fi] += weight * air[0];
                rightWeight[fi] += weight * air[1];
                harmonic[fi] += lambdaVolume;
            }
            if (cells[left].z == 1)
                liquidDelta[leafMap[left]] += deltaVolume;
            if (cells[right].z == 1)
                liquidDelta[leafMap[right]] -= deltaVolume;
            const double applied = value(19, id), appliedChange = config.parameters.x * (applied - original);
            if (cells[left].z == 1)
                appliedDelta[leafMap[left]] += appliedChange;
            if (cells[right].z == 1)
                appliedDelta[leafMap[right]] -= appliedChange;
            if (cells[left].z == 1) {
                beforePhysical[leafMap[left]] += config.parameters.x * original;
                afterPhysical[leafMap[left]] += config.parameters.x * applied;
            }
            if (cells[right].z == 1) {
                beforePhysical[leafMap[right]] -= config.parameters.x * original;
                afterPhysical[leafMap[right]] -= config.parameters.x * applied;
            }
            const bool internalCoarse = leafMap[left] == leafMap[right] && leafSize(l) == 2;
            if (!internalCoarse)
                bad |= applied != actual;
            bad |= !std::isfinite(applied);
            if (open) {
                const double velocityError = std::abs(double(afterFaces[id].x) - applied / area[id]);
                appliedVelocityError =
                    std::max(appliedVelocityError, velocityError / (1 + std::abs(applied / area[id])));
            }
        }
        if (!coupled && live && p[a] > 0 && p[a] < (&f.x)[a] && p[a] % 2 == 0) {
            auto l = p;
            --l[a];
            Coord rcoarse{p[0] / 2, p[1] / 2, p[2] / 2}, lcoarse = rcoarse;
            --lcoarse[a];
            const uint32_t left = index(l, f), right = index(p, f);
            const uint32_t faceId = a * cs + index(rcoarse, {c.x + 1, c.y + 1, c.z + 1, 0});
            flow[faceId] += original;
            if (cells[left].z == 0 && cells[right].z == 0 && support(left) > 0 && support(right) > 0 &&
                area[id] > 0) {
                const double weight =
                    double(config.parameters.x) * area[id] / (.5 * (widths(lcoarse, a) + widths(rcoarse, a)));
                w[faceId] += weight;
                if (!failed)
                    expected += weight * (value(10, index(lcoarse, c)) - value(10, index(rcoarse, c))) /
                                config.parameters.x;
            }
        }
        const double error = std::abs(actual - expected);
        extensionError = std::max(extensionError, error / (1 + std::abs(expected)));
        if (expected == original)
            bad |= memcmp(raw + offsets[5] + uint64_t(id) * 8, raw + offsets[6] + uint64_t(id) * 8, 8) != 0;
        else
            bad |= !std::isfinite(actual) || error > std::max(1e-13, std::abs(expected) * 2e-12);
        if (coupled && live) {
            bad |= memcmp(&beforeFaces[id].y, &afterFaces[id].y, 8) != 0; // FLIP reference and weight
            if (p[a] == 0 || p[a] == (&f.x)[a]) {
                bad |= original != 0;
                bad |= value(19, id) != original;
                bad |= memcmp(&beforeFaces[id], &afterFaces[id], sizeof(XMFLOAT4)) != 0;
            }
        }
    }
    double frameMixedDivergence = 0, frameAppliedDivergence = 0;
    double frameOriginalPhysical = 0, frameProposedPhysical = 0, frameAppliedPhysical = 0;
    uint32_t worstLiquid = 0;
    if (coupled && snapshotSwept[call])
        for (uint32_t id = 0; id < f.w; ++id)
            if (cells[id].z == 1) {
                const double swept = double(fineVolume[id].x) - fineVolume[id].y;
                beforePhysical[leafMap[id]] += swept;
                afterPhysical[leafMap[id]] += swept;
            }
    if (coupled)
        for (uint32_t id = 0; id < f.w; ++id) {
            if (cells[id].z != 1 || leafMap[id] != id)
                continue;
            const double h = config.minimumCell.w;
            const double divisor =
                double(config.parameters.x) * std::max(1e-20, double(macRows[id].volumeUnits) * h * h * h);
            const double divergence = std::abs(liquidDelta[id]) / divisor;
            frameMixedDivergence = std::max(frameMixedDivergence, divergence);
            const double physical = std::abs(beforePhysical[id] + liquidDelta[id]) / divisor;
            if (physical > frameProposedPhysical) {
                frameProposedPhysical = physical;
                worstLiquid = id;
            }
            frameAppliedDivergence = std::max(frameAppliedDivergence, std::abs(appliedDelta[id]) / divisor);
            frameOriginalPhysical = std::max(frameOriginalPhysical, std::abs(beforePhysical[id]) / divisor);
            frameAppliedPhysical = std::max(frameAppliedPhysical, std::abs(afterPhysical[id]) / divisor);
        }
    mixedDivergence = std::max(mixedDivergence, frameMixedDivergence);
    appliedDivergence = std::max(appliedDivergence, frameAppliedDivergence);
    originalPhysicalDivergence = std::max(originalPhysicalDivergence, frameOriginalPhysical);
    proposedPhysicalDivergence = std::max(proposedPhysicalDivergence, frameProposedPhysical);
    appliedPhysicalDivergence = std::max(appliedPhysicalDivergence, frameAppliedPhysical);
    bad |= appliedVelocityError > 2e-6 || frameOriginalPhysical > .000101;
    bad |= frameProposedPhysical > 1.001e-6 || (!failed && frameAppliedPhysical > 1.01e-6);
    for (uint32_t id = 0; id < c.w; id++) {
        const double trialFraction = value(11, id);
        bad |= !std::isfinite(trialFraction) || trialFraction < 0 || trialFraction > 1;
        rhs[id] = double(source[id].w) - cap[id].x;
        ap[id] = double(cap[id].x) * trialFraction - source[id].w;
        scale[id] = double(source[id].w) + cap[id].x;
        if (coupled) {
            bad |= value(9, id * 4 + 3) != double(airChildren[id] != 0);
            bad |= !airChildren[id] && value(10, id) != 0;
        }
    }
    for (uint32_t id = 0; id < coarseFaces; id++) {
        const double right = coupled ? rightWeight[id] : w[id];
        weightError =
            std::max(weightError, std::abs(w[id] - value(7, coupled ? id * 2 : id)) / (1 + std::abs(w[id])));
        if (coupled)
            weightError =
                std::max(weightError, std::abs(right - value(7, id * 2 + 1)) / (1 + std::abs(right)));
        bad |= std::abs(flow[id] - value(8, id)) > std::max(1e-13, std::abs(flow[id]) * 1e-12);
        if (coupled)
            bad |= std::abs(config.parameters.x * (value(17, id) - flow[id]) - harmonic[id]) >
                   std::max(1e-13, std::abs(harmonic[id]) * 2e-12);
        const uint32_t a = id / cs;
        auto p = coord(id % cs, {c.x + 1, c.y + 1, c.z + 1, 0});
        const bool live = p[0] < c.x + (a == 0) && p[1] < c.y + (a == 1) && p[2] < c.z + (a == 2);
        if (!live || p[a] == 0 || p[a] == (&c.x)[a])
            continue;
        auto l = p;
        --l[a];
        const uint32_t li = index(l, c), ri = index(p, c);
        double Q = config.parameters.x * flow[id];
        rhs[li] -= Q;
        rhs[ri] += Q;
        scale[li] += std::abs(Q);
        scale[ri] += std::abs(Q);
        diagonal[li] += w[id];
        diagonal[ri] += right;
        if (w[id] > 0 || right > 0)
            parent[component(li)] = component(ri);
        const double gradient = w[id] * value(10, li) - right * value(10, ri);
        const double corrected = Q + gradient + harmonic[id];
        const double phase = corrected * value(11, corrected >= 0 ? li : ri);
        ap[li] += phase;
        ap[ri] -= phase;
    }
    double worst = 0;
    uint32_t worstCell = 0;
    double worstDeficit = 0;
    uint32_t worstDeficitCell = 0;
    for (uint32_t id = 0; id < c.w; id++) {
        const double p = value(10, id), s = std::max(1e-20, scale[id]);
        const double error = std::abs(ap[id]) / s;
        const double deficit = std::max(0., -ap[id]) / s;
        if (deficit > worstDeficit) {
            worstDeficit = deficit;
            worstDeficitCell = id;
        }
        if (error > worst) {
            worst = error;
            worstCell = id;
        }
        matrixError = std::max(matrixError, std::max(std::abs(rhs[id] - value(9, id * 4)),
                                                     std::abs(diagonal[id] - value(9, id * 4 + 1))) /
                                                (1 + std::abs(rhs[id]) + diagonal[id]));
        bad |= !std::isfinite(p) || p < 0 || (p > 0 && value(11, id) != 1);
    }
    std::ostringstream message;
    bad |= coupled && worstDeficit > 1.001e-12;
    auditedPhaseDeficit = std::max(auditedPhaseDeficit, worstDeficit);
    auditedPhaseResidual = std::max(auditedPhaseResidual, worst);
    if (failed || bad || worst > config.parameters.y * 1.001 || weightError > 2e-12 || matrixError > 2e-12 ||
        extensionError > 2e-12) {
        auto p = coord(worstCell, c);
        message << "Carrier projection audit failed at frame " << frames << ", substep " << call << ", cell ["
                << p[0] << ',' << p[1] << ',' << p[2] << "], residual " << worst << ", rhs " << rhs[worstCell]
                << ", conductance " << diagonal[worstCell] << ", old mass " << source[worstCell].w
                << ", capacity " << cap[worstCell].x << ", previous capacity " << cap[worstCell].y
                << ", isolated rows " << solveStats[15] << ", iterations " << solveStats[2] << ", reference "
                << matrixError;
        if (coupled) {
            const auto dc = coord(worstDeficitCell, c);
            message << "; phase deficit " << worstDeficit << " at [" << dc[0] << ',' << dc[1] << ',' << dc[2]
                    << "] q/cap/oldcap " << source[worstDeficitCell].w << '/' << cap[worstDeficitCell].x
                    << '/' << cap[worstDeficitCell].y;
        }
        if (coupled) {
            const auto lp = coord(worstLiquid, f);
            message << "; mixed correction divergence " << frameMixedDivergence << ", applied divergence "
                    << frameAppliedDivergence << ", velocity cache error " << appliedVelocityError
                    << ", physical divergence before/proposed/after " << frameOriginalPhysical << '/'
                    << frameProposedPhysical << '/' << frameAppliedPhysical << "; worst liquid [" << lp[0]
                    << ',' << lp[1] << ',' << lp[2] << "] width " << leafSize(lp) << ", volume units "
                    << macRows[worstLiquid].volumeUnits << ", lambda " << value(16, worstLiquid)
                    << ", parent p " << value(10, parentId(lp)) << ", parent fraction "
                    << value(11, parentId(lp));
            const auto history = reinterpret_cast<const uint32_t *>(raw + snapshotDataBytes);
            const auto parentCell = parentId(lp);
            double oldGeometry = 0, nextGeometry = 0;
            uint32_t types[3]{};
            for (uint32_t z = 0; z < 2; ++z)
                for (uint32_t y = 0; y < 2; ++y)
                    for (uint32_t x = 0; x < 2; ++x) {
                        Coord child{(lp[0] & ~1u) + x, (lp[1] & ~1u) + y, (lp[2] & ~1u) + z};
                        if (child[0] >= f.x || child[1] >= f.y || child[2] >= f.z)
                            continue;
                        const auto i = index(child, f);
                        oldGeometry += fineVolume[i].y;
                        nextGeometry += fineVolume[i].x;
                        ++types[std::min(2u, uint32_t(cells[i].z))];
                    }
            message << "; liquid parent source/newcap/oldcap " << source[parentCell].w << '/'
                    << cap[parentCell].x << '/' << cap[parentCell].y << ", summed fine geometry old/new "
                    << oldGeometry << '/' << nextGeometry << ", geometric compatibility error "
                    << double(source[parentCell].w) - cap[parentCell].x + nextGeometry - oldGeometry
                    << ", air/liquid/solid " << types[0] << '/' << types[1] << '/' << types[2];
            for (uint32_t i :
                 {0u, 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 249u, 250u, 251u, 252u, 253u, 254u, 255u, 256u})
                if (i <= solveStats[2])
                    message << "; iteration " << i << " phase/div " << metric(history[18 + 2 * i]) << '/'
                            << metric(history[19 + 2 * i]);
        }
        double componentRhs = 0, componentScale = 0, componentCapacity = 0, maxPotential = 0;
        uint32_t componentCells = 0;
        const auto rootCell = component(worstCell);
        for (uint32_t i = 0; i < c.w; i++)
            if (component(i) == rootCell) {
                componentRhs += rhs[i];
                componentScale += scale[i];
                componentCapacity += cap[i].x;
                maxPotential = std::max(maxPotential, value(10, i));
                ++componentCells;
            }
        message << "; component cells " << componentCells << ", rhs " << componentRhs << ", scale "
                << componentScale << ", capacity " << componentCapacity << ", max potential " << maxPotential;
        // Failure-only diagnostic: solve the unmodified scalar transport to
        // distinguish an overly strong certificate from actual overfill. This
        // never feeds back into simulation or replaces the independent audit.
        std::vector<double> transportDiagonal(c.w), concentration(c.w), next(c.w);
        for (uint32_t i = 0; i < c.w; ++i)
            transportDiagonal[i] = cap[i].x;
        auto visitFlow = [&](auto &&visit) {
            for (uint32_t id = 0; id < coarseFaces; ++id) {
                const uint32_t a = id / cs;
                auto at = coord(id % cs, {c.x + 1, c.y + 1, c.z + 1, 0});
                if (at[0] >= c.x + (a == 0) || at[1] >= c.y + (a == 1) || at[2] >= c.z + (a == 2) ||
                    at[a] == 0 || at[a] == (&c.x)[a])
                    continue;
                auto lo = at;
                --lo[a];
                const uint32_t l = index(lo, c), r = index(at, c);
                const double q = config.parameters.x * flow[id];
                visit(q >= 0 ? l : r, q >= 0 ? r : l, std::abs(q));
            }
        };
        visitFlow([&](uint32_t donor, uint32_t, double q) { transportDiagonal[donor] += q; });
        double transportResidual = 0;
        for (uint32_t iteration = 0; iteration < 256; ++iteration) {
            for (uint32_t i = 0; i < c.w; ++i)
                next[i] = source[i].w;
            visitFlow([&](uint32_t donor, uint32_t receiver, double q) {
                next[receiver] += q * concentration[donor];
            });
            transportResidual = 0;
            for (uint32_t i = 0; i < c.w; ++i) {
                transportResidual = std::max(transportResidual,
                                             std::abs(transportDiagonal[i] * concentration[i] - next[i]) /
                                                 std::max(1e-20, double(source[i].w) + transportDiagonal[i]));
                next[i] = transportDiagonal[i] > 0 ? next[i] / transportDiagonal[i] : 0;
            }
            concentration.swap(next);
            if (transportResidual <= 2e-10)
                break;
        }
        double componentExcess = 0, componentFraction = 0;
        for (uint32_t i = 0; i < c.w; ++i)
            if (component(i) == rootCell) {
                componentExcess += std::max(0., double(cap[i].x) * (concentration[i] - 1));
                componentFraction = std::max(componentFraction, concentration[i]);
                if (componentCells <= 8) {
                    const auto at = coord(i, c);
                    message << "; member [" << at[0] << ',' << at[1] << ',' << at[2] << "] old fraction "
                            << (cap[i].y > 0 ? source[i].w / cap[i].y : 0) << " next fraction "
                            << concentration[i];
                }
            }
        message << "; unextended transport residual " << transportResidual << ", component excess "
                << componentExcess << ", component fraction " << componentFraction;
        uint32_t shown = 0;
        for (uint32_t id = 0; id < c.w && shown < 4; id++) {
            if (diagonal[id] != 0 || ap[id] >= -config.parameters.y * std::max(1e-20, scale[id]))
                continue;
            const auto at = coord(id, c);
            uint32_t types[3]{};
            for (uint32_t child = 0; child < 8; child++) {
                Coord pchild{at[0] * 2 + (child & 1), at[1] * 2 + ((child >> 1) & 1),
                             at[2] * 2 + ((child >> 2) & 1)};
                if (pchild[0] < f.x && pchild[1] < f.y && pchild[2] < f.z)
                    ++types[std::min(2u, uint32_t(cells[index(pchild, f)].z))];
            }
            message << "; isolated [" << at[0] << ',' << at[1] << ',' << at[2] << "] q " << source[id].w
                    << " cap " << cap[id].x << " old " << cap[id].y << " phase residual " << ap[id]
                    << " air/liquid/solid " << types[0] << '/' << types[1] << '/' << types[2];
            ++shown;
        }
    }
    snapshot.resource->Unmap(0, &written);
    if (!message.str().empty())
        throw std::runtime_error(message.str());
    validated = true;
    ++auditedSubsteps;
}
void FluidCarrierProjection::report(std::ostream &out) const {
    out << "{\"method\":\""
        << (coupled ? "coupled capacity/mixed-MAC projection"
                    : "fraction/capacity air-only carrier extension")
        << "\",\"steps\":" << steps << ",\"mixedPressureCoupled\":" << (coupled ? "true" : "false")
        << ",\"phaseDeficitTolerance\":" << (coupled ? 1e-12 : double(constants.parameters.y))
        << ",\"auditedPhaseDeficit\":" << auditedPhaseDeficit
        << ",\"auditedPhaseResidual\":" << auditedPhaseResidual
        << ",\"pressureCyclesPerIteration\":" << (coupled ? capacityPressureCycles : 0)
        << ",\"maxOuterIterations\":" << maxIterations
        << ",\"recordedMultigridCycles\":" << (coupled ? steps * maxIterations * capacityPressureCycles : 0)
        << ",\"multigridCycles\":" << (coupled ? iterations * capacityPressureCycles : 0)
        << ",\"mixedCorrectionDivergence\":" << mixedDivergence
        << ",\"appliedCorrectionDivergence\":" << appliedDivergence
        << ",\"originalPhysicalDivergence\":" << originalPhysicalDivergence
        << ",\"proposedPhysicalDivergence\":" << proposedPhysicalDivergence
        << ",\"appliedPhysicalDivergence\":" << appliedPhysicalDivergence
        << ",\"appliedVelocityError\":" << appliedVelocityError << ",\"totalSubsteps\":" << totalSubsteps
        << ",\"lastFrameCalls\":" << calls << ",\"iterations\":" << iterations
        << ",\"peakFrameIterations\":" << stats[10] << ",\"invalid\":" << stats[5]
        << ",\"exhaustedSteps\":" << stats[6] << ",\"isolatedRows\":" << stats[15]
        << ",\"peakResidual\":" << peakResidual << ",\"tolerance\":" << constants.parameters.y
        << ",\"eligibleFaceUpdates\":" << eligibleFaces << ",\"changedFaceUpdates\":" << changedFaces
        << ",\"lastFrameInitialDeficitM3\":" << metric(stats[11])
        << ",\"weightReferenceError\":" << weightError << ",\"matrixReferenceError\":" << matrixError
        << ",\"extensionReferenceError\":" << extensionError << ",\"auditedFrames\":" << auditedFrames
        << ",\"auditedSubsteps\":" << auditedSubsteps << ",\"auditedIdleFrames\":" << auditedIdleFrames
        << ",\"lastFrameMs\":" << gpuMs << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), frames)
        << ",\"allocatedGpuBufferBytes\":" << bytes << ",\"validated\":" << (validated ? "true" : "false")
        << "}";
}
} // namespace lab
