#include "fluid_pressure.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidPressure::FluidPressure(ID3D12Device *device, const std::filesystem::path &folder, XMUINT4 dimensions,
                             FluidPressureMode m, uint32_t sweeps, uint32_t vcycles)
    : mode(m), iterations(sweeps), cycles(vcycles), grid(dimensions) {
    if ((mode != FluidPressureMode::Active && mode != FluidPressureMode::Multigrid) || !iterations ||
        iterations > 1000 || !cycles || cycles > 16 || !grid.x || !grid.y || !grid.z || grid.x > 1048576 ||
        grid.y > 1048576 || grid.z > 1048576 || grid.w > 1048576 ||
        uint64_t(grid.x) * grid.y * grid.z != grid.w)
        throw std::runtime_error("Invalid active pressure configuration");
    coarseGrid = {(grid.x + 1) / 2, (grid.y + 1) / 2, (grid.z + 1) / 2, 0};
    coarseGrid.w = coarseGrid.x * coarseGrid.y * coarseGrid.z;
    // Two exact Jacobi sweeps per 128-cell tile, with a one-step halo.
    tiles = {(grid.x + 7) / 8, (grid.y + 3) / 4, (grid.z + 3) / 4, 0};
    tiles.w = tiles.x * tiles.y * tiles.z;
    if (tiles.w > 65535)
        throw std::runtime_error("Pressure tile schedule exceeds the initial one-dimensional dispatch limit");
    auto make = [&](uint64_t size, const wchar_t *name) {
        allocatedBytes += std::max(uint64_t(256), size);
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    list =
        make(uint64_t(grid.w + coarseGrid.w + tiles.w) * 4, L"Pressure / compact fine, coarse and tile work");
    tileFlags = make(uint64_t(tiles.w) * 4, L"Pressure / occupied fine tiles");
    counters = make(256, L"Pressure / active counts and residual diagnostics");
    arguments = make(256, L"Pressure / indirect fine and coarse dispatch");
    uint64_t coarseCapacity = mode == FluidPressureMode::Multigrid ? coarseGrid.w : 0;
    coarseA = make(coarseCapacity * 32, L"Pressure / Galerkin coarse operator");
    coarseRhs = make(coarseCapacity * 4, L"Pressure / restricted residual");
    error[0] = make(coarseCapacity * 4, L"Pressure / coarse correction ping");
    error[1] = make(coarseCapacity * 4, L"Pressure / coarse correction pong");
    uniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Pressure / hierarchy constants");
    Uniforms c{grid, coarseGrid, tiles, {mode == FluidPressureMode::Active ? 1.f : 2.f / 3, 0, 0, 0}};
    static_assert(sizeof(Uniforms) == 64);
    memcpy(uniforms.mapped, &c, sizeof(c));
    readback = gpu::buffer(device, 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Pressure / frame timing and residual readback");
    D3D12_ROOT_PARAMETER params[12]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 1; i <= 11; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC r{12, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, errorBlob;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errorBlob),
               "Pressure root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Pressure root");
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &pso) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(name) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC p{};
        p.pRootSignature = root.Get();
        p.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&p, IID_PPV_ARGS(&pso)), name);
    };
    compute("PressureClear", clear);
    compute("PressureActive", active);
    compute("PressureAssemble", assemble);
    compute("PressurePrepare", prepare);
    compute("PressureSmooth", smooth);
    compute("PressureTileSmooth", tileSmooth);
    compute("PressureRestrict", restrictResidual);
    compute("PressureCoarseSmooth", coarseSmooth);
    compute("PressureProlongate", prolongate);
    compute("PressureResidual", residual);
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &arg, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch)),
               "Pressure dispatch signature");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 32;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Pressure substep timestamps");
}
uint32_t FluidPressure::solve(ID3D12GraphicsCommandList *cmd, ID3D12Resource *stencil, ID3D12Resource *ping,
                              ID3D12Resource *pong) {
    if (solves >= 16)
        throw std::runtime_error("Pressure exceeded substep timestamp capacity");
    gpu::Event event(cmd, mode == FluidPressureMode::Multigrid ? L"Fluid / two-level Galerkin pressure"
                                                               : L"Fluid / active-cell pressure");
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, solves * 2);
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    ID3D12Resource *resources[] = {ping,
                                   pong,
                                   stencil,
                                   list.resource.Get(),
                                   counters.resource.Get(),
                                   arguments.resource.Get(),
                                   coarseA.resource.Get(),
                                   coarseRhs.resource.Get(),
                                   error[0].resource.Get(),
                                   error[1].resource.Get(),
                                   tileFlags.resource.Get()};
    for (int i = 0; i < 11; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 1, resources[i]->GetGPUVirtualAddress());
    auto direct = [&](ID3D12PipelineState *p, uint32_t groups) {
        cmd->SetPipelineState(p);
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    direct(clear.Get(), (std::max(8u, tiles.w) + 127) / 128);
    direct(active.Get(), (grid.w + 127) / 128);
    if (mode == FluidPressureMode::Multigrid)
        direct(assemble.Get(), (coarseGrid.w + 127) / 128);
    direct(prepare.Get(), 1);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto indirect = [&](ID3D12PipelineState *p, uint64_t offset) {
        cmd->SetPipelineState(p);
        cmd->ExecuteIndirect(dispatch.Get(), 1, arguments.resource.Get(), offset, nullptr, 0);
        gpu::uav(cmd);
    };
    ID3D12Resource *fine[] = {ping, pong};
    uint32_t current = 0;
    auto bindFine = [&]() {
        cmd->SetComputeRootUnorderedAccessView(1, fine[current]->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2, fine[1 - current]->GetGPUVirtualAddress());
    };
    auto relax = [&](uint32_t n) {
        for (uint32_t i = 0; i < n / 2; ++i) {
            bindFine();
            indirect(tileSmooth.Get(), 24);
            current = 1 - current;
        }
        if (n & 1) {
            bindFine();
            indirect(smooth.Get(), 0);
            current = 1 - current;
        }
        bindFine();
    };
    if (mode == FluidPressureMode::Active)
        relax(iterations);
    else
        for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
            relax(4);
            cmd->SetComputeRootUnorderedAccessView(9, error[0].resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(10, error[1].resource->GetGPUVirtualAddress());
            indirect(restrictResidual.Get(), 12);
            uint32_t coarse = 0;
            for (uint32_t j = 0; j < 24; ++j) {
                cmd->SetComputeRootUnorderedAccessView(9, error[coarse].resource->GetGPUVirtualAddress());
                cmd->SetComputeRootUnorderedAccessView(10,
                                                       error[1 - coarse].resource->GetGPUVirtualAddress());
                indirect(coarseSmooth.Get(), 12);
                coarse = 1 - coarse;
            }
            cmd->SetComputeRootUnorderedAccessView(9, error[coarse].resource->GetGPUVirtualAddress());
            indirect(prolongate.Get(), 0);
            relax(4);
        }
    // Constant aggregation cannot represent every two-cell-thick free-surface
    // mode. Finish on the fine operator; do not accept a coarse-only residual.
    if (mode == FluidPressureMode::Multigrid)
        relax(48);
    indirect(residual.Get(), 0);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, solves * 2 + 1);
    ++solves;
    if (validateFrame) {
        const uint64_t coarseBytes = mode == FluidPressureMode::Multigrid ? uint64_t(coarseGrid.w) * 32 : 0;
        if (!snapshot.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(ping->GetDevice(IID_PPV_ARGS(&device)), "Pressure validation device");
            snapshot = gpu::buffer(
                device.Get(), uint64_t(grid.w) * 20 + coarseBytes + list.resource->GetDesc().Width + 36,
                D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                L"Pressure / opt-in operator snapshot before density scratch reuse");
        }
        uint64_t offset = 0;
        auto copy = [&](ID3D12Resource *src, uint64_t bytes) {
            if (!bytes)
                return;
            gpu::transition(cmd, src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(snapshot.resource.Get(), offset, src, 0, bytes);
            offset += bytes;
            gpu::transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        copy(stencil, uint64_t(grid.w) * 16);
        copy(fine[current], uint64_t(grid.w) * 4);
        copy(coarseA.resource.Get(), coarseBytes);
        copy(list.resource.Get(), list.resource->GetDesc().Width);
        copy(arguments.resource.Get(), 36);
    }
    return current;
}
void FluidPressure::recordReadback(ID3D12GraphicsCommandList *cmd) {
    if (!solves)
        return;
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, solves * 2, readback.resource.Get(),
                          0);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 256, counters.resource.Get(), 0, sizeof(metrics));
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void FluidPressure::collect(uint64_t frequency) {
    gpuMs = 0;
    ++measuredFrames;
    if (!solves)
        return;
    void *data;
    D3D12_RANGE range{0, 288}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Pressure metrics map");
    const auto ticks = static_cast<const uint64_t *>(data);
    for (uint32_t i = 0; i < solves; ++i)
        gpuMs += double(ticks[2 * i + 1] - ticks[2 * i]) * 1000 / frequency;
    memcpy(metrics.data(), ticks + 32, sizeof(metrics));
    readback.resource->Unmap(0, &written);
    totalMs += gpuMs;
    if (metrics[0] > grid.w || metrics[1] > coarseGrid.w || metrics[5] > tiles.w || metrics[2])
        throw std::runtime_error("Invalid/nonfinite active pressure solve");
    if (validateFrame)
        validateSnapshot();
}
void FluidPressure::validateSnapshot() {
    void *data;
    D3D12_RANGE range{0, SIZE_T(snapshot.resource->GetDesc().Width)}, written{0, 0};
    gpu::check(snapshot.resource->Map(0, &range, &data), "Pressure invariant snapshot map");
    const auto stencil = static_cast<const XMFLOAT4 *>(data);
    const auto pressure = reinterpret_cast<const float *>(stencil + grid.w);
    const auto coarse = pressure + grid.w;
    const auto ids = reinterpret_cast<const uint32_t *>(
        coarse + (mode == FluidPressureMode::Multigrid ? coarseGrid.w * 8 : 0));
    const auto args = ids + list.resource->GetDesc().Width / 4;
    bool valid = true;
    std::vector<uint8_t> seen(grid.w, 0), coarseSeen(coarseGrid.w, 0), tilesSeen(tiles.w, 0),
        expectedTiles(tiles.w, 0);
    for (uint32_t i = 0; i < metrics[0]; ++i) {
        uint32_t id = ids[i];
        if (id >= grid.w) {
            valid = false;
            continue;
        }
        valid &= !seen[id] && stencil[id].y > 0;
        seen[id] = 1;
    }
    for (uint32_t i = 0; i < metrics[1]; ++i) {
        uint32_t id = ids[grid.w + i];
        if (id >= coarseGrid.w) {
            valid = false;
            continue;
        }
        valid &= !coarseSeen[id];
        coarseSeen[id] = 1;
    }
    for (uint32_t i = 0; i < metrics[5]; ++i) {
        uint32_t id = ids[grid.w + coarseGrid.w + i];
        if (id >= tiles.w) {
            valid = false;
            continue;
        }
        valid &= !tilesSeen[id];
        tilesSeen[id] = 1;
    }
    std::vector<std::array<float, 8>> expected(mode == FluidPressureMode::Multigrid ? coarseGrid.w : 0);
    double maxResidual = 0;
    for (uint32_t id = 0; id < grid.w; ++id) {
        valid &= std::isfinite(pressure[id]) && (seen[id] != 0) == (stencil[id].y > 0);
        if (!seen[id]) {
            valid &= pressure[id] == 0;
            continue;
        }
        const auto s = stencil[id];
        uint32_t xyz[] = {id % grid.x, (id / grid.x) % grid.y, id / (grid.x * grid.y)};
        expectedTiles[((xyz[2] / 4) * tiles.y + xyz[1] / 4) * tiles.x + xyz[0] / 8] = 1;
        uint32_t parent = ((xyz[2] / 2) * coarseGrid.y + xyz[1] / 2) * coarseGrid.x + xyz[0] / 2;
        uint32_t strides[] = {1, grid.x, grid.x * grid.y};
        double r = s.x - double(pressure[id]) / s.y;
        if (!expected.empty()) {
            expected[parent][3] += std::round(1.f / s.y);
            expected[parent][7]++;
        }
        for (uint32_t a = 0; a < 3; ++a)
            for (uint32_t side = 0; side < 2; ++side) {
                if (!(uint32_t(s.z) & (1u << (a * 2 + side))))
                    continue;
                const uint32_t n = side ? id + strides[a] : id - strides[a];
                if (n >= grid.w || !seen[n]) {
                    valid = false;
                    continue;
                }
                r += pressure[n];
                if (!expected.empty()) {
                    uint32_t nAxis = side ? xyz[a] + 1 : xyz[a] - 1;
                    if (nAxis / 2 == xyz[a] / 2)
                        expected[parent][3]--;
                    else
                        expected[parent][a + (side ? 4 : 0)]++;
                }
            }
        maxResidual = std::max(maxResidual, std::abs(r));
    }
    for (uint32_t id = 0; id < expected.size(); ++id) {
        for (int j = 0; j < 8; ++j)
            valid &= expected[id][j] == coarse[id * 8 + j];
        valid &= (coarseSeen[id] != 0) == (expected[id][7] > 0);
    }
    float maxRhs, gpuResidual;
    memcpy(&maxRhs, &metrics[3], 4);
    memcpy(&gpuResidual, &metrics[4], 4);
    valid &= std::abs(maxResidual - gpuResidual) < std::max(.002, double(maxRhs) * .0001);
    valid &= args[0] == (metrics[0] + 127) / 128 && args[1] == 1 && args[2] == 1 &&
             args[3] == (metrics[1] + 127) / 128 && args[4] == 1 && args[5] == 1;
    valid &= tilesSeen == expectedTiles && args[6] == metrics[5] && args[7] == 1 && args[8] == 1;
    snapshot.resource->Unmap(0, &written);
    if (!valid)
        throw std::runtime_error("Pressure operator/partition/residual/indirect validation failed");
    validated = true;
}
void FluidPressure::report(std::ostream &out) const {
    float rhs, residualValue;
    memcpy(&rhs, &metrics[3], 4);
    memcpy(&residualValue, &metrics[4], 4);
    out << "{\"mode\":\"" << (mode == FluidPressureMode::Active ? "active-jacobi" : "two-level-Galerkin")
        << "\",\"fineCells\":" << grid.w << ",\"activeFineCells\":" << metrics[0]
        << ",\"coarseCells\":" << (mode == FluidPressureMode::Multigrid ? coarseGrid.w : 0)
        << ",\"activeCoarseCells\":" << metrics[1] << ",\"activeFineTiles\":" << metrics[5]
        << ",\"sweepsPerTileDispatch\":2"
        << ",\"fineSweepsPerSolve\":" << (mode == FluidPressureMode::Active ? iterations : cycles * 8 + 48)
        << ",\"coarseSweepsPerSolve\":" << (mode == FluidPressureMode::Multigrid ? cycles * 24 : 0)
        << ",\"lastMaxAbsRhs\":" << rhs << ",\"lastMaxAbsResidual\":" << residualValue
        << ",\"lastFrameMs\":" << gpuMs
        << ",\"meanFrameMs\":" << totalMs / std::max(uint64_t(1), measuredFrames)
        << ",\"allocatedGpuBufferBytes\":" << allocatedBytes
        << ",\"validated\":" << (validated ? "true" : "false") << "}";
}
} // namespace lab
