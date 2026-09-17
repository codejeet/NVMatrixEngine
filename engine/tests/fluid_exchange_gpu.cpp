#include "../src/fluid/fluid_particle_grid_exchange.h"
#include "../src/fluid/fluid_implicit_transport.h"
#include "../src/fluid/fluid_uniforms.h"
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#ifndef PT_EXCHANGE_CUDA
#define PT_EXCHANGE_CUDA 0
#endif
#if PT_EXCHANGE_CUDA
#include "cuda_owned_exchange.h"
#endif

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
using namespace lab;
using namespace DirectX;
using Microsoft::WRL::ComPtr;
using Quantity = std::array<double, 4>;
struct Particle {
    XMFLOAT4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
static_assert(sizeof(Particle) == 80);
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Snapshot {
    std::vector<Particle> particles;
    std::vector<Quantity> quantities;
    std::vector<XMFLOAT4> previousPositions;
    std::array<Quantity, 2> grid;
    std::array<uint32_t, 16> counters;
};
Quantity sum(const Snapshot &s) {
    Quantity total{};
    for (const auto &q : s.quantities)
        for (uint32_t a = 0; a < 4; a++)
            total[a] += q[a];
    for (const auto &q : s.grid)
        for (uint32_t a = 0; a < 4; a++)
            total[a] += q[a];
    return total;
}
double energy(const Snapshot &s) {
    double total = 0;
    auto add = [&](const auto &q) {
        if (q[3] > 0)
            for (uint32_t a = 0; a < 3; a++)
                total += .5 * q[a] * q[a] / q[3];
    };
    for (const auto &q : s.quantities)
        add(q);
    for (const auto &q : s.grid)
        add(q);
    return total;
}
uint32_t alive(const Snapshot &s) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < s.particles.size(); i++) {
        const auto &p = s.particles[i];
        const auto &q = s.quantities[i];
        require((p.velocityFlags.w != 0) == (q[3] > 0), "Particle cache/authority liveness disagreement");
        for (double x : q)
            require(std::isfinite(x), "Nonfinite authoritative particle quantity");
        if (p.velocityFlags.w) {
            count++;
            require(std::memcmp(&s.previousPositions[i], &p.positionRadius, sizeof(XMFLOAT4)) == 0,
                    "Seeded or restored particle retained stale motion history");
            require(std::abs(double(.1f) * p.apic0.w - q[3]) <= std::max(1e-15, 2e-7 * q[3]),
                    "Particle mass cache rounding exceeds contract");
            for (uint32_t a = 0; a < 3; a++)
                require(std::abs((&p.velocityFlags.x)[a] - q[a] / q[3]) <= 2e-6,
                        "Particle velocity cache disagreement");
        } else
            require(q == Quantity{}, "Dead particle retained physical quantity");
    }
    return count;
}
// Re-bin fractional restored quantities with the actual engine kernels, then
// run production P2G. The oracle gathers directly from all particle positions,
// independently of the GPU ranges and rounded particle caches.
class TransferProbe {
    ComPtr<ID3D12RootSignature> root;
    std::array<ComPtr<ID3D12PipelineState>, 8> pipelines;
    gpu::Buffer constants, counts, offsets, cursors, indices, scan, faces, massCache, dummy, readback;
    XMUINT4 fine;
    uint32_t faceCount;
    float volume;

  public:
    TransferProbe(ID3D12Device *device, const std::filesystem::path &folder, XMUINT4 grid, uint32_t slots,
                  float particleVolume)
        : fine(grid), faceCount(3 * (grid.x + 1) * (grid.y + 1) * (grid.z + 1)), volume(particleVolume) {
        auto make = [&](uint64_t bytes) {
            return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        counts = make(grid.w * 4);
        offsets = make((grid.w + 1) * 4);
        cursors = make(grid.w * 4);
        indices = make(slots * 4);
        scan = make(4);
        faces = make(faceCount * 16);
        massCache = make(grid.w * 4);
        dummy = make(256);
        constants = gpu::buffer(device, 512, D3D12_HEAP_TYPE_UPLOAD);
        FluidSimulationConstants c{};
        c.minimumCell = {0, 0, 0, 1};
        c.maximumRadius = {float(grid.x), float(grid.y), float(grid.z), .02f};
        c.counts = {slots, 0, 0, 4};
        c.grid = grid;
        c.display.w = 2;
        c.initialMinimum.w = volume;
        memcpy(constants.mapped, &c, sizeof(c));
        const uint32_t registers[]{0, 1, 2, 3, 4, 5, 6, 15, 22, 23, 37};
        D3D12_ROOT_PARAMETER p[12]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (uint32_t i = 0; i < 11; i++) {
            p[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[i + 1].Descriptor.ShaderRegister = registers[i];
        }
        D3D12_ROOT_SIGNATURE_DESC d{12, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob, error;
        gpu::check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                   "Transfer probe root serialization");
        gpu::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root)),
                   "Transfer probe root");
        const char *names[]{"ClearBins",  "CountBins", "ScanCells",           "ScanSums",
                            "FinishScan", "Scatter",   "GatherMassAuthority", "P2GAuthority"};
        for (uint32_t i = 0; i < 8; i++) {
            auto code = gpu::bytes(folder / "shaders" / (std::string("Fluid") + names[i] + ".dxil"));
            D3D12_COMPUTE_PIPELINE_STATE_DESC state{};
            state.pRootSignature = root.Get();
            state.CS = {code.data(), code.size()};
            gpu::check(device->CreateComputePipelineState(&state, IID_PPV_ARGS(&pipelines[i])), names[i]);
        }
        readback = gpu::buffer(device, faceCount * 16 + grid.w * 4, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    void record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *particles, ID3D12Resource *quantities) {
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, constants.resource->GetGPUVirtualAddress());
        ID3D12Resource *buffers[]{particles,
                                  counts.resource.Get(),
                                  offsets.resource.Get(),
                                  cursors.resource.Get(),
                                  indices.resource.Get(),
                                  scan.resource.Get(),
                                  faces.resource.Get(),
                                  massCache.resource.Get(),
                                  dummy.resource.Get(),
                                  dummy.resource.Get(),
                                  quantities};
        for (uint32_t i = 0; i < 11; i++)
            cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i]->GetGPUVirtualAddress());
        const auto *c = static_cast<const FluidSimulationConstants *>(constants.mapped);
        const uint32_t groups[]{
            (fine.w + 255) / 256, (c->counts.x + 255) / 256, (fine.w + 255) / 256, 1,
            (fine.w + 255) / 256, (c->counts.x + 255) / 256, (fine.w + 255) / 256, (faceCount + 127) / 128};
        for (uint32_t i = 0; i < 8; i++) {
            cmd->SetPipelineState(pipelines[i].Get());
            cmd->Dispatch(groups[i], 1, 1);
            gpu::uav(cmd);
        }
        uint64_t offset = 0;
        for (auto buffer : {faces.resource.Get(), massCache.resource.Get()}) {
            const uint64_t bytes = buffer == faces.resource.Get() ? faceCount * 16 : fine.w * 4;
            gpu::transition(cmd, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(), offset, buffer, 0, bytes);
            offset += bytes;
            gpu::transition(cmd, buffer, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    void validate(const Snapshot &s) {
        void *data;
        D3D12_RANGE range{0, SIZE_T(faceCount * 16 + fine.w * 4)}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &data), "Transfer probe readback");
        auto facesData = static_cast<const XMFLOAT4 *>(data);
        auto cellMass = reinterpret_cast<const float *>(facesData + faceCount);
        double maxMassError = 0, maxVelocityError = 0;
        auto quadratic = [](double x) {
            x = std::abs(x);
            return x < .5 ? .75 - x * x : (x < 1.5 ? .5 * (1.5 - x) * (1.5 - x) : 0);
        };
        for (uint32_t cell = 0; cell < fine.w; cell++) {
            double expected = 0;
            for (uint32_t i = 0; i < s.particles.size(); i++)
                if (s.quantities[i][3] > 0) {
                    const auto p = s.particles[i].positionRadius;
                    const uint32_t x = std::min(uint32_t(std::max(0.f, p.x)), fine.x - 1),
                                   y = std::min(uint32_t(std::max(0.f, p.y)), fine.y - 1),
                                   z = std::min(uint32_t(std::max(0.f, p.z)), fine.z - 1);
                    if ((z * fine.y + y) * fine.x + x == cell)
                        expected += s.quantities[i][3] / volume;
                }
            require(std::isfinite(cellMass[cell]) && std::abs(cellMass[cell] - expected) < 2e-6,
                    "Production bin mass quantized or lost fractional owners");
        }
        const uint32_t stride = faceCount / 3;
        for (uint32_t id = 0; id < faceCount; id++) {
            uint32_t axis = id / stride, k = id % stride, x = k % (fine.x + 1),
                     y = k / (fine.x + 1) % (fine.y + 1), z = k / ((fine.x + 1) * (fine.y + 1));
            uint32_t extent[]{fine.x, fine.y, fine.z};
            extent[axis]++;
            double mass = 0, momentum = 0;
            if (x < extent[0] && y < extent[1] && z < extent[2])
                for (uint32_t i = 0; i < s.particles.size(); i++) {
                    const auto &q = s.quantities[i];
                    if (q[3] <= 0)
                        continue;
                    double w = q[3] / volume;
                    const auto &p = s.particles[i];
                    const double center[]{x + (axis == 0 ? 0 : .5), y + (axis == 1 ? 0 : .5),
                                          z + (axis == 2 ? 0 : .5)};
                    double affine = 0;
                    const auto &row = axis == 0 ? p.apic0 : axis == 1 ? p.apic1 : p.apic2;
                    for (uint32_t a = 0; a < 3; a++) {
                        const double d = center[a] - (&p.positionRadius.x)[a];
                        w *= quadratic(d);
                        affine += (&row.x)[a] * d;
                    }
                    mass += w;
                    momentum += w * (q[axis] / q[3] + affine);
                }
            const double v = mass > 1e-8 ? momentum / mass : 0;
            const auto f = facesData[id];
            maxMassError = std::max(maxMassError, std::abs(f.z - mass));
            maxVelocityError = std::max(maxVelocityError, std::abs(f.x - v));
            require(std::isfinite(f.x) && std::isfinite(f.z) && std::abs(f.z - mass) < 3e-6 &&
                        std::abs(f.x - v) < 3e-6 && f.x == f.y,
                    "Production P2G disagrees with independent authoritative gather");
        }
        readback.resource->Unmap(0, &written);
        std::cout << "PASS production fractional bin/P2G | max mass error " << maxMassError
                  << " | max velocity error " << maxVelocityError << '\n';
    }
};
void runFlowingBandTests(ID3D12Device *, const std::filesystem::path &);
int main(int argc, char **argv) try {
    require(argc == 2, "Pass the built lab runtime folder");
    ComPtr<IDXGIFactory6> factory;
    gpu::check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Exchange fixture DXGI");
    ComPtr<ID3D12Device> device;
    for (uint32_t i = 0; !device; i++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            throw std::runtime_error("No hardware DX12 device");
        DXGI_ADAPTER_DESC1 d{};
        gpu::check(adapter->GetDesc1(&d), "Exchange adapter description");
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS features{};
    gpu::check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &features, sizeof(features)),
               "Exchange FP64 support");
    require(features.DoublePrecisionFloatShaderOps, "Exchange fixture requires hardware FP64");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    gpu::check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "Exchange fixture queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "Exchange fixture allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;
    gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd)),
               "Exchange fixture list");
    gpu::check(cmd->Close(), "Exchange initial close");
    ComPtr<ID3D12Fence> fence;
    gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Exchange fixture fence");
    struct Event {
        HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ~Event() {
            if (value)
                CloseHandle(value);
        }
    } event;
    require(event.value != nullptr, "Exchange fixture event");
    uint64_t serial = 0, frequency = 0;
    gpu::check(queue->GetTimestampFrequency(&frequency), "Exchange timestamp frequency");
    const char *names[]{"fractional flowing retire/restore", "allocation exhaustion",
                        "missing restoration sites",         "capacity refusal",
                        "invalid restoration sites",         "closing grid owner",
                        "reserved future emitter slots"};
    for (uint32_t test = 0; test < 7; test++) {
        gpu::check(allocator->Reset(), "Exchange allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "Exchange list reset");
        const uint32_t slots = test == 1 ? 5u : 16u;
        const XMUINT4 fine{4, 2, 2, 16};
        std::vector<Particle> particles(slots);
        std::vector<XMFLOAT4> previousPositions(slots, XMFLOAT4{-999, -999, -999, -999});
        for (uint32_t i = 0; i < 5; i++) {
            particles[i].positionRadius = {i < 2 ? .25f : i < 4 ? 1.25f : 2.25f, .25f, .25f, .02f};
            particles[i].velocityFlags = {float(i) * .125f - .25f, .3f, float(i & 1) * .125f, 1};
            particles[i].apic0.w = 1;
        }
        std::array<uint32_t, 17> offsets{};
        offsets[1] = 2;
        offsets[2] = 4;
        for (uint32_t i = 3; i < offsets.size(); i++)
            offsets[i] = 5;
        const std::array<uint32_t, 5> indices{0, 1, 2, 3, 4};
        const std::array<uint32_t, 2> depositMask{1, 0}, restoreMask{0, 0};
        const std::array<std::array<double, 2>, 2> capacities{{{test == 3 ? .2 : 1, 1}, {1, 1}}};
        auto transportCapacities = capacities;
        if (test == 5)
            transportCapacities[0][0] = 0;
        std::array<XMFLOAT4, 16> sites{};
        for (uint32_t i = 0; i < 16; i++)
            sites[i] = {float(i / 8) * 2 + .25f + float(i % 4) * .25f, .25f + float(i % 8 / 4) * .5f, .25f,
                        0};
        if (test == 4)
            sites[0].x = std::numeric_limits<float>::quiet_NaN();
        const std::array<uint32_t, 2> siteCounts{test == 2 ? 0u : 3u, 5};
        std::array<double, 36> rates{};
        rates[1] = test == 5 ? 2 : .5;
        std::array<Quantity, 36> transfers{};
        std::array<XMFLOAT2, 2> limiter{};
        std::vector<gpu::Buffer> uploads;
        auto upload = [&](const auto &data, bool shared = false) {
            const uint64_t bytes = data.size() * sizeof(data[0]);
            auto staging = gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD);
            memcpy(staging.mapped, data.data(), size_t(bytes));
            auto target = gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_COPY_DEST, L"Exchange fixture buffer",
                                      shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE);
            cmd->CopyBufferRegion(target.resource.Get(), 0, staging.resource.Get(), 0, bytes);
            gpu::transition(cmd.Get(), target.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uploads.push_back(std::move(staging));
            return target;
        };
        auto p = upload(particles), o = upload(offsets), ids = upload(indices), dm = upload(depositMask),
             rm = upload(restoreMask), cap = upload(capacities),
             timedCap = upload(transportCapacities, PT_EXCHANGE_CUDA != 0), pts = upload(sites),
             cnt = upload(siteCounts), flow = upload(rates, PT_EXCHANGE_CUDA != 0),
             flux = upload(transfers, PT_EXCHANGE_CUDA != 0), lim = upload(limiter),
             previous = upload(previousPositions);
        FluidParticleGridExchange exchange(
            device.Get(), argv[1], fine, slots, .1f, .02f,
            FluidParticleGridExchange::CudaSharing{false, PT_EXCHANGE_CUDA != 0});
        FluidParticleGridExchange::View view{
            p.resource.Get(),   o.resource.Get(),   ids.resource.Get(), dm.resource.Get(),
            cap.resource.Get(), pts.resource.Get(), cnt.resource.Get(), 8};
        view.previousPositions = previous.resource.Get();
        if (test == 6)
            view.reusableParticleLimit = 5;
        const uint64_t particleBytes = slots * sizeof(Particle), quantityBytes = slots * sizeof(Quantity);
        const uint64_t previousBytes = slots * sizeof(XMFLOAT4);
        const uint64_t stageBytes =
            (particleBytes + quantityBytes + previousBytes + 128 + 255) & ~uint64_t(255);
        auto readback = gpu::buffer(device.Get(), 7 * stageBytes, D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        auto copy = [&](uint64_t offset, ID3D12Resource *source, uint64_t bytes) {
            gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(), offset, source, 0, bytes);
            gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        auto capture = [&](uint32_t stage) {
            uint64_t offset = stage * stageBytes;
            copy(offset, p.resource.Get(), particleBytes);
            offset += particleBytes;
            copy(offset, exchange.particleQuantities(), quantityBytes);
            offset += quantityBytes;
            copy(offset, previous.resource.Get(), previousBytes);
            offset += previousBytes;
            copy(offset, exchange.gridRead(), 64);
            copy(offset + 64, exchange.counters(), 64);
        };
        auto rejects = [&](const auto &operation) {
            bool caught = false;
            try {
                operation();
            } catch (const std::runtime_error &) {
                caught = true;
            }
            require(caught, "Invalid exchange API sequence/bounds were accepted");
        };
        rejects([&] { exchange.begin(cmd.Get(), view, false); });
        auto badView = view;
        badView.siteStride = 1024;
        rejects([&] { exchange.begin(cmd.Get(), badView, true); });
        exchange.begin(cmd.Get(), view, true);
        rejects([&] { exchange.seed(cmd.Get(), view, slots, 1); });
        exchange.seed(cmd.Get(), view, 0, 5);
        capture(0);
        exchange.deposit(cmd.Get(), view);
        rejects([&] { exchange.deposit(cmd.Get(), view); });
        capture(1);
#if PT_EXCHANGE_CUDA
        const std::array<gpu::CudaInterop::Binding, 6> transportViews{{{exchange.gridRead(), 64},
                                                                       {exchange.gridWrite(), 64},
                                                                       {timedCap.resource.Get(), 32},
                                                                       {flow.resource.Get(), 288},
                                                                       {flux.resource.Get(), 1152},
                                                                       {exchange.counters(), 68}}};
        CudaOwnedExchangeFixture transport(device.Get(), queue.Get(), transportViews);
        transport.record(cmd.Get(), queue.Get(), allocator.Get());
        exchange.commitGridTransport();
#else
        FluidImplicitTransport transport(device.Get(), argv[1], {2, 1, 1, 2}, true);
        transport.beginFrame(cmd.Get(), true, true);
        transport.record(cmd.Get(), exchange.gridRead(), timedCap.resource.Get(), flow.resource.Get(),
                         exchange.gridWrite(), flux.resource.Get(), lim.resource.Get(), .25f, test == 5);
        exchange.commitGridTransport();
        transport.finishFrame(cmd.Get());
#endif
        capture(2);
        view.requests = rm.resource.Get();
        exchange.restore(cmd.Get(), view);
        rejects([&] { exchange.restore(cmd.Get(), view); });
        rejects([&] { exchange.deposit(cmd.Get(), view); });
        capture(3);
        exchange.velocityDelta(cmd.Get(), view);
        capture(4);
        // Known untouched bystander: apply a representable solver velocity
        // increment without assuming any order for restored particle IDs.
        auto updated = particles[4];
        updated.velocityFlags.x += .125f;
        updated.velocityFlags.z += .5f;
        auto staging = gpu::buffer(device.Get(), sizeof(Particle), D3D12_HEAP_TYPE_UPLOAD);
        memcpy(staging.mapped, &updated, sizeof(updated));
        gpu::transition(cmd.Get(), p.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(p.resource.Get(), 4 * sizeof(Particle), staging.resource.Get(), 0,
                              sizeof(Particle));
        gpu::transition(cmd.Get(), p.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uploads.push_back(std::move(staging));
        exchange.velocityDelta(cmd.Get(), view);
        capture(5);
        exchange.velocityDelta(cmd.Get(), view);
        capture(6);
        // Deliberately poison only the known bystander's FP32 mass/velocity
        // caches. Authoritative P2G must consume q, not these cached values.
        auto poisoned = particles[4];
        poisoned.apic0.w = 7;
        poisoned.velocityFlags = {17, -29, 43, 1};
        auto poisonUpload = gpu::buffer(device.Get(), sizeof(Particle), D3D12_HEAP_TYPE_UPLOAD);
        memcpy(poisonUpload.mapped, &poisoned, sizeof(poisoned));
        gpu::transition(cmd.Get(), p.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(p.resource.Get(), 4 * sizeof(Particle), poisonUpload.resource.Get(), 0,
                              sizeof(Particle));
        gpu::transition(cmd.Get(), p.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uploads.push_back(std::move(poisonUpload));
        TransferProbe probe(device.Get(), argv[1], fine, slots, .1f);
        probe.record(cmd.Get(), p.resource.Get(), exchange.particleQuantities());
        gpu::check(cmd->Close(), "Exchange fixture close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Exchange fixture signal");
        gpu::check(fence->SetEventOnCompletion(serial, event.value), "Exchange fixture completion");
        require(WaitForSingleObject(event.value, 30000) == WAIT_OBJECT_0, "Exchange GPU fixture timed out");
#if PT_EXCHANGE_CUDA
        transport.collect();
#else
        transport.collect(frequency);
#endif
        void *mapped;
        D3D12_RANGE range{0, SIZE_T(7 * stageBytes)}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Exchange snapshot map");
        std::array<Snapshot, 7> snapshots;
        for (uint32_t s = 0; s < 7; s++) {
            const char *bytes = static_cast<const char *>(mapped) + s * stageBytes;
            auto &snap = snapshots[s];
            snap.particles.resize(slots);
            snap.quantities.resize(slots);
            snap.previousPositions.resize(slots);
            memcpy(snap.particles.data(), bytes, size_t(particleBytes));
            bytes += particleBytes;
            memcpy(snap.quantities.data(), bytes, size_t(quantityBytes));
            bytes += quantityBytes;
            memcpy(snap.previousPositions.data(), bytes, size_t(previousBytes));
            bytes += previousBytes;
            memcpy(snap.grid.data(), bytes, 64);
            memcpy(snap.counters.data(), bytes + 64, 64);
        }
        readback.resource->Unmap(0, &written);
        const auto initial = sum(snapshots[0]);
        double maxError = 0;
        for (uint32_t s = 0; s < 7; s++) {
            const auto &snap = snapshots[s];
            alive(snap);
            const auto total = sum(snap);
            for (uint32_t a = 0; a < 4; a++) {
                const double impulse = s >= 5 ? (a == 0 ? .125 : a == 2 ? .5 : 0) * double(.1f) : 0;
                const double error = std::abs(total[a] - initial[a] - impulse);
                maxError = std::max(maxError, error);
                require(error < 2e-13, "Particle/grid transaction changed total mass or momentum");
            }
            if (s < 5)
                require(energy(snap) <= energy(snapshots[0]) + 1e-13, "Exchange increased kinetic energy");
        }
        require(alive(snapshots[1]) == (test == 3 ? 5u : 1u),
                "Deposit failed to retire the exact source particles");
        if (test != 3) {
            const auto &g = snapshots[2].grid;
            const double expected = test == 5 ? 0 : double(.1f) * 4 / 1.125;
            const double receiver = test == 5 ? double(.1f) * 4 : .125 * expected;
            require(std::abs(g[0][3] - expected) < 2e-13 && std::abs(g[1][3] - receiver) < 2e-13,
                    "Grid-owned fluid did not advect through the existing solver");
        }
        const auto &end = snapshots[3];
        require(end.counters[7] == (test == 4 ? 1u : 0u), "Unexpected exchange invalid counter");
        if (test == 0)
            require(alive(end) == 9 && end.grid[0] == Quantity{} && end.grid[1] == Quantity{},
                    "Complete restoration retained or lost an owner");
        if (test == 1 || test == 6)
            require(end.counters[1] == 3 && end.counters[6] == 1 && alive(end) == 4 && end.grid[1][3] > 0,
                    "Failed reservation consumed slots or lost grid mass");
        if (test == 2)
            require(end.counters[6] == 1 && end.grid[0][3] > 0 && alive(end) == 6,
                    "Missing sites did not retain grid ownership");
        if (test == 3)
            require(end.counters[8] == 1 && alive(end) == 5, "Capacity refusal changed particles");
        if (test == 4)
            require(end.counters[1] == 5 && end.grid[0][3] > 0 && alive(end) == 6,
                    "Invalid sites committed a partial restoration");
        if (test == 5)
            require(end.grid[0] == Quantity{} && end.grid[1] == Quantity{} && alive(end) == 6,
                    "Closing owner retained residue or failed restoration");
        require(snapshots[3].quantities == snapshots[4].quantities,
                "Zero cache increment rounded authoritative momentum");
        require(snapshots[5].quantities == snapshots[6].quantities, "Velocity increment was applied twice");
        probe.validate(snapshots[6]);
        std::cout << "PASS " << names[test] << " | max joint quantity error " << maxError
                  << " | live particles " << alive(end) << '\n';
#if PT_EXCHANGE_CUDA
        std::cout << "{\"case\":\"cuda-exchange-" << test << "\",\"maximumQuantityError\":" << maxError
                  << ",\"pass\":true}\n";
#endif
    }
#if PT_EXCHANGE_CUDA
    std::cout << "PASS CUDA exchange: seven DX12 retire -> CUDA transport -> DX12 restore cases\n";
#else
    runFlowingBandTests(device.Get(), argv[1]);
#endif
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
