#include "fluid_system.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
FluidSystem::FluidSystem(ID3D12Device *device, const std::filesystem::path &folder, const FluidSystemDesc &d)
    : desc(d) {
    if (desc.narrowBand && (!desc.cudaBackend || !desc.ownedParticles || desc.ballisticTest ||
                            desc.transferTest || desc.materialTest))
        throw std::runtime_error(
            "Narrow-band ownership requires the complete CUDA fluid solver and particle authority");
    if (!desc.cudaBackend &&
        (desc.cudaGraphs || desc.cudaGraphicsContext || desc.cudaMixedPressure ||
         desc.cudaForcedFinePressure || desc.cudaPressureBricks != 512 || desc.cudaPressureChanges != 64 ||
         desc.cudaCgIterations != 32 || !desc.cudaConditionalPressure))
        throw std::runtime_error("CUDA settings require --fluid-backend=cuda");
    if (desc.cudaForcedFinePressure && !desc.cudaMixedPressure)
        throw std::runtime_error("Fine CUDA MGPCG requires the qualified pressure path");
    if (!desc.cudaConditionalPressure && !desc.cudaMixedPressure)
        throw std::runtime_error("CUDA pressure loop selection requires fine or mixed pressure");
    if (!desc.cudaMixedPressure &&
        (desc.cudaPressureBricks != 512 || desc.cudaPressureChanges != 64 || desc.cudaCgIterations != 32))
        throw std::runtime_error("CUDA pressure budgets require --fluid-cuda-pressure=fine or mixed");
    if (!desc.cudaPressureBricks || desc.cudaPressureBricks > 16384 || !desc.cudaPressureChanges ||
        desc.cudaPressureChanges > 16384 || !desc.cudaCgIterations || desc.cudaCgIterations > 32)
        throw std::runtime_error("Invalid CUDA pressure pool/iteration budget");
    if (desc.cudaMixedPressure && (desc.ballisticTest || desc.transferTest || desc.materialTest))
        throw std::runtime_error("CUDA MGPCG requires complete fluid substeps, not partial test modes");
    if (desc.cudaBackend) {
#if !PT_FLUID_CUDA
        throw std::runtime_error("CUDA fluid was not built; configure NVMATRIXENGINE_CUDA_FLUID=ON");
#endif
        if (desc.pressureMode != FluidPressureMode::Uniform || desc.bulkInventory || desc.bulkProjected ||
            desc.bulkCapacity || desc.bulkBounded || desc.bulkPressure || desc.bulkImplicit ||
            desc.bulkAirExtension || desc.bulkCoupled || desc.cutCells || desc.cutPressure ||
            desc.cutTimeCentered || desc.adaptiveParticles || desc.coarseInterior || desc.adaptiveMac ||
            desc.macMultigrid || desc.tiledDensity || desc.sparseWork)
            throw std::runtime_error("CUDA requires fixed particle ownership and no DX12-only solver modes; "
                                     "use --fluid-cuda-pressure=mixed for CUDA mixed pressure");
    }
    if (desc.coarseInterior)
        desc.adaptiveParticles = true;
    if (desc.sparseWork)
        desc.tiledDensity = true;
    if (desc.ownedParticles && (desc.adaptiveParticles || desc.coarseInterior || desc.bulkInventory))
        throw std::runtime_error(
            "Authoritative particles cannot share the legacy resampler/interior/bulk mass ledger");
    if (!d.maxParticles || d.maxParticles > 1000000 || d.initialParticles > d.maxParticles ||
        !std::isfinite(d.simulationRate) || d.simulationRate < 30 || d.simulationRate > 1000 ||
        !d.maxSubsteps || d.maxSubsteps > 16 || !std::isfinite(d.particleRadius) ||
        !std::isfinite(d.gridCellSize) || d.particleRadius <= 0 || d.gridCellSize <= 0)
        throw std::runtime_error("Invalid fluid capacity/timestep/spacing");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite((&d.minimum.x)[i]) || !std::isfinite((&d.maximum.x)[i]) ||
            !std::isfinite((&d.gravity.x)[i]) || (&d.maximum.x)[i] - (&d.minimum.x)[i] < d.particleRadius * 4)
            throw std::runtime_error("Invalid fluid domain");
    for (int i = 0; i < 3; ++i) {
        const double ratio = (double((&d.maximum.x)[i]) - (&d.minimum.x)[i]) / d.gridCellSize;
        const double cells =
            std::abs(ratio - std::round(ratio)) < 1e-4 ? std::round(ratio) : std::ceil(ratio);
        if (cells < 1 || cells > 1048576)
            throw std::runtime_error("Invalid fluid grid dimension");
        (&grid.x)[i] = uint32_t(cells);
    }
    if (!d.pressureIterations || d.pressureIterations > 1000 || d.densityIterations > 1000 ||
        d.transferTest > 4 || !std::isfinite(d.flipRatio) || d.flipRatio < 0 || d.flipRatio > 1)
        throw std::runtime_error("Invalid fluid pressure/test configuration");
    if (!std::isfinite(d.density) || d.density <= 0 || !std::isfinite(d.viscosity) || d.viscosity < 0 ||
        !std::isfinite(d.surfaceTension) || d.surfaceTension < 0 || d.materialTest > 2)
        throw std::runtime_error("Invalid fluid material parameters");
    const double alpha = double(d.viscosity) / (d.simulationRate * d.gridCellSize * d.gridCellSize);
    if (alpha > 4.8)
        throw std::runtime_error(
            "Viscosity needs more than 32 diffusion subcycles: increase simulation rate or grid spacing");
    viscositySubsteps = std::max(1u, uint32_t(std::ceil(6 * alpha / .9)));
    // Explicit capillary-wave safety gate. Refuse an unstable requested material;
    // never silently weaken sigma. The normal water configuration is far below it.
    if (d.surfaceTension > 0 &&
        1 / d.simulationRate >
            .5 * std::sqrt(d.density * std::pow(d.gridCellSize, 3) / (XM_PI * d.surfaceTension)))
        throw std::runtime_error("Surface tension requires a smaller simulation timestep");
    uint64_t cellTotal = uint64_t(grid.x) * grid.y * grid.z;
    if (cellTotal > 1048576)
        throw std::runtime_error(
            "Initial dense fluid grid exceeds 1M cells; use coarser spacing/smaller bounds");
    grid.w = uint32_t(cellTotal);
    if (desc.cutTimeCentered && !desc.cutPressure)
        throw std::runtime_error("Time-centered cut geometry requires cut pressure");
    if (desc.cutPressure && (!desc.cutCells || !desc.adaptiveMac || desc.cutCellFixture))
        throw std::runtime_error(
            "Cut pressure requires geometric cut cells and mixed MAC, without geometry-only fixtures");
    if (desc.bulkProjected &&
        (!desc.bulkInventory || !desc.cutPressure || !desc.macMultigrid || desc.bulkFixture))
        throw std::runtime_error("Projected bulk requires cut-pressure MGPCG and no bulk fixture");
    if (desc.bulkBounded && !desc.bulkCapacity)
        throw std::runtime_error("Phase flux limiting requires capacity-bounded source admission");
    if (desc.bulkImplicit && (!desc.bulkPressure || desc.bulkBounded))
        throw std::runtime_error(
            "Implicit bulk requires pressure support and cannot use the explicit receiver limiter");
    if (desc.bulkAirExtension && !desc.bulkImplicit)
        throw std::runtime_error("Air carrier extension requires implicit bulk transport");
    if (desc.bulkCoupled && !desc.bulkAirExtension)
        throw std::runtime_error("Coupled capacity requires carrier transport");
    if (desc.bulkPressure &&
        ((!desc.bulkBounded && !desc.bulkImplicit) || !desc.bulkCapacity || !desc.cutTimeCentered))
        throw std::runtime_error(
            "Bulk pressure support requires capacity-admitted transport and time-centered cut geometry");
    if (desc.bulkCapacity && !desc.bulkProjected)
        throw std::runtime_error("Capacity admission requires projected bulk");
    if (desc.adaptiveMac) {
        if (desc.pressureMode != FluidPressureMode::Uniform)
            throw std::runtime_error(
                "Mixed MAC requires its own pressure operator; do not combine with fine-grid pressure modes");
        mac = std::make_unique<FluidMac>(device, folder, grid, desc.pressureIterations, desc.gridCellSize,
                                         desc.density, desc.simulationRate, desc.surfaceTension,
                                         desc.macMultigrid, desc.cutPressure);
    }
    if (desc.pressureMode != FluidPressureMode::Uniform)
        pressureSolver = std::make_unique<FluidPressure>(device, folder, grid, desc.pressureMode,
                                                         desc.pressureIterations, desc.pressureCycles);
    activeParticles = desc.initialParticles;
    if (desc.roomPool) {
        if (!std::isfinite(desc.initialDepth) || desc.initialDepth <= 2 * desc.particleRadius ||
            desc.initialDepth >= desc.maximum.y - desc.minimum.y)
            throw std::runtime_error("Invalid room liquid fill depth");
        initialMinimum = {desc.minimum.x + desc.particleRadius, desc.minimum.y + desc.particleRadius,
                          desc.minimum.z + desc.particleRadius};
        initialMaximum = {desc.maximum.x - desc.particleRadius, desc.minimum.y + desc.initialDepth,
                          desc.maximum.z - desc.particleRadius};
        const float volume = (initialMaximum.x - initialMinimum.x) * (initialMaximum.y - initialMinimum.y) *
                             (initialMaximum.z - initialMinimum.z);
        const float spacing = std::cbrt(volume / std::max(1u, desc.initialParticles));
        initialLattice = {std::max(1u, uint32_t((initialMaximum.x - initialMinimum.x) / spacing)), 1,
                          std::max(1u, uint32_t((initialMaximum.z - initialMinimum.z) / spacing)), 1};
        initialLattice.y = std::max(1u, (desc.initialParticles + initialLattice.x * initialLattice.z - 1) /
                                            (initialLattice.x * initialLattice.z));
        particleVolume = volume / (initialLattice.x * initialLattice.y * initialLattice.z);
        if (!desc.initialParticles)
            particleVolume = .5f * desc.gridCellSize * desc.gridCellSize * desc.gridCellSize;
    } else {
        const double n = std::max(1.0, std::ceil(std::cbrt(double(desc.initialParticles)))),
                     h = desc.gridCellSize;
        const double volume = (desc.maximum.x - desc.minimum.x) * (desc.maximum.y - desc.minimum.y) *
                              (desc.maximum.z - desc.minimum.z) * .105;
        particleVolume = float(std::min(.5 * h * h * h, volume / (n * n * n)));
    }
    if (desc.bulkInventory)
        bulk = std::make_unique<FluidBulk>(device, folder, desc, grid, particleVolume);
    if (desc.cutCells)
        cutCells = std::make_unique<FluidCutCells>(device, folder, desc, grid);
    if (desc.bulkPressure)
        bulkPressure = std::make_unique<FluidBulkPressure>(
            device, folder, grid, particleVolume, desc.bulkImplicit, desc.bulkAirExtension, desc.bulkCoupled);
    scanBlockCount = (grid.w + 255) / 256;
    auto make = [&](uint64_t count, const wchar_t *name, bool shared = true) {
        return gpu::buffer(device, count * 4, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           name, desc.cudaBackend && shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE);
    };
    cellCounts = make(grid.w, L"Fluid bin counts");
    cellQuanta = make(desc.adaptiveParticles || desc.ownedParticles ? grid.w : 1,
                      L"Fluid cell rest-mass cache", desc.ownedParticles);
    if (desc.ownedParticles)
        exchange = std::make_unique<FluidParticleGridExchange>(
            device, folder, grid, desc.maxParticles, particleVolume, desc.particleRadius,
            FluidParticleGridExchange::CudaSharing{desc.cudaBackend, desc.narrowBand});
    if (desc.narrowBand)
        narrowCapacity = make(uint64_t(grid.w) * 2, L"Fluid narrow-band physical grid capacities");
    if (desc.adaptiveParticles)
        resampling = std::make_unique<FluidResampling>(device, folder, desc, grid);
    if (desc.coarseInterior)
        interior = std::make_unique<FluidInterior>(device, folder, desc, grid);
    {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &argument, 0};
        gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&binDispatch)),
                   "Fluid conditional bin dispatch");
    }
    cellOffsets = make(grid.w + 1, L"Fluid exclusive cell ranges");
    cellCursor = make(grid.w, L"Fluid scatter cursors");
    sortedIndices = make(d.maxParticles, L"Fluid binned particle IDs");
    scanBlocks = make(scanBlockCount, L"Fluid block prefix scan", false);
    faceStride = (grid.x + 1) * (grid.y + 1) * (grid.z + 1);
    if (uint64_t(faceStride) * 3 > 65535ull * 128)
        throw std::runtime_error("Fluid MAC grid exceeds the initial one-dimensional dispatch limit");
    faces = make(uint64_t(faceStride) * 3 * 4, L"Fluid MAC faces velocity previous weight");
    faceScratch = make(uint64_t(faceStride) * 3 * 4, L"Fluid MAC extrapolation scratch");
    pressure[0] = make(grid.w, L"Fluid pressure ping");
    pressure[1] = make(grid.w, L"Fluid pressure pong");
    cellData = make(uint64_t(grid.w) * 4, L"Fluid classification divergence pressure diagnostics");
    densityData = make(uint64_t(grid.w) * 4, L"Fluid particle density and positional projection RHS");
    densityArguments = make(64, L"Fluid density-error conditional repair dispatch");
    materialData = make(uint64_t(grid.w) * 4, L"Fluid surface colour and curvature");
    previousPositions =
        make(uint64_t(d.maxParticles) * 4, L"Fluid previous rendered particle positions", desc.narrowBand);
    solidGrid = make(uint64_t(grid.w) * 4, L"Fluid solid phi and moving boundary velocity");
    colliderData = gpu::buffer(device, (desc.maxSubsteps + 2) * FluidColliderTimeline::sliceBytes,
                               D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_GENERIC_READ, L"Fluid rigid SDF colliders");
    meshData = gpu::buffer(device, 256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"Fluid empty mesh SDF binding");
    paused = d.transferTest != 0 || d.materialTest != 0;
    particles = gpu::buffer(device, uint64_t(d.maxParticles) * sizeof(FluidParticle), D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            L"Fluid particles APIC",
                            desc.cudaBackend ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE);
    uniforms = gpu::buffer(device, 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Fluid frame constants");
    timingReadback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid timestamp readback");
    D3D12_QUERY_HEAP_DESC q{};
    q.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    q.Count = 2;
    gpu::check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&queries)), "Fluid timestamps");
    D3D12_DESCRIPTOR_RANGE depthRange{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER p[31]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[0].Descriptor.ShaderRegister = 0;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[1].Descriptor.ShaderRegister = 0;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[2].DescriptorTable = {1, &depthRange};
    for (int i = 3; i < 16; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 2;
    }
    p[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[16].Descriptor.ShaderRegister = 1;
    p[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[17].Descriptor.ShaderRegister = 2;
    p[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[18].Descriptor.ShaderRegister = 14;
    p[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[19].Descriptor.ShaderRegister = 15;
    for (uint32_t i = 20; i < 22; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i + 2;
    }
    p[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[22].Descriptor.ShaderRegister = 28;
    for (uint32_t i = 23; i < 25; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i + 6;
    }
    for (uint32_t i = 25; i < 27; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i + 6;
    }
    p[27].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[27].Constants = {2, 0, 1};
    p[28].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[28].Descriptor.ShaderRegister = 35;
    p[29].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[29].Descriptor.ShaderRegister = 36;
    p[30].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[30].Descriptor.ShaderRegister = 37;
    D3D12_ROOT_SIGNATURE_DESC r{31, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    gpu::check(D3D12SerializeRootSignature(&r, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Fluid root serialization");
    gpu::check(
        device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)),
        "Fluid root");
    if (desc.sparseWork)
        work = std::make_unique<FluidWork>(device, folder, root.Get(), grid, desc.coarseInterior,
                                           desc.ownedParticles);
    auto compute = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
        auto code = gpu::bytes(folder / "shaders" / (std::string("Fluid") + name + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC c{};
        c.pRootSignature = root.Get();
        c.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&c, IID_PPV_ARGS(&out)), name);
    };
    compute("Initialize", initialize);
    compute("Emit", emit);
    compute("Integrate", integrate);
    compute("Snapshot", snapshot);
    compute("BakeSolids", bakeSolids);
    compute("Collide", collide);
    compute("ClearBins", clearBins);
    compute("CountBins", countBins);
    compute("ScanCells", scanCells);
    compute("ScanSums", scanSums);
    compute("FinishScan", finishScan);
    compute("Scatter", scatter);
    if (desc.deterministicBins)
        compute("SortBins", sortBins);
    compute(desc.ownedParticles ? (desc.sparseWork ? "P2GSparseAuthority" : "P2GAuthority")
                                : (desc.sparseWork ? "P2GSparse" : "P2G"),
            p2g);
    if (desc.ownedParticles)
        compute("GatherMassAuthority", gatherMass);
    compute("G2P", g2p);
    compute("Extrapolate", extrapolate);
    compute(desc.bulkPressure  ? "DensityGatherCutBulk"
            : desc.cutPressure ? "DensityGatherCut"
                               : "DensityGather",
            densityGather);
    compute("DensityJacobi", densityJacobi);
    if (desc.tiledDensity)
        compute(desc.sparseWork ? "DensityTileSparse" : "DensityTileJacobi", densityTileJacobi);
    compute(desc.cutPressure ? "DensityDisplaceCut" : "DensityDisplace", densityDisplace);
    compute(desc.narrowBand ? "DensityMeasureNarrow" : "DensityMeasure", densityMeasure);
    compute(desc.bulkPressure  ? "DensityGatherAdaptiveCutBulk"
            : desc.cutPressure ? "DensityGatherAdaptiveCut"
                               : "DensityGatherAdaptive",
            densityGatherAdaptive);
    compute("DensityClearArguments", densityClearArguments);
    if (desc.cutPressure)
        compute("DensityContinueArguments", densityContinueArguments);
    compute("DensityPrepareArguments", densityPrepareArguments);
    compute(desc.cutPressure ? "ClassifyCut" : "Classify", classify);
    compute(desc.cutPressure ? "ForcesCut" : "Forces", forces);
    compute(desc.cutPressure ? "DivergenceCut" : "Divergence", divergence);
    compute("Jacobi", jacobi);
    compute("Project", project);
    compute("Measure", measure);
    compute(desc.cutPressure ? "ViscosityCut" : "Viscosity", viscosity);
    compute(desc.bulkPressure ? "SurfaceColorBulk" : "SurfaceColor", surfaceColor);
    compute(desc.cutPressure ? "SurfaceCurvatureCut" : "SurfaceCurvature", surfaceCurvature);
    compute("MaterialFixture", materialFixture);
    auto vs = gpu::bytes(folder / "shaders/FluidDebugVS.dxil"),
         ps = gpu::bytes(folder / "shaders/FluidDebugPS.dxil");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g{};
    g.pRootSignature = root.Get();
    g.VS = {vs.data(), vs.size()};
    g.PS = {ps.data(), ps.size()};
    g.SampleMask = UINT_MAX;
    g.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    g.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    g.RasterizerState.DepthClipEnable = TRUE;
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    g.SampleDesc.Count = 1;
    gpu::check(device->CreateGraphicsPipelineState(&g, IID_PPV_ARGS(&debug)), "Fluid debug PSO");
}
void FluidSystem::bind(ID3D12GraphicsCommandList *cmd) {
    cmd->SetComputeRootSignature(root.Get());
    cmd->SetComputeRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    if (exchange)
        cmd->SetComputeRootUnorderedAccessView(30, exchange->particleQuantities()->GetGPUVirtualAddress());
    if (bulkPressure)
        cmd->SetComputeRootUnorderedAccessView(29, bulkPressure->supportVolume()->GetGPUVirtualAddress());
    if (desc.cutPressure) {
        const auto view = cutCells->gpuView();
        cmd->SetComputeRootUnorderedAccessView(25, view.fineVolume->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(26, view.fineArea->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstant(27, view.swept ? 1u : 0u, 0);
        cmd->SetComputeRootUnorderedAccessView(28, view.solidKernel->GetGPUVirtualAddress());
    }
    cmd->SetComputeRootUnorderedAccessView(1, particles.resource->GetGPUVirtualAddress());
    ID3D12Resource *bins[] = {cellCounts.resource.Get(), cellOffsets.resource.Get(),
                              cellCursor.resource.Get(), sortedIndices.resource.Get(),
                              scanBlocks.resource.Get()};
    for (int i = 0; i < 5; ++i)
        cmd->SetComputeRootUnorderedAccessView(i + 3, bins[i]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(8, faces.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(9, pressure[0].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(10, pressure[1].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(11, cellData.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(12, faceScratch.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(13, densityData.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(14, previousPositions.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(15, solidGrid.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(16, colliderData.resource->GetGPUVirtualAddress() + colliderOffset);
    cmd->SetComputeRootShaderResourceView(17, meshData.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(18, materialData.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(19, cellQuanta.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(
        20, (interior ? interior->state() : particles.resource.Get())->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(
        21, (interior ? interior->totals() : cellQuanta.resource.Get())->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(22, densityArguments.resource->GetGPUVirtualAddress());
    if (work) {
        cmd->SetComputeRootUnorderedAccessView(23, work->data()->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(24, work->arguments()->GetGPUVirtualAddress());
    }
}
FluidParticleGridExchange::View FluidSystem::exchangeView() const {
    FluidParticleGridExchange::View v{};
    v.particles = particles.resource.Get();
    v.previousPositions = previousPositions.resource.Get();
    v.reusableParticleLimit = activeParticles;
    return v;
}
void FluidSystem::setColliders(const std::vector<FluidCollider> &c, uint32_t discontinuities) {
    collidersDirty = colliderTimeline.set(c, discontinuities);
    colliderCount = colliderTimeline.count;
}
void FluidSystem::setMeshSdf(const MeshSdfAsset &asset) {
    if (hasRecorded)
        throw std::runtime_error("Mesh SDF replacement requires a quiescent fluid system");
    if (asset.phi.empty() ||
        uint64_t(asset.dimensions.x) * asset.dimensions.y * asset.dimensions.z != asset.phi.size())
        throw std::runtime_error("Invalid mesh SDF asset dimensions");
    ComPtr<ID3D12Device> device;
    gpu::check(particles.resource->GetDevice(IID_PPV_ARGS(&device)), "Fluid SDF device");
    const uint64_t size = asset.phi.size() * sizeof(float);
    meshAsset = asset;
    meshUpload = gpu::buffer(device.Get(), size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ, L"Fluid immutable mesh SDF staging");
    memcpy(meshUpload.mapped, asset.phi.data(), size_t(size));
    meshData = gpu::buffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid GPU mesh SDF asset");
    meshPending = true;
}
FluidGpuView FluidSystem::gpuView() const {
    return {particles.resource.Get(),
            cellOffsets.resource.Get(),
            sortedIndices.resource.Get(),
            previousPositions.resource.Get(),
            grid,
            colliderData.resource.Get(),
            meshData.resource.Get(),
            colliderCount,
            faces.resource.Get(),
            materialData.resource.Get(),
            cellQuanta.resource.Get(),
            interior ? interior->state() : particles.resource.Get(),
            interior ? interior->totals() : cellQuanta.resource.Get(),
            interior != nullptr,
            colliderData.resource->GetGPUVirtualAddress() + colliderOffset};
}
void FluidSystem::bin(ID3D12GraphicsCommandList *cmd, ID3D12Resource *arguments) {
    gpu::Event event(cmd, L"Fluid / histogram - scan - scatter");
    uint32_t stage = 0;
    if (arguments)
        gpu::transition(cmd, arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto pass = [&](ID3D12PipelineState *state, uint32_t groups) {
        cmd->SetPipelineState(state);
        if (arguments)
            cmd->ExecuteIndirect(binDispatch.Get(), 1, arguments, 12 * stage++, nullptr, 0);
        else
            cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    pass(clearBins.Get(), scanBlockCount);
    pass(countBins.Get(), (desc.maxParticles + 255) / 256);
    pass(scanCells.Get(), scanBlockCount);
    pass(scanSums.Get(), 1);
    pass(finishScan.Get(), scanBlockCount);
    pass(scatter.Get(), (desc.maxParticles + 255) / 256);
    if (desc.deterministicBins) {
        gpu::Event stable(cmd, L"Fluid / deterministic cell ID ordering (reference mode)");
        cmd->SetPipelineState(sortBins.Get());
        if (arguments)
            cmd->ExecuteIndirect(binDispatch.Get(), 1, arguments, 24, nullptr, 0); // cell scan dimensions
        else
            cmd->Dispatch(scanBlockCount, 1, 1);
        gpu::uav(cmd);
    }
    if (gatherMass) {
        cmd->SetPipelineState(gatherMass.Get());
        if (arguments)
            cmd->ExecuteIndirect(binDispatch.Get(), 1, arguments, 24, nullptr, 0);
        else
            cmd->Dispatch(scanBlockCount, 1, 1);
        gpu::uav(cmd, cellQuanta.resource.Get());
    }
    if (arguments)
        gpu::transition(cmd, arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
void FluidSystem::projectGrid(ID3D12GraphicsCommandList *cmd) {
    auto pressureGeometry = [&] {
        if (desc.cutTimeCentered) {
            const auto view = cutCells->gpuView();
            cmd->SetComputeRootUnorderedAccessView(26, view.pressureArea->GetGPUVirtualAddress());
            cmd->SetComputeRoot32BitConstant(27, (view.swept ? 1u : 0u) | (view.timeCentered ? 2u : 0u), 0);
        }
    };
    pressureGeometry();
    gpu::Event event(cmd, L"Fluid / free-surface pressure projection (Jacobi V1)");
    auto pass = [&](ID3D12PipelineState *state, uint32_t groups) {
        cmd->SetPipelineState(state);
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    pass(classify.Get(), scanBlockCount);
    if (bulkPressure) {
        bulkPressure->record(cmd, bulk->gpuView(), cutCells->gpuView(), cellData.resource.Get(),
                             faces.resource.Get());
        bind(cmd);
        pressureGeometry();
    }
    if (desc.materialTest) {
        if (desc.materialTest == 1) {
            pass(materialFixture.Get(), (faceStride * 3 + 127) / 128);
            diffuseVelocity(cmd);
        } else {
            pass(surfaceColor.Get(), scanBlockCount);
            pass(surfaceCurvature.Get(), scanBlockCount);
        }
        return;
    }
    if (desc.transferTest == 1 || desc.transferTest == 2 || desc.transferTest == 4)
        return;
    pass(forces.Get(), (faceStride * 3 + 127) / 128);
    if (!desc.ballisticTest && !desc.transferTest) {
        if (desc.viscosity > 0)
            diffuseVelocity(cmd);
        if (desc.surfaceTension > 0) {
            gpu::Event surfaceEvent(cmd, L"Fluid / surface colour - curvature - capillary pressure");
            pass(surfaceColor.Get(), scanBlockCount);
            pass(surfaceCurvature.Get(), scanBlockCount);
        }
    }
    pass(divergence.Get(), scanBlockCount);
    pressureIndex = 0;
    if (mac) {
        if (desc.cutPressure)
            mac->setCutCells(cutCells->gpuView());
        pressureIndex =
            mac->solve(cmd, uniforms.resource.Get(), faces.resource.Get(), faceScratch.resource.Get(),
                       cellData.resource.Get(), solidGrid.resource.Get(), materialData.resource.Get(),
                       pressure[0].resource.Get(), pressure[1].resource.Get());
        bind(cmd);
        cmd->SetComputeRootUnorderedAccessView(9, pressure[pressureIndex].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(10,
                                               pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
        return;
    }
    if (pressureSolver) {
        pressureIndex = pressureSolver->solve(cmd, faceScratch.resource.Get(), pressure[0].resource.Get(),
                                              pressure[1].resource.Get());
        bind(cmd);
    } else
        for (uint32_t i = 0; i < desc.pressureIterations; ++i) {
            cmd->SetComputeRootUnorderedAccessView(9,
                                                   pressure[pressureIndex].resource->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(
                10, pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
            pass(jacobi.Get(), scanBlockCount);
            pressureIndex = 1 - pressureIndex;
        }
    cmd->SetComputeRootUnorderedAccessView(9, pressure[pressureIndex].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(10, pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
    pass(project.Get(), (faceStride * 3 + 127) / 128);
    pass(measure.Get(), scanBlockCount);
}
void FluidSystem::diffuseVelocity(ID3D12GraphicsCommandList *cmd) {
    gpu::Event event(cmd, L"Fluid / timestep-bounded MAC viscosity");
    cmd->SetPipelineState(viscosity.Get());
    for (uint32_t i = 0; i < viscositySubsteps; ++i) {
        cmd->SetComputeRootUnorderedAccessView(8, faces.resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(12, faceScratch.resource->GetGPUVirtualAddress());
        cmd->Dispatch((faceStride * 3 + 127) / 128, 1, 1);
        gpu::uav(cmd);
        std::swap(faces, faceScratch);
    }
    cmd->SetComputeRootUnorderedAccessView(8, faces.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(12, faceScratch.resource->GetGPUVirtualAddress());
}
void FluidSystem::transferToGrid(ID3D12GraphicsCommandList *cmd, bool binsCurrent) {
    if (!binsCurrent)
        bin(cmd);
    if (bulkSourcesPending) {
        bulk->sources(cmd, gpuView(), solidGrid.resource.Get());
        bulkSourcesPending = false;
        bind(cmd);
    }
    {
        gpu::Event event(cmd, L"Fluid / APIC particle-to-MAC grid gather");
        if (work) {
            work->prepareFaces(cmd);
            uint32_t stamp = work->begin(cmd, FluidWork::P2G);
            work->execute(cmd, p2g.Get(), false);
            work->finish(cmd);
            work->end(cmd, stamp);
            work->auditFaces(cmd, cellCounts.resource.Get(), cellQuanta.resource.Get(), faces.resource.Get());
        } else {
            cmd->SetPipelineState(p2g.Get());
            cmd->Dispatch((faceStride * 3 + 127) / 128, 1, 1);
            gpu::uav(cmd, faces.resource.Get());
        }
    }
    projectGrid(cmd);
}
void FluidSystem::transferToParticles(ID3D12GraphicsCommandList *cmd) {
    if (!desc.transferTest) {
        gpu::Event event(cmd, L"Fluid / free-surface velocity extrapolation");
        for (uint32_t i = 0; i < 4; ++i) {
            auto src = i % 2 ? faceScratch.resource.Get() : faces.resource.Get();
            auto dst = i % 2 ? faces.resource.Get() : faceScratch.resource.Get();
            cmd->SetComputeRootUnorderedAccessView(8, src->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(12, dst->GetGPUVirtualAddress());
            cmd->SetPipelineState(extrapolate.Get());
            cmd->Dispatch((faceStride * 3 + 127) / 128, 1, 1);
            gpu::uav(cmd);
        }
        cmd->SetComputeRootUnorderedAccessView(8, faces.resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(12, faceScratch.resource->GetGPUVirtualAddress());
    }
    gpu::Event event(cmd, L"Fluid / G2P APIC-FLIP and grid advection");
    const uint32_t stamp = work ? work->begin(cmd, FluidWork::G2P) : 0;
    cmd->SetPipelineState(g2p.Get());
    cmd->Dispatch((desc.maxParticles + 127) / 128, 1, 1);
    gpu::uav(cmd, particles.resource.Get());
    if (work)
        work->end(cmd, stamp);
}
void FluidSystem::correctDensity(ID3D12GraphicsCommandList *cmd) {
    if (desc.transferTest || !desc.densityIterations)
        return;
    gpu::Event event(cmd, L"Fluid / positional density projection (no velocity injection)");
    bin(cmd);
    auto pass = [&](ID3D12PipelineState *state, uint32_t groups, uint32_t rows = 1) {
        cmd->SetPipelineState(state);
        cmd->Dispatch(groups, rows, 1);
        gpu::uav(cmd);
    };
    pass(densityGather.Get(), (grid.w + 127) / 128);
    if (work) {
        work->prepareDensity(cmd, false);
        work->auditDensity(cmd, faceScratch.resource.Get(), densityArguments.resource.Get(), false);
    }
    const uint32_t stamp = work ? work->begin(cmd, FluidWork::DensitySolve) : 0;
    pressureIndex = 0;
    for (uint32_t i = 0; i < desc.densityIterations;) {
        const bool paired = desc.tiledDensity && i + 1 < desc.densityIterations;
        cmd->SetComputeRootUnorderedAccessView(9, pressure[pressureIndex].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(10,
                                               pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
        const uint32_t tiles = ((grid.x + 7) / 8) * ((grid.y + 3) / 4) * ((grid.z + 3) / 4);
        if (paired && work)
            work->execute(cmd, densityTileJacobi.Get(), true);
        else
            pass(paired ? densityTileJacobi.Get() : densityJacobi.Get(),
                 paired ? std::min(tiles, 65535u) : scanBlockCount, paired ? (tiles + 65534) / 65535 : 1);
        pressureIndex = 1 - pressureIndex;
        i += paired ? 2 : 1;
    }
    if (work) {
        work->finish(cmd);
        work->end(cmd, stamp);
        work->auditDensityResult(cmd, pressure[pressureIndex].resource.Get(), desc.densityIterations);
    }
    cmd->SetComputeRootUnorderedAccessView(9, pressure[pressureIndex].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(10, pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
    pass(densityDisplace.Get(), (desc.maxParticles + 127) / 128);
}
void FluidSystem::repairDensity(ID3D12GraphicsCommandList *cmd, bool currentBins) {
    // This is a bounded nonlinear correction, not an adaptive pressure grid.
    // Classification/gather are uniform today; the expensive iterations and
    // their resulting rebin are GPU-gated by actual post-contact density error.
    gpu::Event event(cmd, L"Fluid / density-error-driven repair");
    if (!currentBins)
        bin(cmd);
    if (desc.transferTest || !desc.densityIterations)
        return;
    auto pass = [&](ID3D12PipelineState *state, uint32_t groups) {
        cmd->SetPipelineState(state);
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd);
    };
    pass(currentBins ? densityContinueArguments.Get() : densityClearArguments.Get(), 1);
    pass(densityGatherAdaptive.Get(), (grid.w + 127) / 128);
    pass(densityPrepareArguments.Get(), 1);
    if (work) {
        work->prepareDensity(cmd, true);
        work->auditDensity(cmd, faceScratch.resource.Get(), densityArguments.resource.Get(), true);
    }
    const uint32_t stamp = work ? work->begin(cmd, FluidWork::DensitySolve) : 0;
    auto *arguments = densityArguments.resource.Get();
    gpu::transition(cmd, arguments, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    auto indirect = [&](ID3D12PipelineState *state, uint32_t slot) {
        cmd->SetPipelineState(state);
        cmd->ExecuteIndirect(binDispatch.Get(), 1, arguments, 12 * slot, nullptr, 0);
        gpu::uav(cmd);
    };
    pressureIndex = 0;
    for (uint32_t i = 0; i < desc.densityIterations;) {
        const bool paired = desc.tiledDensity && i + 1 < desc.densityIterations;
        cmd->SetComputeRootUnorderedAccessView(9, pressure[pressureIndex].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(10,
                                               pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
        if (paired && work)
            work->execute(cmd, densityTileJacobi.Get(), true);
        else
            indirect(paired ? densityTileJacobi.Get() : densityJacobi.Get(), paired ? 10 : 6);
        pressureIndex = 1 - pressureIndex;
        i += paired ? 2 : 1;
    }
    if (work) {
        work->finish(cmd);
        work->end(cmd, stamp);
        work->auditDensityResult(cmd, pressure[pressureIndex].resource.Get(), desc.densityIterations,
                                 arguments);
    }
    cmd->SetComputeRootUnorderedAccessView(9, pressure[pressureIndex].resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(10, pressure[1 - pressureIndex].resource->GetGPUVirtualAddress());
    indirect(densityDisplace.Get(), 7);
    if (colliderCount)
        indirect(collide.Get(), 7);
    gpu::transition(cmd, arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bin(cmd, arguments);
}
void FluidSystem::record(ID3D12GraphicsCommandList *cmd, float dt, const Camera &camera,
                         ID3D12CommandQueue *queue, ID3D12CommandAllocator *allocator,
                         [[maybe_unused]] gpu::SubmissionTimeline *timeline) {
    if (desc.cudaBackend && (!queue || !allocator))
        throw std::runtime_error("CUDA fluid needs the renderer queue and active allocator");
#if PT_FLUID_CUDA
    if (desc.cudaBackend && !cuda) {
        ComPtr<ID3D12Device> device;
        gpu::check(particles.resource->GetDevice(IID_PPV_ARGS(&device)), "CUDA fluid device");
        cuda_fluid::Config config{grid.x,
                                  grid.y,
                                  grid.z,
                                  desc.maxParticles,
                                  desc.pressureIterations,
                                  desc.densityIterations,
                                  viscositySubsteps,
                                  desc.transferTest,
                                  desc.materialTest,
                                  desc.ballisticTest,
                                  desc.deterministicBins,
                                  desc.cudaGraphs};
        config.mixedPressure = desc.cudaMixedPressure;
        config.forcedFinePressure = desc.cudaForcedFinePressure;
        config.pressureBrickCapacity = desc.cudaPressureBricks;
        config.pressureChangesPerFrame = desc.cudaPressureChanges;
        config.cgIterations = desc.cudaCgIterations;
        config.pressureConditionalGraphs = desc.cudaConditionalPressure;
        config.ownedParticles = desc.ownedParticles;
        config.narrowBand = desc.narrowBand;
        const std::array<ID3D12Resource *, cuda_fluid::BufferCount> shared{
            particles.resource.Get(),    cellCounts.resource.Get(),      cellOffsets.resource.Get(),
            cellCursor.resource.Get(),   sortedIndices.resource.Get(),   faces.resource.Get(),
            pressure[0].resource.Get(),  pressure[1].resource.Get(),     cellData.resource.Get(),
            faceScratch.resource.Get(),  densityData.resource.Get(),     solidGrid.resource.Get(),
            materialData.resource.Get(), densityArguments.resource.Get()};
        std::array<ID3D12Resource *, cuda_fluid::OwnershipBufferCount> owned{};
        if (exchange)
            owned = {exchange->particleQuantities(), exchange->referenceVelocities(),
                     cellQuanta.resource.Get(), exchange->counters()};
        std::array<ID3D12Resource *, cuda_fluid::GridInventoryBufferCount> gridOwned{};
        if (desc.narrowBand)
            gridOwned = {exchange->gridRead(), narrowCapacity.resource.Get()};
        cuda = std::make_unique<FluidCuda>(
            device.Get(), config, shared, meshAsset.phi, queue, desc.cudaGraphicsContext, owned, gridOwned,
            std::array<ID3D12Resource *, cuda_fluid::SurfaceGeometryBufferCount>{},
            desc.narrowBand ? previousPositions.resource.Get() : nullptr);
    }
#endif
    std::optional<gpu::Event> marker;
    marker.emplace(cmd, L"Fluid / simulation");
    if (meshPending) {
        cmd->CopyBufferRegion(meshData.resource.Get(), 0, meshUpload.resource.Get(), 0,
                              meshData.resource->GetDesc().Width);
        gpu::transition(cmd, meshData.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        meshPending = false;
    }
    hasRecorded = true;
    if (pressureSolver)
        pressureSolver->beginFrame(validatePressureThisFrame);
    if (mac)
        mac->beginFrame(resetPending, validateMacThisFrame || validatePressureThisFrame);
    Uniforms c{};
    c.minimumCell = {desc.minimum.x, desc.minimum.y, desc.minimum.z, desc.gridCellSize};
    c.maximumRadius = {desc.maximum.x, desc.maximum.y, desc.maximum.z, desc.particleRadius};
    c.gravityDt = {desc.gravity.x, desc.gravity.y, desc.gravity.z, 1 / desc.simulationRate};
    c.counts = {desc.maxParticles, desc.initialParticles,
                uint32_t(std::ceil(std::cbrt(double(desc.initialParticles)))), desc.transferTest};
    c.viewProjection = camera.viewProjection;
    c.cameraRight = {camera.right.x, camera.right.y, camera.right.z, 0};
    c.cameraUp = {camera.up.x, camera.up.y, camera.up.z, 0};
    c.cameraPosition = {camera.position.x, camera.position.y, camera.position.z, 0};
    c.cameraForward = {camera.forward.x, camera.forward.y, camera.forward.z, 0};
    c.grid = grid;
    c.solver = {desc.density, desc.flipRatio, desc.transfer == FluidTransfer::Flip ? 1.f : 0.f, 0};
    const double h = desc.gridCellSize;
    c.solver.w = float(particleVolume / (h * h * h));
    c.display = {debugMode, 0, desc.coarseInterior ? 1u : 0u,
                 desc.ownedParticles ? 2u : (desc.adaptiveParticles ? 1u : 0u)};
    c.collision = {colliderCount, 0, 0, 0};
    c.material = {desc.viscosity /
                      (desc.simulationRate * desc.gridCellSize * desc.gridCellSize * viscositySubsteps),
                  desc.ballisticTest || desc.transferTest ? 0.f : desc.surfaceTension,
                  float(viscositySubsteps), float(desc.materialTest)};
    c.initialMinimum = {initialMinimum.x, initialMinimum.y, initialMinimum.z, particleVolume};
    c.initialMaximum = {initialMaximum.x, initialMaximum.y, initialMaximum.z, 0};
    c.initialLattice = initialLattice;
    uint32_t steps = 0;
    if (resetPending) {
        accumulator = emissionRemainder = 0;
        activeParticles = desc.initialParticles;
        emittedParticles = 0;
        emitterFull = desc.roomPool && activeParticles == desc.maxParticles;
    }
    if (!paused) {
        accumulator += std::max(0.f, dt);
        const double limit = desc.maxSubsteps / desc.simulationRate;
        if (accumulator > limit) {
            droppedSeconds += accumulator - limit;
            accumulator = limit;
        }
        steps = std::min(desc.maxSubsteps, uint32_t((accumulator + 1e-8) * desc.simulationRate));
        accumulator -= steps / desc.simulationRate;
    } else
        accumulator = 0;
    if (singleStep && paused)
        steps = 1;
    singleStep = false;
    advancedSeconds = steps / desc.simulationRate;
    colliderTimeline.prepare(steps, desc.simulationRate, resetPending, paused, desc.bulkCoupled);
    memcpy(colliderData.mapped, colliderTimeline.slices.data(),
           size_t((steps + 1) * FluidColliderTimeline::sliceBytes));
    const uint64_t startOffset = (desc.maxSubsteps + 1) * FluidColliderTimeline::sliceBytes;
    memcpy(static_cast<char *>(colliderData.mapped) + startOffset, colliderTimeline.simulationStart.data(),
           FluidColliderTimeline::sliceBytes);
    colliderOffset = 0;
    if (emitterFull)
        emitter.enabled = false;
    if (emitter.enabled && desc.roomPool && steps && !emitterFull) {
        const auto v = emitter.velocity;
        const double speed = std::sqrt(double(v.x) * v.x + double(v.y) * v.y + double(v.z) * v.z);
        if (!std::isfinite(speed) || speed < .01 || speed > 10 || !std::isfinite(emitter.radius) ||
            emitter.radius < .01f || emitter.radius > .5f)
            throw std::runtime_error("Invalid fluid inlet speed/radius");
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite((&emitter.position.x)[axis]) ||
                (&emitter.position.x)[axis] - emitter.radius < (&desc.minimum.x)[axis] ||
                (&emitter.position.x)[axis] + emitter.radius > (&desc.maximum.x)[axis])
                throw std::runtime_error("Fluid inlet disc leaves the simulation domain");
        emissionRemainder +=
            XM_PI * emitter.radius * emitter.radius * speed * advancedSeconds / particleVolume;
        const uint32_t count =
            uint32_t(std::min(double(desc.maxParticles - activeParticles), std::floor(emissionRemainder)));
        c.emission = {activeParticles, count, uint32_t(emittedParticles), 0};
        activeParticles += count;
        emittedParticles += count;
        emissionRemainder -= count;
        emitterFull = activeParticles == desc.maxParticles;
        if (emitterFull) {
            emitter.enabled = false;
            emissionRemainder = 0;
        }
    }
    c.emitterOriginRadius = {emitter.position.x, emitter.position.y, emitter.position.z, emitter.radius};
    c.emitterVelocity = {emitter.velocity.x, emitter.velocity.y, emitter.velocity.z, 0};
    memcpy(uniforms.mapped, &c, sizeof(c));
    bind(cmd);
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    if (work)
        work->beginFrame(cmd, resetPending, validateWorkThisFrame);
    if (cutCells)
        cutCells->beginFrame(resetPending, validateCutCellsThisFrame);
    if (bulkPressure) {
        // MAC validation audits every frame, including projections preceding a
        // pause/reset. A final-frame-only bulk snapshot cannot cover those calls.
        bulkPressure->beginFrame(cmd, validateBulkThisFrame || validateMacThisFrame);
        bind(cmd);
    }
    cmd->SetPipelineState(densityClearArguments.Get());
    cmd->Dispatch(1, 1, 1);
    gpu::uav(cmd, densityArguments.resource.Get());
    if (bulk) {
        bulkSourcesPending = resetPending || c.emission.y != 0;
        bulk->validateImplicitThisFrame = validateBulkThisFrame || validateMacThisFrame;
        bulk->beginFrame(cmd, camera, resetPending ? 0 : c.emission.x,
                         resetPending ? activeParticles : c.emission.y, resetPending, validateBulkThisFrame,
                         activeParticles);
        bind(cmd);
    }
    bool binNeeded = resetPending;
    if (interior) {
        interior->begin(cmd, gpuView(), uniforms.resource.Get(), solidGrid.resource.Get(), resetPending,
                        activeParticles, validateInteriorThisFrame, steps != 0, collidersDirty);
        bind(cmd);
    }
    resetThisFrame = resetPending;
    if (resetPending) {
        gpu::Event event(cmd, L"Fluid / GPU spawn initial block");
        cmd->SetPipelineState(initialize.Get());
        cmd->Dispatch((desc.maxParticles + 255) / 256, 1, 1);
        gpu::uav(cmd, particles.resource.Get());
        if (desc.roomPool && colliderCount) {
            cmd->SetPipelineState(collide.Get());
            cmd->Dispatch((desc.maxParticles + 127) / 128, 1, 1);
            gpu::uav(cmd);
        }
        resetPending = false;
        stepCount = 0;
    }
    cmd->SetPipelineState(snapshot.Get());
    cmd->Dispatch((desc.maxParticles + 255) / 256, 1, 1);
    gpu::uav(cmd);
    if (c.emission.y) {
        gpu::Event event(cmd, L"Fluid / swept-disc GPU inlet");
        cmd->SetPipelineState(emit.Get());
        cmd->Dispatch((c.emission.y + 255) / 256, 1, 1);
        gpu::uav(cmd);
    }
    if (exchange) {
        auto v = exchangeView();
        exchange->begin(cmd, v, resetThisFrame);
        exchange->seed(cmd, v, resetThisFrame ? 0 : c.emission.x,
                       resetThisFrame ? activeParticles : c.emission.y);
        bind(cmd);
    }
#if PT_FLUID_CUDA
    if (cuda) {
        changedThisFrame = resetThisFrame || steps != 0 || collidersDirty;
        collidersDirty = false;
        // Initialization/emission/previous-render snapshots stay in the DX12
        // prefix. All solver substeps use a single GPU ownership handoff.
        // PIX regions must never straddle command-list Close/Reset.
        marker.reset();
        cuda->run(cmd, queue, allocator, c, colliderTimeline, steps, binNeeded, resetThisFrame, timeline);
        const auto state = cuda->state();
        if (state.facesSwapped != cudaFacesSwapped) {
            std::swap(faces, faceScratch);
            cudaFacesSwapped = state.facesSwapped;
        }
        pressureIndex = state.pressureIndex;
        stepCount += steps;
        bind(cmd); // Reset clears every root/PSO binding.
        cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        return;
    }
#endif
    cmd->SetPipelineState(bakeSolids.Get());
    cmd->Dispatch(scanBlockCount, 1, 1);
    gpu::uav(cmd);
    changedThisFrame = resetThisFrame || steps != 0 || collidersDirty;
    if (cutCells && (desc.cutPressure || !steps)) {
        auto view = gpuView();
        auto colliders = std::span<const FluidCollider>(colliderTimeline.slices.data(), colliderCount);
        if (desc.cutPressure) {
            view.colliderAddress = colliderData.resource->GetGPUVirtualAddress() + startOffset;
            colliders = {colliderTimeline.simulationStart.data(), colliderCount};
        }
        cutCells->record(cmd, view, colliders, meshAsset, stepCount, false);
        if (desc.bulkProjected)
            bulk->setProjection(cutCells->gpuView(), mac->projectedVolumeFlux());
        bind(cmd);
    }
    if (interior) {
        // The previous end-of-frame bins are already current without new births.
        if (resetThisFrame || c.emission.y)
            bin(cmd);
        interior->restore(cmd, gpuView(), uniforms.resource.Get(), solidGrid.resource.Get());
        bind(cmd);
        bin(cmd, interior->binArguments());
    }
    bool sourceBinsCurrent = false;
    if (desc.bulkCapacity) {
        // Admission uses the simulation START geometry and capacity, before any
        // moving-boundary endpoint or pressure projection is recorded.
        if (bulkSourcesPending) {
            bin(cmd);
            bulk->sources(cmd, gpuView(), solidGrid.resource.Get());
            bulkSourcesPending = false;
            sourceBinsCurrent = true;
            bind(cmd);
        }
        const bool advanceAdmission = steps || resetThisFrame || c.emission.y;
        if (advanceAdmission || validateBulkThisFrame) {
            bulk->allocateSources(cmd, advanceAdmission);
            bind(cmd);
        }
    }
    collidersDirty = false;
    for (uint32_t i = 0; i < steps; ++i) {
        if (colliderTimeline.moving) {
            gpu::Event event(cmd, L"Fluid / substep rigid SDF boundary");
            colliderOffset = (i + 1) * FluidColliderTimeline::sliceBytes;
            bind(cmd);
            cmd->SetPipelineState(bakeSolids.Get());
            cmd->Dispatch(scanBlockCount, 1, 1);
            gpu::uav(cmd, solidGrid.resource.Get());
        }
        if (desc.ballisticTest) {
            cmd->SetPipelineState(integrate.Get());
            cmd->Dispatch((desc.maxParticles + 255) / 256, 1, 1);
            gpu::uav(cmd, particles.resource.Get());
        } else {
            // The previous substep's repair finishes with exact current bins.
            if (cutCells) {
                cutCells->record(
                    cmd, gpuView(),
                    {colliderTimeline.slices.data() + colliderOffset / sizeof(FluidCollider), colliderCount},
                    meshAsset, stepCount + 1);
                if (desc.bulkProjected)
                    bulk->setProjection(cutCells->gpuView(), mac->projectedVolumeFlux());
                bind(cmd);
            }
            // Collider motion only changes the SDF, not particle membership.
            transferToGrid(cmd, i != 0 || sourceBinsCurrent);
            // Capacity transport belongs to an actual simulation substep, not
            // the display-only grid rebuild used for paused edits and reset.
            if (desc.bulkCoupled) {
                auto corrected = bulk->projectCapacity(cmd, cellData.resource.Get(),
                                                       mac->constraintView(faces.resource.Get()));
                mac->applyCapacity(cmd, uniforms.resource.Get(), faces.resource.Get(),
                                   cellData.resource.Get(), corrected);
                bulk->auditCapacityApplication(cmd, faces.resource.Get(), mac->projectedVolumeFlux());
                bind(cmd);
            }
            transferToParticles(cmd);
            if (interior) {
                interior->advect(cmd, gpuView(), uniforms.resource.Get(), solidGrid.resource.Get());
                bind(cmd);
            }
            if (bulk) {
                bulk->advance(cmd, gpuView(), solidGrid.resource.Get(), cellData.resource.Get());
                bind(cmd);
            }
            if (colliderCount) {
                // Density must see the feasible post-advection distribution,
                // not particles still inside the newly moved SDF. Otherwise its
                // solve ignores the mass that the final contact pass squeezes
                // into a neighboring liquid cell.
                gpu::Event event(cmd, L"Fluid / SDF contact before density");
                cmd->SetPipelineState(collide.Get());
                cmd->Dispatch((desc.maxParticles + 127) / 128, 1, 1);
                gpu::uav(cmd, particles.resource.Get());
            }
            correctDensity(cmd);
            if (colliderCount) {
                gpu::Event event(cmd, L"Fluid / SDF contact after density");
                cmd->SetPipelineState(collide.Get());
                cmd->Dispatch((desc.maxParticles + 127) / 128, 1, 1);
                gpu::uav(cmd);
            }
            // Bins finish at corrected/collided positions, ready for the next
            // substep or renderer. No readback is used to decide on repair.
            for (uint32_t repair = 0; repair < (desc.cutPressure ? 4u : 1u); ++repair)
                repairDensity(cmd, repair != 0);
        }
        if (exchange) {
            // Includes G2P, boundary impulses and all contact/density repairs.
            // Import increments, never re-quantize absolute momentum from FP32.
            exchange->velocityDelta(cmd, exchangeView());
            bind(cmd);
        }
        ++stepCount;
    }
    // Surface reconstruction/importance/ownership use the exact render endpoint.
    colliderOffset = 0;
    bind(cmd);
    if ((desc.ballisticTest && steps) || (!steps && binNeeded)) {
        transferToGrid(cmd);
        if (desc.transferTest == 4)
            transferToParticles(cmd);
    }
    if (bulk)
        bulk->finishFrame(cmd);
    if (bulkPressure)
        bulkPressure->finishFrame(cmd);
    if (cutCells) {
        cutCells->finishFrame(cmd, bulk ? bulk->gpuView().inventory : nullptr);
        bind(cmd);
    }
    if (resampling) {
        if (interior) {
            interior->restore(cmd, gpuView(), uniforms.resource.Get(), solidGrid.resource.Get(), true);
            bind(cmd);
            bin(cmd, interior->binArguments());
            interior->deposit(cmd, gpuView(), uniforms.resource.Get(), solidGrid.resource.Get());
            bind(cmd);
            bin(cmd, interior->binArguments());
            changedThisFrame = changedThisFrame || interior->changed;
        }
        resampling->forcedFine = interior && interior->forcedFine;
        resampling->record(cmd, gpuView(), uniforms.resource.Get(), solidGrid.resource.Get(), activeParticles,
                           resetThisFrame, steps != 0, validateResamplingThisFrame);
        bind(cmd);
        if (resampling->changed) {
            bin(cmd, resampling->binArguments());
            changedThisFrame = true;
        }
    }
    if (exchange && !steps) {
        exchange->velocityDelta(cmd, exchangeView());
        bind(cmd);
    }
    cmd->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
}
void FluidSystem::drawDebug(ID3D12GraphicsCommandList *cmd, D3D12_GPU_DESCRIPTOR_HANDLE depth) {
    if (cutCells)
        cutCells->drawDebug(cmd, static_cast<const Uniforms *>(uniforms.mapped)->viewProjection);
    if (mac)
        mac->drawDebug(cmd, uniforms.resource.Get());
    if (!debugVisible)
        return;
    gpu::Event event(cmd, L"Fluid / DEBUG particles (not liquid surface)");
    cmd->SetGraphicsRootSignature(root.Get());
    cmd->SetPipelineState(debug.Get());
    cmd->SetGraphicsRootConstantBufferView(0, uniforms.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(1, particles.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(8, faces.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(11, cellData.resource->GetGPUVirtualAddress());
    cmd->SetGraphicsRootUnorderedAccessView(
        23, (work ? work->data() : particles.resource.Get())->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(2, depth);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, debugMode && debugMode != 5 ? grid.w : desc.maxParticles, 0, 0);
}
void FluidSystem::recordTimings(ID3D12GraphicsCommandList *cmd) {
    cmd->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, timingReadback.resource.Get(), 0);
    if (pressureSolver)
        pressureSolver->recordReadback(cmd);
    if (mac)
        mac->recordReadback(cmd);
    if (resampling)
        resampling->recordReadback(cmd);
    if (interior)
        interior->recordReadback(cmd);
    if (work)
        work->recordReadback(cmd);
}
void FluidSystem::collectTimings(uint64_t frequency) {
#if PT_FLUID_CUDA
    if (cuda)
        cuda->collect();
#endif
    void *data;
    D3D12_RANGE range{0, 16};
    gpu::check(timingReadback.resource->Map(0, &range, &data), "Fluid timing map");
    auto t = static_cast<const uint64_t *>(data);
    simulationMs = double(t[1] - t[0]) * 1000 / frequency;
    D3D12_RANGE written{0, 0};
    timingReadback.resource->Unmap(0, &written);
    if (pressureSolver)
        pressureSolver->collect(frequency);
    if (mac)
        mac->collect(frequency);
    if (bulk)
        bulk->collect(frequency);
    if (bulkPressure)
        bulkPressure->collect(frequency);
    if (cutCells)
        cutCells->collect(frequency);
    if (interior)
        interior->collect(frequency);
    if (resampling)
        resampling->collect(frequency);
    if (work)
        work->collect(frequency);
}
void FluidSystem::recordValidationReadback(ID3D12GraphicsCommandList *cmd) {
    {
        gpu::Event event(cmd, L"Fluid / validation-only post-step density");
        bind(cmd);
        cmd->SetPipelineState(densityMeasure.Get());
        if (desc.narrowBand)
            cmd->SetComputeRootUnorderedAccessView(30, gridOwnedResource()->GetGPUVirtualAddress());
        cmd->Dispatch((grid.w + 127) / 128, 1, 1);
        gpu::uav(cmd, densityData.resource.Get());
    }
    if (!readback.resource) {
        ComPtr<ID3D12Device> device;
        gpu::check(particles.resource->GetDevice(IID_PPV_ARGS(&device)), "Fluid device");
        uint64_t bytes =
            particles.resource->GetDesc().Width + cellCounts.resource->GetDesc().Width +
            cellOffsets.resource->GetDesc().Width + sortedIndices.resource->GetDesc().Width +
            faces.resource->GetDesc().Width + cellData.resource->GetDesc().Width +
            densityData.resource->GetDesc().Width + materialData.resource->GetDesc().Width +
            densityArguments.resource->GetDesc().Width + solidGrid.resource->GetDesc().Width +
            cellQuanta.resource->GetDesc().Width + (interior ? interior->state()->GetDesc().Width : 0) +
            (exchange
                 ? exchange->particleQuantities()->GetDesc().Width + exchange->counters()->GetDesc().Width
                 : 0) +
            (desc.narrowBand ? gridOwnedResource()->GetDesc().Width : 0);
        readback = gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                               D3D12_RESOURCE_STATE_COPY_DEST, L"Fluid test-only particles");
    }
    uint64_t offset = 0;
    for (auto r :
         {particles.resource.Get(), cellCounts.resource.Get(), cellOffsets.resource.Get(),
          sortedIndices.resource.Get(), faces.resource.Get(), cellData.resource.Get(),
          densityData.resource.Get(), materialData.resource.Get(), densityArguments.resource.Get(),
          solidGrid.resource.Get(), cellQuanta.resource.Get(), interior ? interior->state() : nullptr,
          exchange ? exchange->particleQuantities() : nullptr, exchange ? exchange->counters() : nullptr,
          gridOwnedResource()}) {
        if (!r)
            continue;
        gpu::transition(cmd, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(readback.resource.Get(), offset, r, 0, r->GetDesc().Width);
        offset += r->GetDesc().Width;
        gpu::transition(cmd, r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    readbackReady = true;
}
void FluidSystem::validateAndReport(std::ostream &out) {
    out << "{\"milestone\":12,\"transfer\":\""
        << (desc.ballisticTest ? "ballistic-test" : (desc.transfer == FluidTransfer::Flip ? "FLIP" : "APIC"))
        << "\",\"particles\":" << activeParticles << ",\"initialParticles\":" << desc.initialParticles
        << ",\"capacity\":" << desc.maxParticles << ",\"roomPool\":" << (desc.roomPool ? "true" : "false")
        << ",\"emittedParticles\":" << emittedParticles << ",\"particleVolume\":" << particleVolume
        << ",\"cellSize\":" << desc.gridCellSize << ",\"initialDepth\":" << desc.initialDepth
        << ",\"simulationHz\":" << desc.simulationRate << ",\"gridDimensions\":[" << grid.x << ',' << grid.y
        << ',' << grid.z << ']' << ",\"domainMinimum\":[" << desc.minimum.x << ',' << desc.minimum.y << ','
        << desc.minimum.z << ']' << ",\"domainMaximum\":[" << desc.maximum.x << ',' << desc.maximum.y << ','
        << desc.maximum.z << ']' << ",\"emitterFull\":" << (emitterFull ? "true" : "false")
        << ",\"steps\":" << stepCount << ",\"lastSimulationMs\":" << simulationMs
        << ",\"droppedSeconds\":" << droppedSeconds << ",\"density\":" << desc.density
        << ",\"kinematicViscosity\":" << desc.viscosity
        << ",\"deterministicBins\":" << (desc.deterministicBins ? "true" : "false")
        << ",\"ownedParticles\":" << (desc.ownedParticles ? "true" : "false")
        << ",\"debugVisible\":" << (debugVisible ? "true" : "false") << ",\"debugMode\":" << debugMode
        << ",\"surfaceTension\":" << desc.surfaceTension << ",\"viscositySubcycles\":" << viscositySubsteps;
    out << ",\"backend\":\"" << (desc.cudaBackend ? "cuda" : "dx12") << '"';
#if PT_FLUID_CUDA
    if (cuda) {
        out << ",\"cuda\":";
        cuda->report(out);
    }
#endif
    if (pressureSolver) {
        out << ",\"pressureSolver\":";
        pressureSolver->report(out);
    }
    if (mac) {
        out << ",\"adaptiveMac\":";
        mac->report(out);
    }
    if (work) {
        out << ",\"work\":";
        work->report(out);
    }
    if (bulk) {
        out << ",\"bulk\":";
        bulk->report(out);
    }
    if (bulkPressure) {
        out << ",\"bulkPressure\":";
        bulkPressure->report(out);
    }
    if (cutCells) {
        out << ",\"cutCells\":";
        cutCells->report(out);
    }
    if (resampling) {
        out << ",\"resampling\":";
        resampling->report(out);
    }
    if (interior) {
        out << ",\"interior\":";
        interior->report(out);
    }
    if (readbackReady) {
        void *data;
        D3D12_RANGE range{0, size_t(readback.resource->GetDesc().Width)};
        gpu::check(readback.resource->Map(0, &range, &data), "Fluid validation readback");
        auto p = static_cast<const FluidParticle *>(data);
        const uint64_t gridBytes = desc.narrowBand ? gridOwnedResource()->GetDesc().Width : 0;
        const uint64_t authorityBytes =
            gridBytes + (exchange ? exchange->particleQuantities()->GetDesc().Width +
                                        exchange->counters()->GetDesc().Width
                                  : 0);
        // Previous grid/cache sections need only four-byte alignment. Copy the
        // optional FP64 audit into aligned storage instead of casting that tail.
        std::vector<std::array<double, 4>> authority;
        if (exchange) {
            authority.resize(desc.maxParticles);
            memcpy(authority.data(),
                   static_cast<const char *>(data) + readback.resource->GetDesc().Width - authorityBytes,
                   authority.size() * sizeof(authority[0]));
        }
        const auto *quantities = authority.data();
        const auto *exchangeCounters =
            exchange ? reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) +
                                                          readback.resource->GetDesc().Width -
                                                          exchange->counters()->GetDesc().Width - gridBytes)
                     : nullptr;
        std::vector<std::array<double, 4>> gridAuthority(gridBytes / 32);
        if (gridBytes)
            memcpy(gridAuthority.data(),
                   static_cast<const char *>(data) + readback.resource->GetDesc().Width - gridBytes,
                   gridBytes);
        double authoritativeVolume = 0, massCacheError = 0, velocityCacheError = 0;
        uint32_t active = 0;
        std::array<uint32_t, 4> roomQuadrants{};
        double meanY = 0, restMass = 0, maxSpeed = 0, maxGravityError = 0, maxSolidPenetration = 0;
        double maxBinPositionError = 0;
        uint32_t penetrationParticle = 0, penetrationCollider = 0;
        std::string failure;
        double gridVolume = 0, gridMeanY = 0;
        uint32_t gridOwners = 0;
        for (uint32_t i = 0; i < gridAuthority.size(); ++i) {
            const auto &q = gridAuthority[i];
            const uint32_t nx = (grid.x + 1) / 2, ny = (grid.y + 1) / 2;
            const uint32_t xyz[]{2 * (i % nx), 2 * ((i / nx) % ny), 2 * (i / (nx * ny))};
            const uint32_t children =
                std::min(2u, grid.x - xyz[0]) * std::min(2u, grid.y - xyz[1]) * std::min(2u, grid.z - xyz[2]);
            const double h = desc.gridCellSize, capacity = children * h * h * h;
            for (double v : q)
                if (!std::isfinite(v))
                    failure = "Nonfinite grid-owned quantity";
            if (q[3] < 0 || q[3] > capacity * (1 + 2e-13) || (q[3] == 0 && q != std::array<double, 4>{}))
                failure = "Invalid narrow-band grid inventory";
            if (q[3] > 0)
                ++gridOwners;
            gridVolume += q[3];
            gridMeanY += q[3] * (desc.minimum.y + (xyz[1] + .5 * std::min(2u, grid.y - xyz[1])) * h);
        }
        authoritativeVolume = gridVolume;
        for (uint32_t i = 0; i < desc.maxParticles; ++i) {
            auto f = reinterpret_cast<const float *>(&p[i]);
            for (int j = 0; j < 20; ++j)
                if (!std::isfinite(f[j]))
                    failure = "Nonfinite fluid state";
            if (exchange) {
                const auto &q = quantities[i];
                for (double x : q)
                    if (!std::isfinite(x))
                        failure = "Nonfinite authoritative particle quantity";
                if ((q[3] > 0) != (p[i].velocityFlags.w != 0) || q[3] < 0)
                    failure = "Particle cache/authority liveness mismatch";
                if (!p[i].velocityFlags.w && q != std::array<double, 4>{})
                    failure = "Dead particle retained authoritative quantity";
                authoritativeVolume += q[3];
                if (q[3] > 0) {
                    massCacheError =
                        std::max(massCacheError, std::abs(double(p[i].apic0.w) * particleVolume / q[3] - 1));
                    for (uint32_t a = 0; a < 3; a++) {
                        const double v = q[a] / q[3];
                        velocityCacheError =
                            std::max(velocityCacheError,
                                     std::abs((&p[i].velocityFlags.x)[a] - v) / std::max(1., std::abs(v)));
                    }
                }
            }
            if (p[i].velocityFlags.w == 0)
                continue;
            ++active;
            const double weight = exchange ? quantities[i][3] / particleVolume : p[i].apic0.w;
            if (exchange ? weight <= 0
                         : (weight < 1.0 / 16 || weight > 8 || weight * 16 != std::round(weight * 16)))
                failure = "Invalid adaptive particle mass";
            restMass += weight;
            if (desc.roomPool) {
                uint32_t quadrant = p[i].positionRadius.x > (desc.minimum.x + desc.maximum.x) * .5f ? 1 : 0;
                quadrant |= p[i].positionRadius.z > (desc.minimum.z + desc.maximum.z) * .5f ? 2 : 0;
                ++roomQuadrants[quadrant];
            }
            for (uint32_t k = 0; k < colliderCount; ++k) {
                const auto &collider = static_cast<const FluidCollider *>(colliderData.mapped)[k];
                XMFLOAT3 local;
                XMStoreFloat3(&local, XMVector3TransformCoord(XMLoadFloat3(reinterpret_cast<const XMFLOAT3 *>(
                                                                  &p[i].positionRadius)),
                                                              XMLoadFloat4x4(&collider.worldToLocal)));
                const auto e = collider.extentType;
                float d = 1e6f;
                if (e.w == 0)
                    d = std::sqrt(local.x * local.x + local.y * local.y + local.z * local.z) - e.x;
                if (e.w == 1) {
                    XMFLOAT3 q{std::abs(local.x) - e.x, std::abs(local.y) - e.y, std::abs(local.z) - e.z};
                    d = float(std::sqrt(std::pow(std::max(q.x, 0.f), 2) + std::pow(std::max(q.y, 0.f), 2) +
                                        std::pow(std::max(q.z, 0.f), 2)) +
                              std::min(std::max({q.x, q.y, q.z}), 0.f));
                }
                if (e.w == 5)
                    d = meshAsset.sample(local);
                if (p[i].positionRadius.w - d > maxSolidPenetration) {
                    maxSolidPenetration = p[i].positionRadius.w - d;
                    penetrationParticle = i;
                    penetrationCollider = k;
                }
            }
            meanY += p[i].positionRadius.y * weight;
            if (stepCount <= 16 && !desc.transferTest && desc.ballisticTest) {
                uint32_t n = uint32_t(std::ceil(std::cbrt(double(desc.initialParticles))));
                double y = (double((i / n) % n) + .5) / n;
                double initialY = desc.minimum.y + (desc.maximum.y - desc.minimum.y) * (.48 + .42 * y);
                double time = stepCount / double(desc.simulationRate);
                maxGravityError = std::max(maxGravityError, std::abs(p[i].positionRadius.y - initialY -
                                                                     .5 * desc.gravity.y * time * time));
                maxGravityError =
                    std::max(maxGravityError, std::abs(p[i].velocityFlags.y - desc.gravity.y * time));
            }
            maxSpeed = std::max(maxSpeed, std::sqrt(double(p[i].velocityFlags.x) * p[i].velocityFlags.x +
                                                    double(p[i].velocityFlags.y) * p[i].velocityFlags.y +
                                                    double(p[i].velocityFlags.z) * p[i].velocityFlags.z));
            for (int j = 0; j < 3; ++j)
                if ((&p[i].positionRadius.x)[j] < (&desc.minimum.x)[j] + desc.particleRadius - .0001f ||
                    (&p[i].positionRadius.x)[j] > (&desc.maximum.x)[j] - desc.particleRadius + .0001f)
                    failure = "Fluid escaped bounds";
        }
        auto count = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) +
                                                        particles.resource->GetDesc().Width);
        auto offsets = reinterpret_cast<const uint32_t *>(reinterpret_cast<const char *>(count) +
                                                          cellCounts.resource->GetDesc().Width);
        auto sorted = reinterpret_cast<const uint32_t *>(reinterpret_cast<const char *>(offsets) +
                                                         cellOffsets.resource->GetDesc().Width);
        std::vector<bool> seen(desc.maxParticles, false);
        const auto *quanta = reinterpret_cast<const uint32_t *>(
            static_cast<const char *>(data) + readback.resource->GetDesc().Width -
            cellQuanta.resource->GetDesc().Width - (interior ? interior->state()->GetDesc().Width : 0) -
            authorityBytes);
        const auto *owned = reinterpret_cast<const FluidParticle *>(reinterpret_cast<const char *>(quanta) +
                                                                    cellQuanta.resource->GetDesc().Width);
        activeCells = maxCellOccupancy = 0;
        if (offsets[0] != 0 || offsets[grid.w] != active)
            failure = "GPU prefix scan total mismatch";
        for (uint32_t cell = 0; cell < grid.w; ++cell) {
            if (count[cell])
                ++activeCells;
            maxCellOccupancy = std::max(maxCellOccupancy, count[cell]);
            if (offsets[cell + 1] < offsets[cell] || offsets[cell + 1] - offsets[cell] != count[cell] ||
                offsets[cell + 1] > active) {
                failure = "Invalid GPU cell range";
                break;
            }
            uint32_t expectedQuanta = 0;
            double expectedVolume = 0;
            if (interior) {
                uint32_t nx = (grid.x + 1) / 2, ny = (grid.y + 1) / 2;
                uint32_t owner =
                    (((cell / (grid.x * grid.y)) / 2) * ny + ((cell / grid.x) % grid.y) / 2) * nx +
                    (cell % grid.x) / 2;
                if (owned[owner].velocityFlags.w) {
                    const auto &o = owned[owner];
                    const uint32_t xyz[] = {cell % grid.x, (cell / grid.x) % grid.y,
                                            cell / (grid.x * grid.y)};
                    const float spacing[] = {o.positionRadius.w, o.apic1.w, o.apic2.w};
                    expectedQuanta = 16;
                    for (uint32_t a = 0; a < 3; ++a) {
                        const uint32_t extent = (uint32_t(o.velocityFlags.w) >> (5 * a)) & 31;
                        const double first = (&o.positionRadius.x)[a] - .5 * (extent - 1) * spacing[a];
                        const double split =
                            (&desc.minimum.x)[a] + double((xyz[a] / 2) * 2 + 1) * desc.gridCellSize;
                        const uint32_t low = uint32_t(
                            std::clamp(std::ceil((split - first) / spacing[a]), 0.0, double(extent)));
                        expectedQuanta *= (xyz[a] & 1) ? extent - low : low;
                    }
                }
            }
            for (uint32_t j = offsets[cell]; j < offsets[cell + 1]; ++j) {
                uint32_t id = sorted[j];
                if (desc.deterministicBins && j > offsets[cell] && sorted[j - 1] >= id)
                    failure = "Non-deterministic particle cell ordering";
                if (id >= desc.maxParticles || seen[id]) {
                    failure = "Duplicate/out-of-range sorted particle";
                    break;
                }
                seen[id] = true;
                if (!exchange)
                    expectedQuanta += uint32_t(p[id].apic0.w * 16);
                if (exchange)
                    expectedVolume += quantities[id][3];
                uint32_t xyz[3] = {cell % grid.x, (cell / grid.x) % grid.y, cell / (grid.x * grid.y)};
                // Reciprocal/FMA binning can straddle an exact boundary. Bound
                // float subtraction/multiply rounding in WORLD units; a fixed
                // 2um tolerance is smaller than an ULP in the enlarged room.
                for (int axis = 0; axis < 3; ++axis) {
                    double low = (&desc.minimum.x)[axis] + double(xyz[axis]) * desc.gridCellSize;
                    double value = (&p[id].positionRadius.x)[axis];
                    const double tolerance =
                        std::max(.000002, 4 * double(std::numeric_limits<float>::epsilon()) *
                                              (std::abs(value) + std::abs((&desc.minimum.x)[axis]) +
                                               desc.gridCellSize));
                    maxBinPositionError = std::max(
                        maxBinPositionError, std::max({low - value, value - low - desc.gridCellSize, 0.}));
                    if (value < low - tolerance || value > low + desc.gridCellSize + tolerance)
                        failure = "Particle binned into incorrect cell";
                }
            }
            if (desc.adaptiveParticles && expectedQuanta != quanta[cell])
                failure = "Adaptive cell rest-mass quanta mismatch";
            if (exchange) {
                float cached;
                memcpy(&cached, &quanta[cell], sizeof(cached));
                if (desc.narrowBand) {
                    const uint32_t x = cell % grid.x, y = (cell / grid.x) % grid.y,
                                   z = cell / (grid.x * grid.y);
                    const uint32_t owner =
                        ((z / 2) * ((grid.y + 1) / 2) + y / 2) * ((grid.x + 1) / 2) + x / 2;
                    const uint32_t children = std::min(2u, grid.x - (x / 2) * 2) *
                                              std::min(2u, grid.y - (y / 2) * 2) *
                                              std::min(2u, grid.z - (z / 2) * 2);
                    expectedVolume += gridAuthority[owner][3] / children;
                }
                const double expected = expectedVolume / particleVolume;
                if (!std::isfinite(cached) || std::abs(cached - expected) > std::max(1e-12, expected * 2e-7))
                    failure = "Fractional cell mass disagrees with authoritative bin gather";
            }
        }
        for (uint32_t i = 0; i < desc.maxParticles; ++i)
            if (seen[i] != (p[i].velocityFlags.w != 0))
                failure = "Particle missing from GPU bins";
        auto face = reinterpret_cast<const XMFLOAT4 *>(reinterpret_cast<const char *>(sorted) +
                                                       sortedIndices.resource->GetDesc().Width);
        double maxTransferError = 0, maxAffineError = 0;
        std::array<double, 3> weights{};
        const float base[3] = {.7f, .2f, -.3f};
        const float affine[3][3] = {{.2f, -.4f, .1f}, {.4f, -.1f, .2f}, {0, -.2f, -.1f}};
        for (uint32_t id = 0; id < faceStride * 3; ++id) {
            auto f = face[id];
            if (!std::isfinite(f.x) || !std::isfinite(f.y) || !std::isfinite(f.z) || !std::isfinite(f.w) ||
                f.z < 0)
                failure = "Invalid MAC P2G velocity/weight";
            uint32_t axis = id / faceStride, k = id % faceStride;
            weights[axis] += f.z;
            if ((desc.transferTest != 1 && desc.transferTest != 2 && desc.transferTest != 4) || f.z < .00001f)
                continue;
            uint32_t coord[3] = {k % (grid.x + 1), (k / (grid.x + 1)) % (grid.y + 1),
                                 k / ((grid.x + 1) * (grid.y + 1))};
            double expected = base[axis];
            if (desc.transferTest == 2 || desc.transferTest == 4)
                for (int j = 0; j < 3; ++j) {
                    double position = (&desc.minimum.x)[j] +
                                      (coord[j] + (uint32_t(j) == axis ? 0 : .5)) * desc.gridCellSize;
                    expected +=
                        affine[axis][j] * (position - ((&desc.minimum.x)[j] + (&desc.maximum.x)[j]) * .5);
                }
            maxTransferError = std::max(maxTransferError, std::abs(f.x - expected));
        }
        if (desc.transferTest == 4) {
            for (uint32_t i = 0; i < desc.initialParticles; ++i)
                for (int a = 0; a < 3; ++a) {
                    double expected = base[a];
                    for (int j = 0; j < 3; ++j)
                        expected += affine[a][j] * ((&p[i].positionRadius.x)[j] -
                                                    ((&desc.minimum.x)[j] + (&desc.maximum.x)[j]) * .5);
                    maxTransferError =
                        std::max(maxTransferError, std::abs((&p[i].velocityFlags.x)[a] - expected));
                    const XMFLOAT4 rows[] = {p[i].apic0, p[i].apic1, p[i].apic2};
                    for (int j = 0; j < 3; ++j)
                        maxAffineError =
                            std::max(maxAffineError, std::abs(double((&rows[a].x)[j]) - affine[a][j]));
                }
        }
        if (desc.transferTest == 1 || desc.transferTest == 2 || desc.transferTest == 4) {
            // Velocity (m/s) and its affine derivative (1/s) have different
            // error scales. The derivative amplifies float transfer error by 1/h.
            if (maxAffineError > .00005)
                failure = "APIC affine roundtrip failed: " + std::to_string(maxAffineError);
            if (maxTransferError > .00001)
                failure = "MAC APIC constant/affine reproduction failed: max error " +
                          std::to_string(maxTransferError);
            for (auto weight : weights)
                if (std::abs(weight - active) > std::max(1.0, active * .00002))
                    failure = "Quadratic MAC weights lost mass";
        }
        auto cells = reinterpret_cast<const XMFLOAT4 *>(reinterpret_cast<const char *>(face) +
                                                        faces.resource->GetDesc().Width);
        const auto density = reinterpret_cast<const XMFLOAT4 *>(reinterpret_cast<const char *>(cells) +
                                                                cellData.resource->GetDesc().Width);
        const auto material = reinterpret_cast<const XMFLOAT4 *>(reinterpret_cast<const char *>(density) +
                                                                 densityData.resource->GetDesc().Width);
        const auto repairArgs = reinterpret_cast<const uint32_t *>(reinterpret_cast<const char *>(material) +
                                                                   materialData.resource->GetDesc().Width);
        const auto solids = reinterpret_cast<const XMFLOAT4 *>(reinterpret_cast<const char *>(repairArgs) +
                                                               densityArguments.resource->GetDesc().Width);
        if (advancedSeconds > 0 && !desc.ballisticTest && !desc.transferTest && desc.densityIterations) {
            const uint32_t groups[] = {scanBlockCount, (desc.maxParticles + 255) / 256,
                                       scanBlockCount, 1,
                                       scanBlockCount, (desc.maxParticles + 255) / 256,
                                       scanBlockCount, (desc.maxParticles + 127) / 128};
            if (repairArgs[24] > 1)
                failure = "Invalid GPU density repair request";
            if (repairArgs[25] > (desc.cutPressure ? 4u : 1u))
                failure = "GPU density repair exceeded the nonlinear correction budget";
            for (uint32_t i = 0; i < 8; ++i)
                if (repairArgs[3 * i] != (repairArgs[24] ? groups[i] : 0) || repairArgs[3 * i + 1] != 1 ||
                    repairArgs[3 * i + 2] != 1)
                    failure = "Invalid density repair/rebin indirect arguments";
            const uint32_t tiles = ((grid.x + 7) / 8) * ((grid.y + 3) / 4) * ((grid.z + 3) / 4);
            if (repairArgs[30] != (repairArgs[24] ? std::min(tiles, 65535u) : 0) ||
                repairArgs[31] != (tiles + 65534) / 65535 || repairArgs[32] != 1)
                failure = "Invalid paired density repair dispatch";
        }
        const double sigma =
            desc.ballisticTest || desc.transferTest || desc.materialTest ? 0 : desc.surfaceTension;
        double maxCurvature = 0, maxCurvatureError = 0, maxViscosityError = 0;
        uint32_t curvatureSamples = 0, viscositySamples = 0;
        for (uint32_t id = 0; id < grid.w; ++id) {
            const auto m = material[id];
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite((&m.x)[j]))
                    failure = "Nonfinite capillary field";
            maxCurvature = std::max(maxCurvature, std::abs(double(m.y)));
            if (m.x < 0 || m.x > 1.00001f || maxCurvature > 2 / desc.gridCellSize + .001)
                failure = "Invalid capillary colour/curvature bound";
            if (desc.materialTest == 2 && m.x > .45f && m.x < .55f) {
                const int xyz[3] = {int(id % grid.x), int((id / grid.x) % grid.y),
                                    int(id / (grid.x * grid.y))};
                double r2 = 0;
                for (int a = 0; a < 3; ++a) {
                    const double q = (&desc.minimum.x)[a] + (xyz[a] + .5) * desc.gridCellSize -
                                     ((&desc.minimum.x)[a] + (&desc.maximum.x)[a]) * .5;
                    r2 += q * q;
                }
                maxCurvatureError = std::max(maxCurvatureError, std::abs(m.y * std::sqrt(r2) / 2 - 1));
                ++curvatureSamples;
            }
        }
        if (desc.materialTest == 1) {
            double e0 = 0, e1 = 0;
            const double alpha = double(desc.viscosity) / (desc.simulationRate * desc.gridCellSize *
                                                           desc.gridCellSize * viscositySubsteps);
            const double attenuation = std::pow(1 - 4 * alpha * std::pow(std::sin(.2), 2), viscositySubsteps);
            for (uint32_t id = 0; id < faceStride * 3; ++id) {
                e0 += double(face[id].y) * face[id].y;
                e1 += double(face[id].x) * face[id].x;
            }
            const uint32_t margin = viscositySubsteps + 2;
            for (uint32_t z = margin; z + margin < grid.z; ++z)
                for (uint32_t y = margin; y + margin < grid.y; ++y)
                    for (uint32_t x = margin; x + margin < grid.x; ++x) {
                        const auto f = face[(z * (grid.y + 1) + y) * (grid.x + 1) + x];
                        const double original = std::sin((z + .5) * .4);
                        maxViscosityError =
                            std::max({maxViscosityError, std::abs(f.x - original * attenuation),
                                      std::abs(f.y - original)});
                        ++viscositySamples;
                    }
            if (!viscositySamples || maxViscosityError > 2e-6 || e1 > e0 * 1.000001)
                failure = "GPU viscosity Fourier decay/FLIP preservation/energy check failed";
        }
        if (desc.materialTest == 2 && (!curvatureSamples || maxCurvatureError > .04))
            failure = "GPU outward sphere curvature disagrees with 2/r: " + std::to_string(maxCurvatureError);
        double before2 = 0, after2 = 0, maxDivergence = 0, maxResidualMismatch = 0;
        double maxCudaFluxMismatch = 0, maxCudaFluxDivergence = 0;
        uint32_t projectedCells = 0;
        for (uint32_t id = 0; id < grid.w; ++id) {
            auto c = cells[id];
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite((&c.x)[j]))
                    failure = "Nonfinite pressure/divergence";
            if ((desc.ballisticTest || desc.transferTest) && c.z != (count[id] ? 1.f : 0.f))
                failure = "Incorrect liquid classification";
            if (interior && quanta[id] && !count[id] && (c.z != 1 || density[id].z != 1))
                failure = "Particle-free interior lost global pressure/density coupling";
            if (c.z != 1)
                continue;
            ++projectedCells;
            before2 += double(c.x) * c.x;
            after2 += double(c.y) * c.y;
            maxDivergence = std::max(maxDivergence, std::abs(double(c.y)));
            if (desc.cudaMixedPressure) {
                // The mixed operator is not the uniform six-point Laplacian.
                // Independently audit the actual published fine-face flux here;
                // operator/Galerkin identity has dedicated CUDA MAC fixtures.
                const uint32_t xyz[] = {id % grid.x, (id / grid.x) % grid.y, id / (grid.x * grid.y)};
                double fluxDivergence = 0;
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    uint32_t q[] = {xyz[0], xyz[1], xyz[2]};
                    const auto index = [&] {
                        return axis * faceStride + (q[2] * (grid.y + 1) + q[1]) * (grid.x + 1) + q[0];
                    };
                    const double a = face[index()].x;
                    ++q[axis];
                    fluxDivergence += double(face[index()].x) - a;
                }
                fluxDivergence /= desc.gridCellSize;
                maxCudaFluxDivergence = std::max(maxCudaFluxDivergence, std::abs(fluxDivergence));
                maxCudaFluxMismatch = std::max(maxCudaFluxMismatch, std::abs(fluxDivergence - c.y));
                continue;
            }
            // Mixed-grid residual is audited at projection time against an
            // independently assembled face operator, before scratch reuse.
            if (mac)
                continue;
            int xyz[3] = {int(id % grid.x), int((id / grid.x) % grid.y), int(id / (grid.x * grid.y))};
            double laplacian = 0;
            for (int axis = 0; axis < 3; ++axis)
                for (int side : {-1, 1}) {
                    int n[3] = {xyz[0], xyz[1], xyz[2]};
                    n[axis] += side;
                    if (n[axis] < 0 || n[axis] >= int((&grid.x)[axis]))
                        continue;
                    const uint32_t ni = (n[2] * grid.y + n[1]) * grid.x + n[0];
                    auto neighbor = cells[ni];
                    double pn = neighbor.w;
                    if (sigma > 0 && neighbor.z == 0) {
                        const auto a = material[id], b = material[ni];
                        const double t = std::abs(a.x - b.x) > 1e-6
                                             ? std::clamp((double(a.x) - .5) / (a.x - b.x), 0., 1.)
                                             : .5;
                        pn = sigma * (a.y + (b.y - a.y) * t);
                    }
                    if (neighbor.z != 2)
                        laplacian += pn - c.w;
                }
            double expected = c.x - laplacian / (double(desc.density) * desc.gridCellSize *
                                                 desc.gridCellSize * desc.simulationRate);
            maxResidualMismatch = std::max(maxResidualMismatch, std::abs(expected - c.y));
        }
        if (mac) {
            maxResidualMismatch = mac->residualMismatch;
            if (!mac->validated)
                failure = "Missing independent mixed-MAC operator validation";
        }
        if (maxResidualMismatch > (mac ? .0002 : .0001))
            failure = "Pressure residual disagrees with projected MAC divergence";
        if (desc.cudaMixedPressure && (maxCudaFluxDivergence > .000101 || maxCudaFluxMismatch > .00001))
            failure = "Published CUDA fine-face flux failed independent divergence audit";
        // Surface tension adds an inhomogeneous pressure boundary; the residual
        // identity remains mandatory, but zero initial divergence need not stay
        // zero after a finite-iteration solve with nonzero capillary pressure.
        if (sigma == 0 && after2 > before2 * 1.001 + 1e-8)
            failure = "Pressure projection increased RMS divergence";
        if (desc.transferTest == 3 && (before2 < 1 || after2 > before2 * .25))
            failure = "Compression fixture did not reduce divergence by 50 percent";
        double occupiedVolume = 0, maxDensity = 0, currentVolume = 0, currentDensity = 0,
               currentFluidDensity = 0;
        uint32_t peakCell = 0, currentPeakCell = 0;
        for (uint32_t id = 0; id < grid.w; ++id) {
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite((&density[id].x)[j]))
                    failure = "Nonfinite particle density";
            if (density[id].x < 0 || density[id].w < 0)
                failure = "Invalid particle density";
            if (density[id].x > maxDensity) {
                maxDensity = density[id].x;
                peakCell = id;
            }
            if (density[id].w > currentDensity) {
                currentDensity = density[id].w;
                currentPeakCell = id;
            }
            if (solids[id].x >= 0)
                currentFluidDensity = std::max(currentFluidDensity, double(density[id].w));
            currentVolume += std::min(1.f, density[id].w);
            occupiedVolume += std::min(1.f, density[id].x);
        }
        const double h = desc.gridCellSize;
        const double restVolume = activeParticles * double(particleVolume);
        if (exchange) {
            const double error = std::abs(authoritativeVolume - restVolume) / std::max(restVolume, 1e-30);
            if (error > 1e-11 || massCacheError > 2e-7 || velocityCacheError > 3e-7 || exchangeCounters[16])
                failure = "Live authoritative particle conservation/cache audit failed";
            out << ",\"particleAuthority\":{\"validated\":true,\"inventoryBits\":64,\"invalid\":"
                << exchangeCounters[16] << ",\"volumeM3\":" << authoritativeVolume
                << ",\"relativeVolumeError\":" << error << ",\"massCacheRelativeError\":" << massCacheError
                << ",\"velocityCacheRelativeError\":" << velocityCacheError << '}';
        }
        occupiedVolume *= h * h * h;
        if (!desc.ballisticTest && !desc.transferTest && desc.initialParticles >= 100000 &&
            stepCount >= 1200 && occupiedVolume < restVolume * .95)
            failure = "Settling fixture lost more than 5 percent of kernel-estimated volume: " +
                      std::to_string(occupiedVolume / restVolume);
        out << ",\"densityIterations\":" << desc.densityIterations << ",\"densitySchedule\":\""
            << (desc.tiledDensity ? "paired-tile" : "scalar") << "\""
            << ",\"densityRepairLimit\":" << (desc.cutPressure ? 4 : 1)
            << ",\"densityRepairsLastSubstep\":" << (desc.densityIterations ? repairArgs[25] : 0)
            << ",\"densityRepairRequestedLastSubstep\":" << (repairArgs[24] ? "true" : "false")
            << ",\"estimatedOccupiedVolume\":" << occupiedVolume << ",\"restVolume\":" << restVolume
            << ",\"maxRelativeDensity\":" << maxDensity
            << ",\"postStepMaxRelativeDensity\":" << currentDensity
            << ",\"postStepMaxFluidDensity\":" << currentFluidDensity
            << ",\"postStepOccupiedVolume\":" << currentVolume * h * h * h;
        auto reportPeak = [&](const char *name, uint32_t id) {
            out << ",\"" << name << "\":{\"cell\":[" << id % grid.x << ',' << (id / grid.x) % grid.y << ','
                << id / (grid.x * grid.y) << "],\"solidPhi\":" << solids[id].x
                << ",\"binParticles\":" << count[id] << '}';
        };
        reportPeak("densityPeak", peakCell);
        reportPeak("postStepDensityPeak", currentPeakCell);
        // Independent double-precision direct gather (not the GPU bins, and not
        // the pre-correction density array) checks both diagnostic peak sites.
        auto quadraticWeight = [](double x) {
            x = std::abs(x);
            return x < .5 ? .75 - x * x : (x < 1.5 ? .5 * (1.5 - x) * (1.5 - x) : 0);
        };
        double densityAuditError = 0;
        for (uint32_t id : {peakCell, currentPeakCell}) {
            const double center[] = {double(id % grid.x) + .5, double((id / grid.x) % grid.y) + .5,
                                     double(id / (grid.x * grid.y)) + .5};
            double mass = 0;
            for (uint32_t i = 0; i < desc.maxParticles; ++i) {
                if (!p[i].velocityFlags.w)
                    continue;
                double weight = p[i].apic0.w;
                for (uint32_t a = 0; a < 3; ++a)
                    weight *= quadraticWeight(
                        (double((&p[i].positionRadius.x)[a]) - (&desc.minimum.x)[a]) / h - center[a]);
                mass += weight;
            }
            if (interior)
                for (uint64_t i = 0; i < interior->state()->GetDesc().Width / sizeof(FluidParticle); ++i) {
                    const auto &o = owned[i];
                    if (!o.velocityFlags.w)
                        continue;
                    double weight = o.apic0.w;
                    const float spacing[] = {o.positionRadius.w, o.apic1.w, o.apic2.w};
                    for (uint32_t a = 0; a < 3; ++a) {
                        uint32_t n = (uint32_t(o.velocityFlags.w) >> (5 * a)) & 31;
                        double sum = 0;
                        for (uint32_t j = 0; j < n; ++j)
                            sum += quadraticWeight((double((&o.positionRadius.x)[a]) +
                                                    (double(j) - .5 * (n - 1)) * spacing[a] -
                                                    (&desc.minimum.x)[a]) /
                                                       h -
                                                   center[a]);
                        weight *= sum / n;
                    }
                    mass += weight;
                }
            if (desc.narrowBand) {
                const int cx = int(id % grid.x), cy = int((id / grid.x) % grid.y),
                          cz = int(id / (grid.x * grid.y));
                for (int z = -1; z <= 1; ++z)
                    for (int y = -1; y <= 1; ++y)
                        for (int x = -1; x <= 1; ++x) {
                            const int sx = cx + x, sy = cy + y, sz = cz + z;
                            if (sx < 0 || sy < 0 || sz < 0 || sx >= int(grid.x) || sy >= int(grid.y) ||
                                sz >= int(grid.z))
                                continue;
                            const uint32_t owner =
                                ((sz / 2) * ((grid.y + 1) / 2) + sy / 2) * ((grid.x + 1) / 2) + sx / 2;
                            const uint32_t children = std::min(2u, grid.x - (sx / 2) * 2) *
                                                      std::min(2u, grid.y - (sy / 2) * 2) *
                                                      std::min(2u, grid.z - (sz / 2) * 2);
                            const double w = (x == 0 ? 2. / 3 : 1. / 6) * (y == 0 ? 2. / 3 : 1. / 6) *
                                             (z == 0 ? 2. / 3 : 1. / 6);
                            mass += gridAuthority[owner][3] * w / (children * double(particleVolume));
                        }
            }
            densityAuditError =
                std::max(densityAuditError, std::abs(mass * particleVolume / (h * h * h) - density[id].w));
        }
        if (densityAuditError > .0002)
            failure = "Post-step density disagrees with independent current-state gather";
        out << ",\"maxPostStepDensityAuditError\":" << densityAuditError;
        out << ",\"maxCurvature\":" << maxCurvature << ",\"maxCurvatureRelativeError\":" << maxCurvatureError
            << ",\"curvatureTestSamples\":" << curvatureSamples
            << ",\"maxViscosityError\":" << maxViscosityError
            << ",\"viscosityTestSamples\":" << viscositySamples;
        out << ",\"pressureIterations\":" << desc.pressureIterations
            << ",\"divergenceRmsBefore\":" << std::sqrt(before2 / std::max(1u, projectedCells))
            << ",\"divergenceRmsAfter\":" << std::sqrt(after2 / std::max(1u, projectedCells))
            << ",\"maxDivergence\":" << maxDivergence << ",\"maxPressureResidualMismatch\":";
        if (desc.cudaMixedPressure)
            out << "null,\"pressureAudit\":\"published-fine-face-flux\",\"maxPublishedFluxDivergence\":"
                << maxCudaFluxDivergence << ",\"maxPublishedFluxMismatch\":" << maxCudaFluxMismatch;
        else
            out << maxResidualMismatch;
        const double particleMass = restMass;
        const double particleMeanY = particleMass ? meanY / particleMass : 0;
        if (desc.narrowBand) {
            restMass += gridVolume / particleVolume;
            meanY += gridMeanY / particleVolume;
            out << ",\"narrowBandOwnership\":{\"gridCells\":" << gridOwners
                << ",\"gridVolumeM3\":" << gridVolume
                << ",\"particleVolumeM3\":" << particleMass * particleVolume
                << ",\"activeParticles\":" << active << '}';
        }
        if (interior) {
            double ownedMass = 0;
            for (uint64_t i = 0; i < interior->state()->GetDesc().Width / sizeof(FluidParticle); ++i)
                if (owned[i].velocityFlags.w) {
                    ownedMass += owned[i].apic0.w;
                    meanY += double(owned[i].positionRadius.y) * owned[i].apic0.w;
                }
            if (ownedMass != interior->massUnits())
                failure = "Interior snapshot mass disagrees with ownership counters";
            restMass += ownedMass;
        }
        D3D12_RANGE written{0, 0};
        readback.resource->Unmap(0, &written);
        if (exchange
                ? std::abs(restMass - activeParticles) > std::max(1., double(activeParticles)) * 1e-11
                : (restMass != activeParticles || (!desc.adaptiveParticles && active != activeParticles)))
            failure = "Fluid rest-mass/count mismatch";
        if (maxGravityError > .00002)
            failure = "GPU gravity does not match analytic free fall";
        if (stepCount && maxSolidPenetration > .001)
            failure = "Particle penetrated scene SDF: " + std::to_string(maxSolidPenetration) +
                      " particle=" + std::to_string(penetrationParticle) +
                      " collider=" + std::to_string(penetrationCollider);
        if (!failure.empty())
            throw std::runtime_error(failure);
        out << ",\"validated\":true,\"active\":" << active << ",\"restMassUnits\":" << restMass
            << ",\"particleMassUnits\":" << particleMass
            << ",\"meanHeight\":" << (restMass ? meanY / restMass : 0)
            << ",\"particleMeanHeight\":" << particleMeanY << ",\"maxSpeed\":" << maxSpeed
            << ",\"maxParticleCfl\":" << maxSpeed / (desc.simulationRate * desc.gridCellSize)
            << ",\"maxGravityError\":" << maxGravityError;
        out << ",\"maxSolidPenetration\":" << maxSolidPenetration
            << ",\"maxBinPositionError\":" << maxBinPositionError;
        if (desc.roomPool)
            out << ",\"roomQuadrants\":[" << roomQuadrants[0] << ',' << roomQuadrants[1] << ','
                << roomQuadrants[2] << ',' << roomQuadrants[3] << ']';
        out << ",\"maxTransferError\":" << maxTransferError << ",\"maxAffineError\":" << maxAffineError
            << ",\"faceWeights\":[" << weights[0] << "," << weights[1] << "," << weights[2] << "]";
    } else
        out << ",\"validated\":false";
    out << ",\"activeCells\":" << (readbackReady ? std::to_string(activeCells) : "null")
        << ",\"maxCellOccupancy\":" << (readbackReady ? std::to_string(maxCellOccupancy) : "null")
        << ",\"gridCells\":" << grid.w;
    out << "}";
}
} // namespace lab
