#include "hamiltonian_wave.h"
#include "fluid_system.h"
#include <bit>
#include <cstring>
#include <d3dcompiler.h>

namespace lab {
using namespace DirectX;
using Microsoft::WRL::ComPtr;
HamiltonianWave::HamiltonianWave(ID3D12Device *device, const std::filesystem::path &folder,
                                 const FluidSystemDesc &d, XMUINT4 grid)
    : resolution(d.hamiltonian.resolution), fftSize(2 * resolution),
      minimum(d.waveMinimum), maximum(d.waveMaximum), config(d.hamiltonian), simulationGrid(grid),
      capacity(d.maxParticles) {
    config.validate(d.initialDepth, -d.gravity.y);
    settings.domain = {minimum.x, minimum.z, maximum.x - minimum.x, maximum.z - minimum.z};
    settings.physics = {d.initialDepth, d.minimum.y + d.initialDepth, -d.gravity.y, config.epsilon};
    settings.coupling = {config.relaxation, 4 * d.gridCellSize, 1 / d.simulationRate, config.amplitude};
    settings.local = {d.minimum.x, d.minimum.z, d.maximum.x, d.maximum.z};
    settings.grid = {resolution, fftSize, layers, config.order};
    settings.spectrum = {config.windSpeed, config.extendOpticalSurface ? 1.f : 0.f, config.minimumWavelength, 0};
    // Match FluidSystem's rest lattice, including its rounded vertical count.
    const float lx = d.maximum.x - d.minimum.x - 2 * d.particleRadius;
    const float lz = d.maximum.z - d.minimum.z - 2 * d.particleRadius;
    const float ly = d.initialDepth - d.particleRadius;
    const float spacing = std::cbrt(lx * ly * lz / d.initialParticles);
    const uint32_t nx = std::max(1u, uint32_t(lx / spacing)), nz = std::max(1u, uint32_t(lz / spacing));
    const uint32_t ny = std::max(1u, (d.initialParticles + nx * nz - 1) / (nx * nz));
    initialDepth = d.initialDepth;
    bottom = d.minimum.y;
    // Keep 40% of the spare IDs available for waves, moving boundaries and
    // rounded seed layers. Filling consumes the remaining rest-volume budget.
    particleVolume = lx * ly * lz / (nx * ny * nz);
    maximumFillDepth = std::max(initialDepth, std::min(
        d.maximum.y - bottom - 2 * d.gridCellSize - config.amplitude,
        initialDepth + float(.6 * (capacity - d.initialParticles) * particleVolume / (lx * lz))));
    maximumSourceParticles = uint32_t(double(maximumFillDepth - initialDepth) * settings.domain.z * settings.domain.w / particleVolume);
    settings.mass = {particleVolume, 0, 0, config.adaptive ? 1.f : 0.f};
    seedCandidates = nx * nz * uint32_t(std::ceil((d.maximum.y - d.minimum.y) / (ly / ny)));
    if (seedCandidates > 65535u * 64)
        throw std::runtime_error("Hamiltonian boundary seed lattice exceeds dispatch capacity");
    auto make = [&](uint64_t size, const wchar_t *name) {
        return gpu::buffer(device, size, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
    };
    pool = make(uint64_t(SlotCount) * fftSize * fftSize * 8, L"Hamiltonian / canonical spectral workspace");
    published = make(uint64_t(resolution) * resolution * (layers + 3) * 16,
                     L"Hamiltonian / height and depth velocities");
    freeIds = make(uint64_t(capacity) * 4, L"Hamiltonian / FAB recycled particle IDs");
    counters = make((64 + resolution * resolution * 5) * 4, L"Hamiltonian / diagnostics and conservative particle transfers");
    uniforms = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ, L"Hamiltonian / frame constants");
    memcpy(uniforms.mapped, &settings, sizeof(settings));
    readback = gpu::buffer(device, 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_COPY_DEST, L"Hamiltonian / bounded diagnostic readback");
    auto signature = [&](bool coupling, ComPtr<ID3D12RootSignature> &out) {
        D3D12_ROOT_PARAMETER p[13]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants = {4, 0, 8};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].Descriptor.ShaderRegister = 3;
        const uint32_t registers[]{coupling ? 39u : 0u, coupling ? 38u : 1u, coupling ? 41u : 2u};
        for (uint32_t i = 0; i < 3; ++i) {
            p[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[2 + i].Descriptor.ShaderRegister = registers[i];
        }
        p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[5].Descriptor.ShaderRegister = 0;
        const uint32_t extra[]{0, 2, 4, 12, 40};
        for (uint32_t i = 0; i < 5; ++i) {
            p[6 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[6 + i].Descriptor.ShaderRegister = extra[i];
        }
        for (uint32_t i = 11; i < 13; ++i) {
            p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[i].Descriptor.ShaderRegister = i - 10;
        }
        D3D12_ROOT_SIGNATURE_DESC desc{coupling ? 13u : 5u, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, error;
        gpu::check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                   "Hamiltonian root serialization");
        gpu::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&out)),
                   "Hamiltonian root");
    };
    signature(false, root);
    signature(true, couplingRoot);
    const char *names[]{"Init",      "FFT",       "Operator", "Product",      "Bernoulli",
                        "Integrate", "Publish",   "Snapshot", "TargetHeight", "Smooth",
                        "Relax",     "ClearFree", "Cull",     "Seed",         "Validate",
                        "Emit", "FreeWater", "Transfers", "Activity", "Regions"};
    for (uint32_t i = 0; i < PassCount; ++i) {
        const bool coupling = i == TargetHeight || i == Cull || i == Seed || i == Emit || i == FreeWater || i == Activity || i == Regions;
        auto code = gpu::bytes(folder / "shaders" / (std::string("Hamiltonian") + names[i] +
            (resolution > 32 && !coupling ? std::to_string(fftSize) : "") + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = (i == TargetHeight || i == Cull || i == Seed || i == Emit || i == FreeWater || i == Activity || i == Regions)
                               ? couplingRoot.Get() : root.Get();
        ps.CS = {code.data(), code.size()};
        gpu::check(device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&pipelines[i])), names[i]);
    }
}
void HamiltonianWave::bind(ID3D12GraphicsCommandList *cmd, const FluidGpuView *v,
                           ID3D12Resource *fluidConstants) {
    cmd->SetComputeRootSignature(v ? couplingRoot.Get() : root.Get());
    cmd->SetComputeRootConstantBufferView(1, constants());
    cmd->SetComputeRootUnorderedAccessView(2, pool.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, published.resource->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, counters.resource->GetGPUVirtualAddress());
    if (v) {
        cmd->SetComputeRootConstantBufferView(5, fluidConstants->GetGPUVirtualAddress());
        ID3D12Resource *extra[]{v->particles, v->offsets, v->indices, v->previousPositions,
                                freeIds.resource.Get()};
        for (uint32_t i = 0; i < 5; ++i)
            cmd->SetComputeRootUnorderedAccessView(6 + i, extra[i]->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(11, v->colliderAddress);
        cmd->SetComputeRootShaderResourceView(12, v->meshPhi->GetGPUVirtualAddress());
    }
}
void HamiltonianWave::prepareTransfers(float seconds, bool resetting) {
    const double area = double(settings.domain.z) * settings.domain.w;
    const float previousDepth = settings.physics.x;
    const int32_t received = resetting ? 0 : receivedSamples();
    const int32_t localReceived = resetting ? 0 : std::bit_cast<int32_t>(diagnostics[14]);
    const float localVolume = (localReceived - settings.mass.z) * particleVolume;
    inletVolume = double(received) * particleVolume;
    settings.physics.x = initialDepth + float(inletVolume / area);
    settings.physics.y = bottom + settings.physics.x;
    frameRise = resetting ? 0.f : settings.physics.x - previousDepth;
    const float localArea = config.adaptive ? std::max(1.f, float(diagnostics[18]) * float(area) / (resolution * resolution)) :
        (settings.local.z - settings.local.x - 2 * settings.coupling.y) *
        (settings.local.w - settings.local.y - 2 * settings.coupling.y);
    settings.fill.z += frameRise;
    settings.fill.w += localVolume;
    if (resetting) {
        settings.fill = {};
    }
    settings.fill.x = seconds > 0 ? (settings.fill.z - settings.fill.w / localArea) / (seconds * settings.physics.x) : 0.f;
    if (config.adaptive)
        settings.fill.x *= localArea / std::max(1.f, diagnostics[19] * .001f);
    settings.fill.y = seconds > 0 ? settings.fill.z / (seconds * settings.physics.x) : 0.f;
    if (seconds > 0)
        settings.fill.z = settings.fill.w = 0;
    settings.mass.y = float(received);
    settings.mass.z = float(localReceived);
    if (resetting || frameRise != 0)
        history = false; // finite-depth operators changed; old AB2 forcing is stale
    memcpy(uniforms.mapped, &settings, sizeof(settings));
}
void HamiltonianWave::rebase(ID3D12GraphicsCommandList *cmd) {
    if (frameRise == 0)
        return;
    bind(cmd);
    // Deposited volume is already in eta. Shift only its reference depth;
    // changing the published height here would count the same water twice.
    symbol(cmd, Eta, Eta, 7);
}
void HamiltonianWave::advanceFreeWater(ID3D12GraphicsCommandList *cmd, const FluidGpuView &view,
                                      ID3D12Resource *constants, uint32_t start, uint32_t count, uint32_t serial) {
    bind(cmd, &view, constants);
    if (count)
        run(cmd, Emit, count, start, serial, std::min(capacity, std::max(64u, capacity / 20)), 1, 0, (count + 63) / 64);
    run(cmd, FreeWater, 0, 0, 0, 0, 1, 0, (capacity + 63) / 64);
    bind(cmd);
    run(cmd, Transfers, RealH, RealP);
    fft(cmd, T0, RealH);
    sum(cmd, Eta, Eta, T0);
    fft(cmd, T0, RealP);
    sum(cmd, Psi, Psi, T0);
    fft(cmd, T0, RealPx);
    symbol(cmd, T1, T0, 3);
    fft(cmd, T0, RealPz);
    symbol(cmd, T2, T0, 4);
    sum(cmd, T1, T1, T2);
    symbol(cmd, T1, T1, 12);
    sum(cmd, Psi, Psi, T1);
    symbol(cmd, Eta, Eta, 7);
    symbol(cmd, Psi, Psi, 6);
    // Canonical source packets carry volume and momentum. The ordinary
    // wave/3D coupling handles their subsequent motion, independent of emitters.
}
void HamiltonianWave::run(ID3D12GraphicsCommandList *cmd, Pass pass, uint32_t dst, uint32_t a, uint32_t b,
                          uint32_t op, float x, float y, uint32_t groups) {
    const uint32_t args[]{dst, a, b, op, std::bit_cast<uint32_t>(x), std::bit_cast<uint32_t>(y), 0, 0};
    cmd->SetComputeRoot32BitConstants(0, 8, args, 0);
    cmd->SetPipelineState(pipelines[pass].Get());
    cmd->Dispatch(groups ? groups : fftSize * fftSize / 64, 1, 1);
    gpu::uav(cmd);
}
void HamiltonianWave::fft(ID3D12GraphicsCommandList *cmd, Slot dst, Slot src, bool inverse) {
    run(cmd, FFT, FftScratch, src, 0, inverse ? 1 : 0, 1, 0, fftSize);
    run(cmd, FFT, dst, FftScratch, 0, inverse ? 3 : 2, 1, 0, fftSize);
}
void HamiltonianWave::symbol(ID3D12GraphicsCommandList *cmd, Slot dst, Slot src, uint32_t op, float scale) {
    run(cmd, Operator, dst, src, src, op, scale);
}
void HamiltonianWave::sum(ID3D12GraphicsCommandList *cmd, Slot dst, Slot a, Slot b, float x, float y) {
    run(cmd, Operator, dst, a, b, 0, x, y);
}
void HamiltonianWave::product(ID3D12GraphicsCommandList *cmd, Slot dst, Slot a, Slot b) {
    fft(cmd, ProductA, a, true);
    fft(cmd, ProductB, b, true);
    run(cmd, Product, ProductOut, ProductA, ProductB);
    fft(cmd, dst, ProductOut);
    symbol(cmd, dst, dst, 6); // 2/3 Galerkin de-aliasing after EVERY product
}
void HamiltonianWave::dno(ID3D12GraphicsCommandList *cmd) {
    const float e = config.epsilon;
    symbol(cmd, V, Psi, 1);
    sum(cmd, Dno, V, V, 1, 0);
    if (e == 0) {
        sum(cmd, NLH, V, V, 0, 0);
        return;
    }
    symbol(cmd, Px, Psi, 3);
    symbol(cmd, Pz, Psi, 4);
    product(cmd, EtaV, Eta, V);
    symbol(cmd, G1, EtaV, 1, -1);
    product(cmd, T0, Eta, Px);
    symbol(cmd, T0, T0, 3, -1);
    sum(cmd, G1, G1, T0);
    product(cmd, T0, Eta, Pz);
    symbol(cmd, T0, T0, 4, -1);
    sum(cmd, G1, G1, T0);
    sum(cmd, NLH, G1, G1, e, 0);
    if (config.order == 3) {
        // Equation (8), evaluated as successive de-aliased products, like the reference.
        symbol(cmd, T0, EtaV, 1);
        product(cmd, T1, Eta, T0);
        symbol(cmd, G2, T1, 1);
        symbol(cmd, Bpsi, Psi, 2);
        product(cmd, T0, Eta, Bpsi);
        product(cmd, T1, Eta, T0);
        symbol(cmd, T0, T1, 1, -.5f);
        sum(cmd, G2, G2, T0);
        symbol(cmd, T0, EtaV, 2);
        product(cmd, T1, Eta, T0);
        sum(cmd, G2, G2, T1, 1, -1);
        symbol(cmd, T0, Bpsi, 1);
        product(cmd, T1, Eta, T0);
        product(cmd, T2, Eta, T1);
        sum(cmd, G2, G2, T2, 1, .5f);
        symbol(cmd, Hx, Eta, 3);
        symbol(cmd, Hz, Eta, 4);
        product(cmd, T0, Hx, Hx);
        product(cmd, T1, Hz, Hz);
        sum(cmd, T2, T0, T1);
        product(cmd, T3, T2, V);
        sum(cmd, G2, G2, T3);
        sum(cmd, NLH, NLH, G2, 1, e * e);
    }
    sum(cmd, Dno, V, NLH);
}
void HamiltonianWave::reset(ID3D12GraphicsCommandList *cmd) {
    gpu::Event event(cmd, L"Hamiltonian / reset canonical state");
    prepareTransfers(0, true);
    bind(cmd);
    run(cmd, Init);
    fft(cmd, Eta, RealH);
    fft(cmd, Psi, RealP);
    history = false;
    steps = 0;
    publish(cmd);
    snapshot(cmd);
}
void HamiltonianWave::snapshot(ID3D12GraphicsCommandList *cmd) {
    bind(cmd);
    run(cmd, Snapshot);
}
void HamiltonianWave::advance(ID3D12GraphicsCommandList *cmd) {
    gpu::Event event(cmd, L"Hamiltonian / Zakharov exact-linear AB2");
    bind(cmd);
    dno(cmd);
    if (config.epsilon > 0) {
        symbol(cmd, Hx, Eta, 3);
        symbol(cmd, Hz, Eta, 4);
        fft(cmd, RealPx, Px, true);
        fft(cmd, RealPz, Pz, true);
        fft(cmd, RealHx, Hx, true);
        fft(cmd, RealHz, Hz, true);
        fft(cmd, RealV, Dno, true);
        run(cmd, Bernoulli, RealP);
        fft(cmd, NLP, RealP);
        symbol(cmd, NLP, NLP, 6);
    } else
        sum(cmd, NLP, Psi, Psi, 0, 0);
    run(cmd, Integrate, 0, 0, 0, history ? 1 : 0);
    history = true;
    ++steps;
}
void HamiltonianWave::couple(ID3D12GraphicsCommandList *cmd, const FluidGpuView &v,
                             ID3D12Resource *fluidConstants) {
    gpu::Event event(cmd, L"Hamiltonian / 3D height relaxation and inverse DNO");
    if (config.relaxation == 0) {
        bind(cmd);
        publish(cmd);
        return;
    }
    bind(cmd);
    dno(cmd);
    sum(cmd, SavedV, Dno, Dno, 1, 0);
    fft(cmd, RealH, Eta, true);
    bind(cmd, &v, fluidConstants);
    run(cmd, TargetHeight, Target);
    bind(cmd);
    for (uint32_t i = 0; i < 2; ++i) {
        run(cmd, Smooth, SmoothTarget, Target);
        run(cmd, Smooth, Target, SmoothTarget);
    }
    run(cmd, Relax, RealH, Target);
    fft(cmd, Eta, RealH);
    symbol(cmd, Eta, Eta, 7); // preserve basin volume: feedback may not change the mean
    // Preserve the wave's own kinematic velocity, never inject NS velocity into psi.
    symbol(cmd, Psi, SavedV, 5);
    if (config.epsilon > 0)
        for (uint32_t i = 0; i < 8; ++i) {
            dno(cmd);
            sum(cmd, T0, SavedV, NLH, 1, -1);
            symbol(cmd, Psi, T0, 5);
        }
    dno(cmd);
    run(cmd, Validate, 0, 0, 0, 1);
    // A relaxation changes the canonical state: discard the old AB2 forcing.
    // Exponential Euler bootstrap is exact for the linear part (paper Sec. 3.4).
    if (config.relaxation > 0)
        history = false;
    publish(cmd);
}
void HamiltonianWave::publish(ID3D12GraphicsCommandList *cmd) {
    fft(cmd, RealH, Eta, true);
    fft(cmd, RealP, Psi, true);
    run(cmd, Publish, 0, RealH, RealP);
    symbol(cmd, V, Psi, 1);
    product(cmd, EtaV, Eta, V);
    sum(cmd, Lift, Psi, EtaV, 1, -config.epsilon);
    symbol(cmd, Lift, Lift, 8);
    // All 24 depth/component transforms are independent. Batch them, fusing
    // the harmonic symbol into the first FFT instead of serializing 96 passes.
    run(cmd, FFT, VelocityScratch, Lift, 0, 5, 1, 0, fftSize * layers * 3);
    run(cmd, FFT, VelocityScratch, VelocityScratch, 0, 11, 1, 0, fftSize * layers * 3);
    run(cmd, Publish, 0, VelocityScratch, 0, 2, 1, 0, resolution * resolution * layers / 64);
}
void HamiltonianWave::reseed(ID3D12GraphicsCommandList *cmd, const FluidGpuView &v,
                             ID3D12Resource *fluidConstants, bool initialize) {
    gpu::Event event(cmd, L"Hamiltonian / fluxed boundary particle reservoir");
    bind(cmd);
    run(cmd, ClearFree);
    bind(cmd, &v, fluidConstants);
    if (config.adaptive) {
        run(cmd, Activity, 0, 0, 0, initialize ? 1 : 0, 1, 0, resolution * resolution / 64);
        run(cmd, Regions, 0, 0, 0, initialize ? 1 : 0, 1, 0, resolution * resolution / 64);
    }
    run(cmd, Cull, 0, 0, 0, initialize ? 1 : 0, 1, 0, (capacity + 63) / 64);
    run(cmd, Seed, seedCandidates, 0, uint32_t(steps), initialize ? 1 : 0, 1, 0, (seedCandidates + 63) / 64);
}
void HamiltonianWave::recordReadback(ID3D12GraphicsCommandList *cmd) {
    bind(cmd);
    run(cmd, Validate);
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(readback.resource.Get(), 0, counters.resource.Get(), 0, sizeof(diagnostics));
    gpu::transition(cmd, counters.resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readable = true;
}
void HamiltonianWave::collect() {
    if (!readable)
        return;
    void *data = nullptr;
    D3D12_RANGE range{0, sizeof(diagnostics)}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Hamiltonian diagnostics");
    memcpy(diagnostics.data(), data, sizeof(diagnostics));
    readback.resource->Unmap(0, &written);
    readable = false;
    if (int64_t(receivedSamples()) + diagnostics[11] != diagnostics[12])
        throw std::runtime_error("Particle/wave mass ledger diverged: received=" + std::to_string(receivedSamples()) +
                                 ", free=" + std::to_string(diagnostics[11]) + ", emitted=" + std::to_string(diagnostics[12]));
    if (diagnostics[4] || diagnostics[3])
        throw std::runtime_error("Hamiltonian state at substep " + std::to_string(steps) +
                                 ": nonfinite=" + std::to_string(diagnostics[4]) +
                                 ", boundary overflow=" + std::to_string(diagnostics[3]) +
                                 ", free IDs=" + std::to_string(diagnostics[0]) +
                                 ", seed attempts=" + std::to_string(diagnostics[1]) +
                                 ", active interior=" + std::to_string(diagnostics[9]));
}
void HamiltonianWave::report(std::ostream &out) const {
    out << "{\"enabled\":true,\"hosOrder\":" << config.order << ",\"epsilon\":" << config.epsilon
        << ",\"amplitudeMetres\":" << config.amplitude << ",\"relaxationPerSecond\":" << config.relaxation
        << ",\"windSpeedMetresPerSecond\":" << config.windSpeed << ",\"minimumWavelengthMetres\":" << config.minimumWavelength
        << ",\"resolution\":" << resolution << ",\"fftResolution\":" << fftSize
        << ",\"depthLayers\":" << layers << ",\"steps\":" << steps
        << ",\"meanDepthMetres\":" << depth() << ",\"fillLimitMetres\":" << fillLimit()
        << ",\"addedVolumeM3\":" << addedVolume()
        << ",\"freeWaterParticles\":" << diagnostics[11] << ",\"sourceParticles\":" << diagnostics[12]
        << ",\"receivedParticles\":" << receivedSamples() << ",\"solidContacts\":" << diagnostics[15]
        << ",\"sourceAdmissionRejected\":" << rejectedSamples()
        << ",\"adaptiveRegions\":" << (config.adaptive ? "true" : "false")
        << ",\"activeColumns\":" << activeColumns() << ",\"regionChanges\":" << regionChanges()
        << ",\"publishedMeanHeightMetres\":" << publishedMeanHeight()
        << ",\"localBounds\":[" << settings.local.x << ','
        << settings.local.y << ',' << settings.local.z << ',' << settings.local.w << ']'
        << ",\"boundarySeeded\":" << diagnostics[1] - diagnostics[16] << ",\"boundaryRetired\":" << diagnostics[2]
        << ",\"boundaryOverflow\":" << diagnostics[3] << ",\"nonfinite\":" << diagnostics[4]
        << ",\"maxElevation\":" << std::bit_cast<float>(diagnostics[5])
        << ",\"maxVelocity\":" << std::bit_cast<float>(diagnostics[6]) << ",\"inverseDnoRelativeResidual\":"
        << std::bit_cast<float>(diagnostics[7]) / std::max(1e-6f, std::bit_cast<float>(diagnostics[8]))
        << "}";
}
} // namespace lab
