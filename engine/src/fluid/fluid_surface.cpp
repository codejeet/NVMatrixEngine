#include "fluid_surface.h"
#include "fluid_complexity.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidSurface::FluidSurface(ID3D12Device5 *device, const std::filesystem::path &folder,
                           const FluidSystemDesc &desc)
    : narrowBand(desc.narrowBand), shaderFolder(folder / "shaders") {
    const float spacing = desc.gridCellSize * .5f, padding = desc.gridCellSize * 2;
    for (uint32_t a = 0; a < 3; ++a)
        (&expectedSimulationGrid.x)[a] =
            uint32_t(std::ceil(((&desc.maximum.x)[a] - (&desc.minimum.x)[a]) / desc.gridCellSize));
    minimumSpacing = {desc.minimum.x - padding, desc.minimum.y - padding, desc.minimum.z - padding, spacing};
    for (int a = 0; a < 3; ++a)
        (&brickGrid.x)[a] =
            uint32_t(std::ceil(((&desc.maximum.x)[a] - (&desc.minimum.x)[a] + 2 * padding) / (8 * spacing)));
    const uint64_t capacity = uint64_t(brickGrid.x) * brickGrid.y * brickGrid.z;
    // One-dimensional indirect dispatch has a 65535 group ceiling, six groups/brick.
    if (!capacity || capacity > 10000)
        throw std::runtime_error("Fluid surface exceeds initial 10000-brick budget");
    brickGrid.w = uint32_t(capacity);
    auto make = [&](uint64_t bytes, const wchar_t *name) {
        allocatedBytes += bytes;
        return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    field = make(capacity * 729 * 16, L"Fluid sparse phi and material displacement (9 cubed nodes)");
    shapes = make(uint64_t(desc.maxParticles) * 48, L"Fluid covariance-shaped reconstruction kernels");
    // Page table followed by 16 mask uints and packed surface bounds per slot.
    // Conservative zero-crossing masks skip field loads for empty/interior cells.
    // Trailing GPU flag distinguishes actual LOD deformation from merely
    // rerunning reconstruction for a camera/importance update.
    map = make((capacity * 18 + 8) * 4, L"Fluid page table, surface bounds, LOD flag and phase domain");
    list = make(capacity * 4, L"Fluid compact active brick IDs");
    aabbs = make(capacity * sizeof(D3D12_RAYTRACING_AABB), L"Fluid GPU procedural surface AABBs");
    counts = make(256, L"Fluid active and surface brick counters");
    arguments = make(256, L"Fluid indirect reconstruction dispatch");
    uniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Fluid reconstruction constants");
    readback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid surface timings and counts");
    D3D12_ROOT_PARAMETER params[25]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    for (int i = 1; i < 11; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i - 1;
    }
    for (int i = 11; i < 13; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[i].Descriptor.ShaderRegister = i - 11;
    }
    params[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[13].Descriptor.ShaderRegister = 10;
    for (uint32_t i = 14; i < 16; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i - 3;
    }
    for (uint32_t i = 16; i < 25; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i - 3;
    }
    D3D12_ROOT_SIGNATURE_DESC r{25, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Fluid surface root serialize");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Fluid surface root");
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
        auto code = gpu::bytes(folder / "shaders" / (std::string("Fluid") + name + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = root.Get();
        d.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)), name);
    };
    compute("SurfaceClear", clear);
    compute(narrowBand ? "SurfaceMarkNarrow" : "SurfaceMark", mark);
    compute("SurfacePrepare", prepare);
    compute(narrowBand ? "SurfaceReconstructNarrow" : "SurfaceReconstruct", reconstruct);
    compute("SurfaceBounds", bounds);
    compute("SurfaceShape", shape);
    const char *lodNames[]{"SurfaceLodBegin",    "SurfaceLodFingerprint", "SurfaceLodPlan",
                           "SurfaceLodFine",     "SurfaceLodCoarse",      "SurfaceLodMasks",
                           "SurfaceLodAnalyze",  "SurfaceLodGuard",       "SurfaceLodRepair",
                           "SurfaceLodFinalize", "SurfaceLodCommit",      "SurfaceLodReference"};
    for (uint32_t i = 0; i < LodPassCount; ++i)
        compute((std::string(lodNames[i]) + (narrowBand ? "Narrow" : "")).c_str(), lodPipelines[i]);
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &arg, 0};
    gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch)),
               "Fluid indirect dispatch signature");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 3;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Fluid surface timestamps");
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.AABBs.AABBCount = capacity;
    geometry.AABBs.AABBs = {aabbs.resource->GetGPUVirtualAddress(), sizeof(D3D12_RAYTRACING_AABB)};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
    input.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    input.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    input.NumDescs = 1;
    input.pGeometryDescs = &geometry;
    input.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&input, &info);
    blas = gpu::buffer(device, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, L"Fluid procedural BLAS");
    scratch = make(info.ScratchDataSizeInBytes, L"Fluid BLAS build scratch");
    allocatedBytes += info.ResultDataMaxSizeInBytes;
}
void FluidSurface::setImportance(const FluidComplexityGpuView &view) {
    if (view.grid.x != brickGrid.x || view.grid.y != brickGrid.y || view.grid.z != brickGrid.z)
        throw std::runtime_error("Surface LOD and importance lattices differ");
    importance = view.state;
}
void FluidSurface::record(ID3D12GraphicsCommandList4 *cmd, const FluidSurfaceInput &system,
                          const Camera &camera, float dt) {
    const bool phaseMode = system.phase != nullptr;
    if (narrowBand != (system.gridOwned != nullptr) ||
        (narrowBand && (phaseMode || !system.description.narrowBand || system.view.interiorEnabled ||
                        !std::isfinite(system.particleVolume) || system.particleVolume <= 0 ||
                        !std::isfinite(system.advancedSeconds) || system.advancedSeconds < 0)))
        throw std::runtime_error("Invalid narrow-band surface ownership inputs");
    if (narrowBand) {
        const auto &v = system.view;
        const uint64_t owners = uint64_t((v.grid.x + 1) / 2) * ((v.grid.y + 1) / 2) * ((v.grid.z + 1) / 2);
        const auto d = system.gridOwned->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || d.Width < owners * 32 ||
            !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
            throw std::runtime_error("Invalid narrow-band grid surface buffer");
    }
    if (phaseMode != (system.planes != nullptr))
        throw std::runtime_error("Fluid surface requires both phase and plane geometry buffers");
    if (phaseMode) {
        const auto &v = system.view;
        const auto &d = system.description;
        // The current publication is a full-capacity Cartesian domain. Reject
        // unsupported carving/particle-only LOD instead of losing owner volume.
        if (adaptive || fixture || v.interiorEnabled || v.colliderCount || !v.faces ||
            !std::isfinite(system.advancedSeconds) || system.advancedSeconds < 0)
            throw std::runtime_error(
                "Phase surface requires uncarved uniform geometry and finite elapsed time");
        const uint64_t owners = uint64_t((v.grid.x + 1) / 2) * ((v.grid.y + 1) / 2) * ((v.grid.z + 1) / 2);
        if (!owners || v.grid.w != uint64_t(v.grid.x) * v.grid.y * v.grid.z ||
            minimumSpacing.w != d.gridCellSize * .5f ||
            minimumSpacing.x != d.minimum.x - d.gridCellSize * 2 ||
            minimumSpacing.y != d.minimum.y - d.gridCellSize * 2 ||
            minimumSpacing.z != d.minimum.z - d.gridCellSize * 2)
            throw std::runtime_error("Phase surface and simulation lattices differ");
        for (uint32_t axis = 0; axis < 3; ++axis)
            if ((&v.grid.x)[axis] != (&expectedSimulationGrid.x)[axis] ||
                uint32_t(std::ceil(((&d.maximum.x)[axis] - (&d.minimum.x)[axis]) / d.gridCellSize)) !=
                    (&v.grid.x)[axis])
                throw std::runtime_error("Phase surface grid dimensions differ");
        ID3D12Resource *inputs[]{system.phase, system.planes, v.faces};
        const uint64_t sizes[]{owners * 16, owners * 32,
                               uint64_t(v.grid.x + 1) * (v.grid.y + 1) * (v.grid.z + 1) * 48};
        for (uint32_t i = 0; i < 3; ++i) {
            const auto resource = inputs[i]->GetDesc();
            if (resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || resource.Width < sizes[i] ||
                !(resource.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                throw std::runtime_error("Invalid phase surface GPU buffer");
            for (uint32_t j = 0; j < i; ++j)
                if (inputs[i] == inputs[j])
                    throw std::runtime_error("Aliased phase surface GPU buffers");
            for (auto *output :
                 {field.resource.Get(), map.resource.Get(), list.resource.Get(), aabbs.resource.Get(),
                  counts.resource.Get(), arguments.resource.Get(), phaseHeights.resource.Get()})
                if (inputs[i] == output)
                    throw std::runtime_error("Phase geometry aliases a reconstruction output");
        }
        if (!phaseMark || !phaseReconstruct || !phaseHeight || !phaseHeights.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(cmd->GetDevice(IID_PPV_ARGS(&device)), "Phase surface device");
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            gpu::check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
                       "Phase surface FP64 capability");
            if (!options.DoublePrecisionFloatShaderOps)
                throw std::runtime_error("Phase surface requires FP64 shader arithmetic");
            auto pipeline = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
                auto code = gpu::bytes(shaderFolder / (std::string(name) + ".dxil"));
                D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
                ps.pRootSignature = root.Get();
                ps.CS = {code.data(), code.size()};
                gpu::check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&out)), name);
            };
            pipeline("FluidSurfacePhaseMark", phaseMark);
            pipeline("FluidSurfacePhaseReconstruct", phaseReconstruct);
            pipeline("FluidSurfacePhaseHeights", phaseHeight);
            if (!phaseHeights.resource) {
                phaseHeights = gpu::buffer(device.Get(), owners * 32, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           L"Fluid phase / bounded local height columns");
                allocatedBytes += owners * 32;
            }
        }
    }
    gpu::Event event(cmd, L"Fluid / continuous surface and procedural BLAS");
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    const bool cameraMoved = camera.position.x != previousCamera.x || camera.position.y != previousCamera.y ||
                             camera.position.z != previousCamera.z;
    const bool updating = !recorded || system.changedThisFrame || system.phase != previousPhase ||
                          system.planes != previousPlanes || (adaptive && (lodNeedsUpdate || cameraMoved)) ||
                          (phaseMode && (system.advancedSeconds > 0 || phaseMotion || system.resetThisFrame));
    changedThisFrame = updating;
    lodSnapshotReady = false;
    if (updating) {
        if (adaptive && !lodState.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(cmd->GetDevice(IID_PPV_ARGS(&device)), "Surface LOD device");
            auto makeLod = [&](uint64_t bytes, const wchar_t *name) {
                allocatedBytes += bytes;
                return gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
            };
            lodState = makeLod(uint64_t(brickGrid.w) * 48, L"Surface / persistent error and LOD state");
            lodNext =
                makeLod(uint64_t(brickGrid.w) * 48, L"Surface / immutable next shared-node constraints");
            lodLists =
                makeLod(uint64_t(brickGrid.w) * 76 + uint64_t(system.view.grid.w) * 4,
                        L"Surface / compact work, cell fingerprints and persistent fine interface masks");
        }
        if (adaptive && validateLodThisFrame && !lodReference.resource) {
            ComPtr<ID3D12Device> device;
            gpu::check(cmd->GetDevice(IID_PPV_ARGS(&device)), "Surface reference device");
            lodReference =
                gpu::buffer(device.Get(), field.resource->GetDesc().Width, D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            L"Surface / opt-in same-state fine reference");
            lodSnapshot = gpu::buffer(device.Get(),
                                      2 * field.resource->GetDesc().Width + map.resource->GetDesc().Width +
                                          lodNext.resource->GetDesc().Width,
                                      D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                      L"Surface / independent LOD error and continuity snapshot");
        }
        if (readable) {
            for (auto r : {field.resource.Get(), map.resource.Get(), aabbs.resource.Get()})
                gpu::transition(cmd, r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const auto view = system.view;
        const auto &desc = system.description;
        Uniforms c{
            minimumSpacing,
            {desc.minimum.x, desc.minimum.y, desc.minimum.z, desc.gridCellSize},
            brickGrid,
            view.grid,
            {desc.gridCellSize * 1.5f, desc.gridCellSize * .5f, float(desc.maxParticles), float(fixture)},
            {view.colliderCount, anisotropic ? 1u : 0u, view.interiorEnabled ? 1u : 0u, lodCoarseAxes}};
        c.lodTolerance = {lodPhiTolerance, lodNormalTolerance, std::clamp(dt * 7.5f, 0.f, 1.f),
                          lodPhiTolerance * .5f};
        c.lodControl = {adaptive ? 1u : 0u, ++updateNumber, (!recorded || system.resetThisFrame) ? 1u : 0u,
                        (importance && recorded ? 1u : 0u) | (validateLodThisFrame ? 2u : 0u) |
                            (system.stepCount != previousSteps ? 4u : 0u) | (forceFine ? 8u : 0u)};
        c.lodCamera = {camera.position.x, camera.position.y, camera.position.z, 8};
        c.phaseControl = {system.resetThisFrame ? 0.f : system.advancedSeconds, phaseMode ? 1.f : 0.f,
                          narrowBand ? 1.f : 0.f, system.particleVolume};
        static_assert(sizeof(Uniforms) <= 256);
        memcpy(uniforms.mapped, &c, sizeof(c));
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
        ID3D12Resource *buffers[] = {view.particles,          view.offsets,         view.indices,
                                     view.previousPositions,  map.resource.Get(),   field.resource.Get(),
                                     list.resource.Get(),     aabbs.resource.Get(), counts.resource.Get(),
                                     arguments.resource.Get()};
        for (int i = 0; i < 10; ++i)
            cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(11, view.colliderAddress);
        cmd->SetComputeRootShaderResourceView(12, view.meshPhi->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(13, shapes.resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(14, view.interior->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(15, view.interiorTotals->GetGPUVirtualAddress());
        ID3D12Resource *lodBuffers[]{lodState.resource.Get(), lodNext.resource.Get(), importance,
                                     lodLists.resource.Get(), lodReference.resource.Get()};
        for (uint32_t i = 0; i < 5; ++i)
            cmd->SetComputeRootUnorderedAccessView(
                16 + i, (lodBuffers[i] ? lodBuffers[i] : field.resource.Get())->GetGPUVirtualAddress());
        ID3D12Resource *phaseBuffers[]{narrowBand ? system.gridOwned : system.phase, system.planes,
                                       phaseMode ? view.faces : nullptr,
                                       phaseMode ? phaseHeights.resource.Get() : nullptr};
        for (uint32_t i = 0; i < 4; ++i)
            cmd->SetComputeRootUnorderedAccessView(
                21 + i, (phaseBuffers[i] ? phaseBuffers[i] : field.resource.Get())->GetGPUVirtualAddress());
        auto pass = [&](ID3D12PipelineState *p, uint32_t groups) {
            cmd->SetPipelineState(p);
            cmd->Dispatch(groups, 1, 1);
            gpu::uav(cmd);
        };
        pass(clear.Get(), (brickGrid.w + 127) / 128);
        if (phaseMode) {
            gpu::Event heightEvent(cmd, L"Fluid phase / local height-function columns");
            const uint32_t owners =
                ((view.grid.x + 1) / 2) * ((view.grid.y + 1) / 2) * ((view.grid.z + 1) / 2);
            pass(phaseHeight.Get(), (owners + 127) / 128);
        }
        if (adaptive)
            pass(lodPipelines[LodBegin].Get(), (brickGrid.w + 127) / 128);
        if (!fixture && !phaseMode)
            pass(shape.Get(), (desc.maxParticles + 127) / 128);
        if (adaptive && !fixture)
            pass(lodPipelines[LodFingerprint].Get(), (view.grid.w + 127) / 128);
        pass(phaseMode ? phaseMark.Get() : mark.Get(), (brickGrid.w + 127) / 128);
        if (adaptive)
            pass(lodPipelines[LodPlan].Get(), (brickGrid.w + 127) / 128);
        pass(prepare.Get(), 1);
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        auto indirect = [&](ID3D12PipelineState *state, uint64_t offset) {
            cmd->SetPipelineState(state);
            cmd->ExecuteIndirect(dispatch.Get(), 1, arguments.resource.Get(), offset, nullptr, 0);
            gpu::uav(cmd);
        };
        if (adaptive) {
            indirect(lodPipelines[LodFine].Get(), 24);
            indirect(lodPipelines[LodCoarse].Get(), 36);
            indirect(lodPipelines[LodMasks].Get(), 12);
            indirect(lodPipelines[LodAnalyze].Get(), 12);
            pass(lodPipelines[LodGuard].Get(), (brickGrid.w + 127) / 128);
            indirect(lodPipelines[LodRepair].Get(), 0);
            indirect(lodPipelines[LodFinalize].Get(), 0);
            if (validateLodThisFrame)
                indirect(lodPipelines[LodReference].Get(), 0);
            pass(lodPipelines[LodCommit].Get(), (brickGrid.w + 127) / 128);
        } else
            indirect(phaseMode ? phaseReconstruct.Get() : reconstruct.Get(), 0);
        // Bounds reduction needs only active slots, just like reconstruction.
        cmd->SetPipelineState(bounds.Get());
        cmd->ExecuteIndirect(dispatch.Get(), 1, arguments.resource.Get(), 12, nullptr, 0);
        gpu::uav(cmd);
        if (adaptive && validateLodThisFrame) {
            uint64_t offset = 0;
            for (auto *resource : {field.resource.Get(), lodReference.resource.Get(), map.resource.Get(),
                                   lodNext.resource.Get()}) {
                gpu::transition(cmd, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
                cmd->CopyBufferRegion(lodSnapshot.resource.Get(), offset, resource, 0,
                                      resource->GetDesc().Width);
                offset += resource->GetDesc().Width;
                gpu::transition(cmd, resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            lodSnapshotReady = true;
        }
        gpu::transition(cmd, arguments.resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        for (auto r : {field.resource.Get(), map.resource.Get(), aabbs.resource.Get()})
            gpu::transition(cmd, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        readable = true;
    }
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    if (updating) {
        gpu::Event build(cmd, L"Fluid / GPU AABB BLAS rebuild (inactive topology changes)");
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        d.Inputs.NumDescs = 1;
        d.Inputs.pGeometryDescs = &geometry;
        d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        d.DestAccelerationStructureData = blas.resource->GetGPUVirtualAddress();
        d.ScratchAccelerationStructureData = scratch.resource->GetGPUVirtualAddress();
        cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        gpu::uav(cmd, blas.resource.Get());
    }
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    recorded = true;
    previousCamera = camera.position;
    previousSteps = system.stepCount;
    previousPhase = system.phase;
    previousPlanes = system.planes;
    // Clear the prior frame's material displacement once when simulation stops.
    // Otherwise a cached field would keep telling RR that stationary water moves.
    phaseMotion = phaseMode && system.advancedSeconds > 0 && !system.resetThisFrame;
}
void FluidSurface::recordReadback(ID3D12GraphicsCommandList *cmd) {
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, readback.resource.Get(), 0);
    gpu::transition(cmd, counts.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 24, counts.resource.Get(), 0,
                          adaptive        ? 96
                          : previousPhase ? 28
                                          : 16);
    gpu::transition(cmd, counts.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void FluidSurface::collect(uint64_t frequency) {
    void *data;
    D3D12_RANGE range{0, 120}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Fluid surface metrics map");
    const auto t = static_cast<const uint64_t *>(data);
    reconstructionMs = double(t[1] - t[0]) * 1000 / frequency;
    blasMs = double(t[2] - t[1]) * 1000 / frequency;
    const auto n = reinterpret_cast<const uint32_t *>(t + 3);
    activeBricks = n[0];
    surfaceBricks = n[1];
    const uint32_t bad = n[2];
    renderedVolume = n[3] * std::pow(double(minimumSpacing.w), 3) / 256;
    phaseHeightColumns = previousPhase ? n[4] : 0;
    phaseHeightNodes = previousPhase ? n[5] : 0;
    phasePlaneNodes = previousPhase ? n[6] : 0;
    if (adaptive) {
        std::copy(n + 4, n + 24, lodCounts.begin());
        lodNeedsUpdate = lodCounts[7] != 0;
    }
    readback.resource->Unmap(0, &written);
    if (activeBricks > brickGrid.w || surfaceBricks > activeBricks)
        throw std::runtime_error("Fluid surface brick counter corruption");
    if (bad)
        throw std::runtime_error("Nonfinite fluid reconstruction field/kernel");
    if (lodSnapshotReady)
        validateLod();
}
void FluidSurface::report(std::ostream &out) const {
    out << "{\"representation\":\"sparse trilinear phi / procedural DXR\",\"activeBricks\":" << activeBricks
        << ",\"surfaceBricks\":" << surfaceBricks << ",\"brickCapacity\":" << brickGrid.w
        << ",\"reconstructionMs\":" << reconstructionMs << ",\"blasMs\":" << blasMs
        << ",\"allocatedBytes\":" << allocatedBytes << ",\"tetrahedralVolumeEstimate\":" << renderedVolume
        << ",\"anisotropic\":" << (anisotropic && !previousPhase ? "true" : "false")
        << ",\"phaseGeometry\":" << (previousPhase ? "true" : "false");
    if (previousPhase)
        out << ",\"phaseHeightColumns\":" << phaseHeightColumns
            << ",\"phaseHeightNodes\":" << phaseHeightNodes << ",\"phasePlaneNodes\":" << phasePlaneNodes;
    if (adaptive)
        out << ",\"adaptiveSurface\":{\"fineGatherBricks\":" << lodCounts[0]
            << ",\"coarseAxes\":" << lodCoarseAxes << ",\"coarseGatherBricks\":" << lodCounts[1]
            << ",\"coarseSurfaceBricks\":" << lodCounts[2] << ",\"transitionSurfaceBricks\":" << lodCounts[3]
            << ",\"fineNodeEvaluations\":" << lodCounts[4] << ",\"coarseNodeEvaluations\":" << lodCounts[5]
            << ",\"fullRefreshBricks\":" << lodCounts[6] << ",\"repairBricks\":" << lodCounts[8]
            << ",\"repairNodeEvaluations\":" << lodCounts[9] << ",\"coarseBricks\":" << lodCounts[14]
            << ",\"phiToleranceCells\":" << lodPhiTolerance << ",\"fineInterfaceCells\":" << lodCounts[16]
            << ",\"analyzedBandNodes\":" << lodCounts[17] << ",\"normalTolerance\":" << lodNormalTolerance
            << ",\"audits\":" << lodAudits << ",\"maxPhiErrorCells\":" << lodMaxPhiError
            << ",\"maxNormalError\":" << lodMaxNormalError << ",\"maxBoundaryError\":" << lodBoundaryError
            << ",\"maxMotionBoundaryError\":" << lodMotionBoundaryError
            << ",\"admissionAudit\":{\"surfaceBricks\":" << lodAdmission[0]
            << ",\"protected\":" << lodAdmission[1] << ",\"signVeto\":" << lodAdmission[2]
            << ",\"phiRejected\":" << lodAdmission[3] << ",\"normalRejected\":" << lodAdmission[4]
            << ",\"accurate\":" << lodAdmission[5] << ",\"accurateButFine\":" << lodAdmission[6]
            << ",\"transitioning\":" << lodAdmission[7] << "}}";
    out << "}";
}
} // namespace lab
