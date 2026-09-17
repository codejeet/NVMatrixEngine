#include "fluid_complexity.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidComplexity::FluidComplexity(ID3D12Device *device, const std::filesystem::path &folder,
                                 const FluidSurface &surface, const FluidComplexityDesc &d)
    : desc(d), minimumSpacing(surface.minimumSpacing), bricks(surface.brickGrid) {
    for (float value : {d.responseSeconds, d.vorticityScale, d.gradientScale, d.accelerationScale})
        if (!std::isfinite(value) || value <= 0)
            throw std::runtime_error("Complexity time/normalization scales must be finite and positive");
    for (float value : {d.wakeSeconds, d.leadSeconds, d.solidMargin})
        if (!std::isfinite(value) || value < 0)
            throw std::runtime_error("Complexity wake bounds must be finite and nonnegative");
    for (int i = 0; i < 3; ++i) {
        const float p = (&d.promote.x)[i], q = (&d.demote.x)[i];
        if (!std::isfinite(p) || !std::isfinite(q) || q < 0 || p > 1 || q >= p ||
            (i && (p >= (&d.promote.x)[i - 1] || q >= (&d.demote.x)[i - 1])))
            throw std::runtime_error("Complexity LOD thresholds must be ordered with a hysteresis gap");
    }
    auto make = [&](uint64_t size, const wchar_t *name) {
        allocatedBytes += std::max(size, uint64_t(256));
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    raw = make(uint64_t(bricks.w) * 64, L"Complexity / raw immutable decisions");
    state = make(uint64_t(bricks.w) * 64, L"Complexity / persistent importance and requested LODs");
    lists = make(uint64_t(bricks.w) * 32, L"Complexity / physics and surface LOD partitions");
    active = make(uint64_t(bricks.w) * 4, L"Complexity / compact active union");
    counters = make(256, L"Complexity / partition metrics");
    arguments = make(256, L"Complexity / indirect per-LOD dispatch and debug draw");
    uniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Complexity / constants");
    readback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Complexity / small asynchronous metrics");
    // Allocated once; copying full decisions is restricted to explicit bounded validation.
    validation = gpu::buffer(device, uint64_t(bricks.w) * 100 + 256, D3D12_HEAP_TYPE_READBACK,
                             D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                             L"Complexity / opt-in invariant validation");
    allocatedBytes += 512 + uint64_t(bricks.w) * 100 + 256;
    D3D12_ROOT_PARAMETER p[14]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 1; i <= 9; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    for (int i = 10; i <= 11; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[i].Descriptor.ShaderRegister = i - 10;
    }
    p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[12].Descriptor.ShaderRegister = 9;
    p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[13].Descriptor.ShaderRegister = 10;
    D3D12_ROOT_SIGNATURE_DESC r{14, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Complexity root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Complexity root");
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(name) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&out)), name);
    };
    compute("ComplexityClear", clear);
    compute("ComplexityClassify", classify);
    compute("ComplexitySchedule", schedule);
    compute("ComplexityPrepare", prepare);
    auto vs = gpu::bytes(folder / "shaders/ComplexityVS.dxil"),
         ps = gpu::bytes(folder / "shaders/ComplexityPS.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
    g.pRootSignature = root.Get();
    g.VS = {vs.data(), vs.size()};
    g.PS = {ps.data(), ps.size()};
    g.SampleMask = UINT_MAX;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable = TRUE;
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    g.SampleDesc.Count = 1;
    gpu::check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&debug)), "Complexity debug PSO");
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC signature{16, 1, &arg, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&draw)),
               "Complexity draw signature");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 3;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Complexity timestamps");
}
void FluidComplexity::record(ID3D12GraphicsCommandList *cmd, const FluidSystem &fluid,
                             const FluidSurface &surface, const Camera &camera, float dt) {
    gpu::Event event(cmd, L"Fluid / adaptive importance (requested LOD policy)");
    const auto view = fluid.gpuView();
    const auto &system = fluid.description();
    Uniforms c{};
    static_assert(sizeof(c) == 256);
    c.viewProjection = camera.viewProjection;
    c.minimumSpacing = minimumSpacing;
    c.simulationMinimumCell = {system.minimum.x, system.minimum.y, system.minimum.z, system.gridCellSize};
    c.bricks = bricks;
    c.grid = view.grid;
    c.cameraPosition = {camera.position.x, camera.position.y, camera.position.z, 0};
    c.cameraForward = {camera.forward.x, camera.forward.y, camera.forward.z, 0};
    c.timing = {std::clamp(dt, 0.f, .25f), desc.responseSeconds, recorded ? 1.f : 0.f,
                fluid.resetThisFrame ? 1.f : 0.f};
    c.scales = {desc.vorticityScale, desc.gradientScale, desc.accelerationScale, desc.solidMargin};
    c.promote = {desc.promote.x, desc.promote.y, desc.promote.z, 0};
    c.demote = {desc.demote.x, desc.demote.y, desc.demote.z, 0};
    c.wake = {desc.wakeSeconds, desc.leadSeconds, 0, 0};
    particleConsumer = system.adaptiveParticles;
    macConsumer = system.adaptiveMac;
    surfaceConsumer = surface.adaptive;
    c.control = {view.colliderCount, debugMode, system.surfaceTension > 0 ? 1u : 0u,
                 (system.adaptiveParticles ? 1u : 0u) | (system.coarseInterior ? 2u : 0u) |
                     (system.ownedParticles ? 8u : 0u) |
                     (opticalFeedback && opticalFeedbackReady && !fluid.resetThisFrame ? 4u : 0u)};
    memcpy(uniforms.mapped, &c, sizeof(c));
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    bool update = !recorded || !frozen || fluid.resetThisFrame;
    if (update) {
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
        ID3D12Resource *buffers[] = {view.faces,
                                     view.offsets,
                                     view.material,
                                     raw.resource.Get(),
                                     state.resource.Get(),
                                     lists.resource.Get(),
                                     active.resource.Get(),
                                     counters.resource.Get(),
                                     arguments.resource.Get()};
        for (int i = 0; i < 9; ++i)
            cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(10, surface.mapResource()->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(11, view.colliderAddress);
        cmd->SetComputeRootUnorderedAccessView(12, view.cellQuanta->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(
            13, (opticalFeedback ? opticalFeedback : state.resource.Get())->GetGPUVirtualAddress());
        cmd->SetPipelineState(clear.Get());
        cmd->Dispatch(1, 1, 1);
        gpu::uav(cmd, counters.resource.Get());
        cmd->SetPipelineState(classify.Get());
        cmd->Dispatch(bricks.w, 1, 1);
        gpu::uav(cmd);
        ++classifications;
    } else
        ++frozenFrames;
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    if (update) {
        gpu::Event scheduling(cmd, L"Fluid / refinement padding and GPU LOD compaction");
        cmd->SetPipelineState(schedule.Get());
        cmd->Dispatch((bricks.w + 63) / 64, 1, 1);
        gpu::uav(cmd);
        cmd->SetPipelineState(prepare.Get());
        cmd->Dispatch(1, 1, 1);
        gpu::uav(cmd);
    }
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    recorded = true;
}
void FluidComplexity::drawDebug(ID3D12GraphicsCommandList *cmd) {
    if (!debugMode)
        return;
    gpu::Event event(cmd, L"Fluid / requested LOD inspection (x-ray wire bricks)");
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetGraphicsRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(5, state.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(7, active.resource->GetGPUVirtualAddress());
    cmd->SetPipelineState(debug.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cmd->ExecuteIndirect(draw.Get(), 1, arguments.resource.Get(), 96, nullptr, 0);
    gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}
void FluidComplexity::recordReadback(ID3D12GraphicsCommandList *cmd, bool validate) {
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, readback.resource.Get(), 0);
    auto copy = [&](gpu::Buffer &src, gpu::Buffer &dst, uint64_t offset, uint64_t bytes) {
        gpu::transition(cmd, src.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(dst.resource.Get(), offset, src.resource.Get(), 0, bytes);
        gpu::transition(cmd, src.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    copy(counters, readback, 24, sizeof(counts));
    validatePending = validate;
    if (validate) {
        copy(state, validation, 0, uint64_t(bricks.w) * 64);
        copy(lists, validation, uint64_t(bricks.w) * 64, uint64_t(bricks.w) * 32);
        copy(active, validation, uint64_t(bricks.w) * 96, uint64_t(bricks.w) * 4);
        copy(arguments, validation, uint64_t(bricks.w) * 100, 112);
    }
}
void FluidComplexity::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 24 + sizeof(counts)}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Complexity metrics map");
    auto ticks = static_cast<const uint64_t *>(data);
    classificationMs = double(ticks[1] - ticks[0]) * 1000 / frequency;
    schedulingMs = double(ticks[2] - ticks[1]) * 1000 / frequency;
    memcpy(counts.data(), ticks + 3, sizeof(counts));
    readback.resource->Unmap(0, &written);
    ++measurements;
    totalClassificationMs += classificationMs;
    totalSchedulingMs += schedulingMs;
    if (counts[0] > bricks.w || counts[16] ||
        std::any_of(counts.begin() + 1, counts.begin() + 9,
                    [&](uint32_t count) { return count > bricks.w; }) ||
        std::accumulate(counts.begin() + 1, counts.begin() + 5, 0u) != counts[0] ||
        std::accumulate(counts.begin() + 5, counts.begin() + 9, 0u) != counts[0])
        throw std::runtime_error("Complexity nonfinite metric or invalid LOD partition counts");
    if (!validatePending)
        return;
    range = {0, SIZE_T(bricks.w) * 100 + 112};
    gpu::check(validation.resource->Map(0, &range, &data), "Complexity validation map");
    const auto states = static_cast<const ComplexityBrick *>(data);
    const auto ids = reinterpret_cast<const uint32_t *>(states + bricks.w);
    const auto activeIds = ids + bricks.w * 8;
    const auto args = activeIds + bricks.w;
    bool valid = true;
    std::vector<uint32_t> membership(bricks.w, 0);
    uint32_t totalParticles = 0, surfaceCount = 0, feedbackCount = 0, feedbackRaised = 0;
    for (uint32_t i = 0; i < bricks.w; ++i) {
        const auto &b = states[i];
        for (int j = 0; j < 12; ++j)
            valid &= std::isfinite(reinterpret_cast<const float *>(&b)[j]);
        valid &= b.state.x < 4 && b.state.y < 4;
        if (b.state.z & 2)
            valid &= b.state.x == 0;
        totalParticles += b.state.w;
        surfaceCount += (b.state.z & 2) ? 1u : 0u;
        feedbackCount += (b.state.z & 32) ? 1u : 0u;
        feedbackRaised += (b.state.z & 64) ? 1u : 0u;
    }
    for (uint32_t partition = 0; partition < 8; ++partition) {
        valid &= args[partition * 3] == counts[partition + 1] && args[partition * 3 + 1] == 1 &&
                 args[partition * 3 + 2] == 1;
        for (uint32_t i = 0; i < counts[partition + 1]; ++i) {
            uint32_t id = ids[partition * bricks.w + i];
            if (id >= bricks.w) {
                valid = false;
                continue;
            }
            uint32_t bit = partition < 4 ? 1u : 2u;
            valid &= !(membership[id] & bit);
            membership[id] |= bit;
            valid &= (partition < 4 ? states[id].state.x : states[id].state.y) == partition % 4;
        }
    }
    for (uint32_t i = 0; i < counts[0]; ++i) {
        uint32_t id = activeIds[i];
        if (id >= bricks.w) {
            valid = false;
            continue;
        }
        valid &= membership[id] == 3;
        membership[id] |= 4;
    }
    for (uint32_t i = 0; i < bricks.w; ++i)
        valid &= membership[i] == ((states[i].state.z & 31u) ? 7u : 0u);
    valid &= feedbackCount == counts[19] && feedbackRaised == counts[20];
    valid &= totalParticles == std::accumulate(counts.begin() + 9, counts.begin() + 13, 0u) &&
             surfaceCount == counts[13] && args[24] == 24 && args[25] == counts[0] && !args[26] && !args[27];
    validation.resource->Unmap(0, &written);
    if (!valid)
        throw std::runtime_error(
            "Complexity GPU coverage/uniqueness/LOD/indirect argument validation failed");
    validated = true;
}
FluidComplexityGpuView FluidComplexity::gpuView() const {
    return {state.resource.Get(),     lists.resource.Get(), active.resource.Get(),
            arguments.resource.Get(), minimumSpacing,       bricks};
}
const char *FluidComplexity::viewName() const {
    static const char *names[] = {"off",
                                  "physics",
                                  "surface",
                                  "optical prior",
                                  "visibility",
                                  "temporal change",
                                  "requested simulation LOD",
                                  "requested surface LOD",
                                  "surface band / padding",
                                  "moving solid / swept wake",
                                  "vorticity"};
    return names[std::min(debugMode, 10u)];
}
void FluidComplexity::report(std::ostream &out) const {
    out << "{\"mode\":\""
        << (macConsumer ? (particleConsumer ? "adaptive MAC and particle samples" : "adaptive MAC")
                        : (particleConsumer ? "adaptive particle samples; uniform MAC" : "uniform solver"))
        << (surfaceConsumer ? "; error-controlled surface sampling" : "; uniform surface")
        << "\",\"brickCapacity\":" << bricks.w << ",\"activeBricks\":" << counts[0]
        << ",\"physicsLodBricks\":[";
    for (int i = 1; i <= 4; ++i)
        out << (i == 1 ? "" : ",") << counts[i];
    out << "],\"surfaceLodBricks\":[";
    for (int i = 5; i <= 8; ++i)
        out << (i == 5 ? "" : ",") << counts[i];
    out << "],\"particlesByRequestedLod\":[";
    for (int i = 9; i <= 12; ++i)
        out << (i == 9 ? "" : ",") << counts[i];
    out << "],\"surfaceBricks\":" << counts[13] << ",\"movingSolidBricks\":" << counts[14]
        << ",\"paddedBricks\":" << counts[15] << ",\"invalidMetrics\":" << counts[16]
        << ",\"promotions\":" << counts[17] << ",\"demotions\":" << counts[18]
        << ",\"opticalFeedbackBricks\":" << counts[19] << ",\"opticalRaisedBricks\":" << counts[20]
        << ",\"classifications\":" << classifications << ",\"frozenFrames\":" << frozenFrames
        << ",\"classificationMeanMs\":" << totalClassificationMs / std::max(uint64_t(1), measurements)
        << ",\"schedulingMeanMs\":" << totalSchedulingMs / std::max(uint64_t(1), measurements)
        << ",\"allocatedBytes\":" << allocatedBytes << ",\"validated\":" << (validated ? "true" : "false")
        << ",\"view\":\"" << viewName() << "\",\"frozen\":" << (frozen ? "true" : "false") << "}";
}
} // namespace lab
