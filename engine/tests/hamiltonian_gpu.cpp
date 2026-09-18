#include "../src/fluid/fluid_system.h"
#include <complex>
#include <cstring>
#include <dxgi1_6.h>
#include <iostream>
#include <numbers>
#include <sstream>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
using namespace lab;
using namespace DirectX;
using Microsoft::WRL::ComPtr;
using Complex = std::complex<double>;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Context {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t serial = 0;
    Context() {
        gpu::check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)),
                   "Wave test device");
        D3D12_COMMAND_QUEUE_DESC desc{};
        gpu::check(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "Wave test queue");
        gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                   "Wave test allocator");
        gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&cmd)),
                   "Wave test list");
        gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Wave test fence");
        require(event != nullptr, "Wave test event");
    }
    ~Context() { CloseHandle(event); }
    void finish() {
        gpu::check(cmd->Close(), "Wave test close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Wave test signal");
        gpu::check(fence->SetEventOnCompletion(serial, event), "Wave test completion");
        require(WaitForSingleObject(event, 60000) == WAIT_OBJECT_0, "Wave test GPU timeout");
        gpu::check(allocator->Reset(), "Wave test allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "Wave test list reset");
    }
    template <class T> std::vector<T> read(ID3D12Resource *resource, size_t count) {
        auto result =
            gpu::buffer(device.Get(), count * sizeof(T), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                        D3D12_RESOURCE_STATE_COPY_DEST, L"Wave test readback");
        gpu::transition(cmd.Get(), resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(result.resource.Get(), 0, resource, 0, count * sizeof(T));
        gpu::transition(cmd.Get(), resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        finish();
        void *data = nullptr;
        D3D12_RANGE range{0, count * sizeof(T)}, empty{0, 0};
        gpu::check(result.resource->Map(0, &range, &data), "Wave test map");
        std::vector<T> values(count);
        memcpy(values.data(), data, range.End);
        result.resource->Unmap(0, &empty);
        return values;
    }
};
static Complex c(XMFLOAT2 v) { return {v.x, v.y}; }
int main(int argc, char **argv) try {
    require(argc == 2 || (argc == 3 && (std::string(argv[2]) == "--ocean-grid" || std::string(argv[2]) == "--grid64")),
            "Usage: NVMatrixEngineHamiltonianGpuTest <runtime folder> [--ocean-grid|--grid64]");
    Context ctx;
    FluidSystemDesc d;
    d.roomPool = true;
    d.hamiltonian.enabled = true;
    d.hamiltonian.adaptive = false; // fixed domains isolate the analytic wave/transfer fixtures
    d.hamiltonian.relaxation = 0;
    d.hamiltonian.epsilon = 0;
    d.hamiltonian.resolution = argc == 3 ? (std::string(argv[2]) == "--grid64" ? 64 : 128) : 32;
    d.initialDepth = .8f;
    d.hamiltonian.amplitude = .05f;
    d.minimum = {-4, 0, -4};
    d.maximum = {4, 2, 4};
    d.waveMinimum = {-5, 0, -5};
    d.waveMaximum = {5, 2, 5};
    d.gridCellSize = .25f;
    d.particleRadius = .025f;
    const uint32_t P = d.hamiltonian.resolution, N = 2 * P, NN = N * N;
    const XMUINT4 grid{32, 8, 32, 8192};
    HamiltonianWave linear(ctx.device.Get(), argv[1], d, grid);
    linear.reset(ctx.cmd.Get());
    auto initial = ctx.read<XMFLOAT2>(linear.canonicalState(), 2 * NN);
    auto initialSurface = ctx.read<XMFLOAT4>(linear.surface(), P * P);
    for (uint32_t z = 0; z < P; ++z)
        for (uint32_t x = 0; x < P; ++x) {
            double expected = .8 + .05 * (.7 * std::cos(2 * std::numbers::pi * (x + .5) / P) +
                                          .3 * std::cos(3 * std::numbers::pi * (z + .5) / P));
            require(std::abs(initialSurface[z * P + x].x - expected) < 2e-6,
                    "FFT roundtrip/reflecting initialization mismatch");
        }
    constexpr uint32_t steps = 120;
    for (uint32_t i = 0; i < steps; ++i)
        linear.advance(ctx.cmd.Get());
    linear.couple(ctx.cmd.Get(), {}, nullptr); // relaxation=0 publishes an uncoupled state
    auto state = ctx.read<XMFLOAT2>(linear.canonicalState(), 2 * NN);
    double maxError = 0;
    for (uint32_t j = 0; j < NN; ++j) {
        int mx = int(j % N), mz = int(j / N);
        if (mx >= int(N / 2))
            mx -= N;
        if (mz >= int(N / 2))
            mz -= N;
        double k = std::numbers::pi * std::hypot(mx, mz) / 10, G = k * std::tanh(k * .8),
               w = std::sqrt(9.81 * G);
        auto filter = [N](int m) {
            double r = std::abs(m) / double(N / 2);
            return (5 + 4 * std::cos(std::numbers::pi * r) - std::cos(2 * std::numbers::pi * r)) / 8;
        };
        double f = std::pow(filter(mx) * filter(mz), steps), t = steps / d.simulationRate;
        if (std::abs(mx) >= int(N / 3) || std::abs(mz) >= int(N / 3))
            f = 0;
        Complex expectedH = c(initial[j]) * std::cos(w * t) * f;
        Complex expectedP = c(initial[j]) * (-9.81 * (w > 0 ? std::sin(w * t) / w : t)) * f;
        maxError = std::max({maxError, std::abs(c(state[j]) - expectedH) / NN,
                             std::abs(c(state[NN + j]) - expectedP) / NN});
    }
    require(maxError < 3e-6, "Exact finite-depth Airy dispersion / zero-mode propagation failed");
    auto surface = ctx.read<XMFLOAT4>(linear.surface(), P * P * (HamiltonianWave::layers + 1));
    for (uint32_t layer = 0; layer < HamiltonianWave::layers; ++layer) {
        const double depth = .8 * (1 - double(layer) / (HamiltonianWave::layers - 1));
        for (uint32_t z = 0; z < P; z += 7)
            for (uint32_t x = 0; x < P; x += 7) {
                double vx = 0, vy = 0, vz = 0;
                for (auto j : {2u, N - 2, 3 * N, (N - 3) * N}) {
                    int mx = int(j % N), mz = int(j / N);
                    if (mx >= int(N / 2))
                        mx -= N;
                    if (mz >= int(N / 2))
                        mz -= N;
                    double kx = std::numbers::pi * mx / 10, kz = std::numbers::pi * mz / 10,
                           k = std::hypot(kx, kz);
                    double C = std::cosh(k * (.8 - depth)) / std::cosh(k * .8),
                           S = k * std::sinh(k * (.8 - depth)) / std::cosh(k * .8);
                    Complex mode =
                        c(state[NN + j]) *
                        std::polar(1.0, 2 * std::numbers::pi * (mx * double(x) + mz * double(z)) / N) /
                        double(NN);
                    vx += (Complex(0, kx * C) * mode).real();
                    vy += (S * mode).real();
                    vz += (Complex(0, kz * C) * mode).real();
                }
                const auto v = surface[(layer + 1) * P * P + z * P + x];
                require(std::abs(v.x - vx) < 3e-6 && std::abs(v.y - vy) < 3e-6 && std::abs(v.z - vz) < 3e-6,
                        "Harmonic depth extension / bottom no-flux failed");
            }
    }
    std::cout << "PASS FFT, reflecting walls, Airy dispersion and eight depth layers; max error="
              << maxError << '\n';
    {
        auto sourceDesc = d;
        sourceDesc.initialParticles = 10000;
        sourceDesc.maxParticles = 15000;
        sourceDesc.hamiltonian.amplitude = 0;
        HamiltonianWave source(ctx.device.Get(), argv[1], sourceDesc, grid);
        std::vector<gpu::Buffer> staging;
        auto upload = [&](const void *data, size_t bytes) {
            auto u = gpu::buffer(ctx.device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, L"Free-water fixture staging");
            memcpy(u.mapped, data, bytes);
            auto result = gpu::buffer(ctx.device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_COPY_DEST, L"Free-water fixture");
            ctx.cmd->CopyBufferRegion(result.resource.Get(), 0, u.resource.Get(), 0, bytes);
            gpu::transition(ctx.cmd.Get(), result.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            staging.push_back(std::move(u));
            return result;
        };
        std::vector<FluidParticle> empty(sourceDesc.maxParticles);
        std::vector<uint32_t> emptyOffsets(grid.w + 1);
        auto particles = upload(empty.data(), empty.size() * sizeof(FluidParticle));
        auto previous = upload(empty.data(), empty.size() * sizeof(FluidParticle));
        auto bins = upload(emptyOffsets.data(), emptyOffsets.size() * sizeof(uint32_t));
        FluidCollider shelf{};
        XMStoreFloat4x4(&shelf.worldToLocal, XMMatrixTranslation(0, -1.15f, 0));
        shelf.centerRestitution = {0, 1.15f, 0, 0};
        shelf.extentType = {5, .15f, 2, 1};
        shelf.angularSlip.w = 1;
        auto solid = upload(&shelf, sizeof(shelf));
        gpu::transition(ctx.cmd.Get(), solid.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto uniforms = gpu::buffer(ctx.device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, L"Free-water fixture constants");
        FluidSimulationConstants fc{};
        fc.minimumCell = {-4, 0, -4, .25f};
        fc.maximumRadius = {4, 2, 4, .025f};
        fc.gravityDt = {0, -9.81f, 0, 1.f / 120};
        fc.counts = {sourceDesc.maxParticles, 0, 1, 0};
        fc.initialLattice = {1, 1, 1, 1}; // zero-size centre lattice: no boundary samples
        fc.initialMinimum.w = source.sampleVolume();
        fc.grid = grid;
        fc.emitterOriginRadius = {-4.7f, 1.7f, 0, .3f};
        fc.emitterVelocity = {.3f, 0, 0, 0};
        fc.collision.x = 1;
        memcpy(uniforms.mapped, &fc, sizeof(fc));
        FluidGpuView v{};
        v.particles = particles.resource.Get(); v.previousPositions = previous.resource.Get();
        v.offsets = v.indices = v.meshPhi = bins.resource.Get();
        v.colliderAddress = solid.resource->GetGPUVirtualAddress();
        source.reset(ctx.cmd.Get());
        for (uint32_t i = 0; i < 120; ++i) {
            source.reseed(ctx.cmd.Get(), v, uniforms.resource.Get());
            source.advanceFreeWater(ctx.cmd.Get(), v, uniforms.resource.Get(), 0, i == 0 ? 32 : 0, 0);
            source.advance(ctx.cmd.Get());
            source.couple(ctx.cmd.Get(), v, uniforms.resource.Get());
        }
        source.recordReadback(ctx.cmd.Get());ctx.finish();source.collect();
        require(source.freeWaterSamples() == 32 && source.receivedSamples() == 0 && source.addedVolume() == 0,
                "Blocked source raised the basin without delivering water");
        auto held = ctx.read<FluidParticle>(particles.resource.Get(), sourceDesc.maxParticles);
        for (const auto &p : held)
            if (p.velocityFlags.w == 2)
                require(p.positionRadius.y >= 1.32f, "Free water passed through a solid shelf");
        // Remove the obstruction; the same previously emitted particles must fall
        // and deliver their mass, even though the source has been closed for a second.
        fc.collision.x = 0;
        memcpy(uniforms.mapped, &fc, sizeof(fc));
        for (uint32_t i = 0; i < 120; ++i) {
            source.reseed(ctx.cmd.Get(), v, uniforms.resource.Get());
            source.advanceFreeWater(ctx.cmd.Get(), v, uniforms.resource.Get(), 0, 0, 0);
            source.advance(ctx.cmd.Get());
            source.couple(ctx.cmd.Get(), v, uniforms.resource.Get());
        }
        source.recordReadback(ctx.cmd.Get());ctx.finish();source.collect();
        require(source.freeWaterSamples() == 0 && source.receivedSamples() == 32,
                "Emitted particles did not transfer into the wave domain");
        require(std::abs(source.addedVolume() - 32.0 * source.sampleVolume()) < 1e-10,
                "Particle-to-wave transfer lost mass");
        auto received = ctx.read<XMFLOAT4>(source.surface(), P * P);
        double mean = 0;for (const auto &p : received) mean += p.x / (P * P);
        require(std::abs(mean - (.8 + source.addedVolume() / 100)) < 2e-6,
                "Rendered basin mean disagrees with transferred particle volume");
        source.prepareTransfers(1.f / 60, false);
        source.rebase(ctx.cmd.Get());
        source.couple(ctx.cmd.Get(), v, uniforms.resource.Get());
        auto rebased = ctx.read<XMFLOAT4>(source.surface(), P * P);
        for (uint32_t i = 0; i < P * P; ++i)
            require(std::abs(rebased[i].x - received[i].x) < 2e-6,
                    "Rebasing counted incoming water twice or lost the surface");
        source.reset(ctx.cmd.Get());source.recordReadback(ctx.cmd.Get());ctx.finish();source.collect();
        require(source.receivedSamples() == 0 && source.depth() == sourceDesc.initialDepth,
                "Reset retained transferred source volume");
        fc.collision.x = 1;
        memcpy(uniforms.mapped, &fc, sizeof(fc));
        source.reseed(ctx.cmd.Get(), v, uniforms.resource.Get());
        source.advanceFreeWater(ctx.cmd.Get(), v, uniforms.resource.Get(), 0, sourceDesc.maxParticles + 1, 0);
        source.recordReadback(ctx.cmd.Get());ctx.finish();source.collect();
        require(source.rejectedSamples() > 0 && source.full() && source.freeWaterSamples() == source.emittedSamples(),
                "Exhausted particle pool lost water instead of closing source admission");
        std::cout << "PASS free-water source, solid obstruction, delayed arrival, mass transfer and rebase/reset\n";
    }
    std::vector<XMFLOAT2> order2;
    for (unsigned order : {2u, 3u}) {
        d.hamiltonian.epsilon = .8f;
        d.hamiltonian.order = order;
        HamiltonianWave nonlinear(ctx.device.Get(), argv[1], d, grid);
        nonlinear.reset(ctx.cmd.Get());
        for (uint32_t i = 0; i < steps; ++i)
            nonlinear.advance(ctx.cmd.Get());
        auto result = ctx.read<XMFLOAT2>(nonlinear.canonicalState(), 2 * NN);
        double difference = 0;
        for (uint32_t i = 0; i < 2 * NN; ++i) {
            require(std::isfinite(result[i].x) && std::isfinite(result[i].y),
                    "Nonlinear state is not finite");
            difference += std::norm(c(result[i]) - c(state[i]));
        }
        require(std::sqrt(difference) / NN > 1e-5, "Nonlinear solver collapsed to linear waves");
        require(std::abs(c(result[4])) / NN > 1e-5, "Nonlinear second harmonic missing");
        if (order == 2)
            order2 = result;
        else {
            double delta = 0;
            for (uint32_t i = 0; i < 2 * NN; ++i)
                delta += std::norm(c(result[i]) - c(order2[i]));
            require(std::sqrt(delta) / NN > 1e-7, "HOS-3 correction is missing");
        }
        nonlinear.reset(ctx.cmd.Get());
        auto reset = ctx.read<XMFLOAT2>(nonlinear.canonicalState(), 2 * NN);
        for (uint32_t i = 0; i < 2 * NN; ++i)
            require(std::abs(c(reset[i]) - c(initial[i])) < 1e-5, "Reset retained spectral or AB2 history");
        std::cout << "PASS HOS-" << order << " nonlinear response, second harmonic and reset\n";
    }
    // A manufactured connected 3D surface exercises the production extraction,
    // relaxation and inverse DNO without invoking the wave equations on the CPU.
    d.hamiltonian.epsilon = .2f;
    d.hamiltonian.relaxation = 10;
    d.hamiltonian.order = 2;
    HamiltonianWave coupled(ctx.device.Get(), argv[1], d, grid);
    std::vector<FluidParticle> particles;
    std::vector<uint32_t> offsets(grid.w + 1), indices;
    for (uint32_t z = 0; z < grid.z; ++z)
        for (uint32_t y = 0; y < grid.y; ++y)
            for (uint32_t x = 0; x < grid.x; ++x) {
                uint32_t id = (z * grid.y + y) * grid.x + x;
                offsets[id] = uint32_t(particles.size());
                if (y == 3) {
                    float px = -4 + (x + .5f) * .25f, pz = -4 + (z + .5f) * .25f;
                    FluidParticle p{};
                    p.positionRadius = {px, .825f + .05f * std::exp(-(px * px + pz * pz)), pz, .025f};
                    p.velocityFlags.w = 1;
                    p.apic0.w = 1;
                    indices.push_back(uint32_t(particles.size()));
                    particles.push_back(p);
                }
            }
    offsets.back() = uint32_t(particles.size());
    std::vector<gpu::Buffer> uploads;
    auto upload = [&](const void *data, uint64_t size) {
        auto u = gpu::buffer(ctx.device.Get(), size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_GENERIC_READ, L"Wave test staging");
        memcpy(u.mapped, data, size);
        auto b = gpu::buffer(ctx.device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST,
                             L"Wave test input");
        ctx.cmd->CopyBufferRegion(b.resource.Get(), 0, u.resource.Get(), 0, size);
        gpu::transition(ctx.cmd.Get(), b.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uploads.push_back(std::move(u));
        return b;
    };
    auto ps = upload(particles.data(), particles.size() * sizeof(FluidParticle));
    auto os = upload(offsets.data(), offsets.size() * 4);
    auto ids = upload(indices.data(), indices.size() * 4);
    auto constants = gpu::buffer(ctx.device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, L"Wave test fluid constants");
    FluidSimulationConstants fc{};
    fc.minimumCell = {-4, 0, -4, .25f};
    fc.maximumRadius = {4, 2, 4, .025f};
    fc.grid = grid;
    memcpy(constants.mapped, &fc, sizeof(fc));
    FluidGpuView view{};
    view.particles = ps.resource.Get();
    view.offsets = os.resource.Get();
    view.indices = ids.resource.Get();
    view.previousPositions = ps.resource.Get();
    view.meshPhi = ps.resource.Get();
    view.colliderAddress = ps.resource->GetGPUVirtualAddress();
    coupled.reset(ctx.cmd.Get());
    for (uint32_t i = 0; i < 24; ++i)
        coupled.advance(ctx.cmd.Get());
    auto before = ctx.read<XMFLOAT2>(coupled.canonicalState(), 2 * NN);
    coupled.couple(ctx.cmd.Get(), view, constants.resource.Get());
    auto after = ctx.read<XMFLOAT2>(coupled.canonicalState(), 21 * NN);
    require(std::abs(c(after[0])) < 1e-6, "3D coupling changed mean basin volume");
    double heightChange = 0, potentialChange = 0, residual = 0, scale = 0;
    for (uint32_t i = 0; i < NN; ++i) {
        heightChange += std::norm(c(after[i]) - c(before[i]));
        potentialChange += std::norm(c(after[NN + i]) - c(before[NN + i]));
        if (i) {
            residual += std::norm(c(after[12 * NN + i]) - c(after[20 * NN + i]));
            scale += std::norm(c(after[20 * NN + i]));
        }
    }
    require(std::sqrt(heightChange) / NN > 1e-4, "3D height did not feed back into the wave state");
    require(std::sqrt(potentialChange) / NN > 1e-7,
            "Canonical potential was not reconstituted after height feedback");
    require(std::sqrt(residual / std::max(scale, 1e-15)) < 2e-3,
            "Inverse DNO failed to preserve kinematic velocity");
    coupled.recordReadback(ctx.cmd.Get());
    ctx.finish();
    coupled.collect();
    std::cout << "PASS two-way canonical feedback; inverse DNO relative residual="
              << std::sqrt(residual / scale) << '\n';
    {
        // A quiet full-volume particle half-space must not manufacture a hump
        // merely because it overlaps a flat wave plane. This used to happen
        // when particle extrema and the render-kernel radius disagreed.
        FluidSystemDesc flatDesc=d;
        flatDesc.hamiltonian.amplitude=flatDesc.hamiltonian.epsilon=0;
        HamiltonianWave flat(ctx.device.Get(),argv[1],flatDesc,grid);
        std::vector<FluidParticle> rest;
        std::vector<uint32_t> restOffsets(grid.w+1),restIndices;
        for(uint32_t z=0;z<grid.z;++z)for(uint32_t y=0;y<grid.y;++y)for(uint32_t x=0;x<grid.x;++x) {
            uint32_t cell=(z*grid.y+y)*grid.x+x;restOffsets[cell]=uint32_t(rest.size());
            for(uint32_t sz=0;sz<5;++sz)for(uint32_t sy=0;sy<5;++sy)for(uint32_t sx=0;sx<5;++sx) {
                float py=(y+(sy+.5f)/5)*.25f;if(py>=flatDesc.initialDepth)continue;
                FluidParticle p{};p.positionRadius={-4+(x+(sx+.5f)/5)*.25f,py,-4+(z+(sz+.5f)/5)*.25f,.025f};
                p.velocityFlags.w=1;p.apic0.w=1;restIndices.push_back(uint32_t(rest.size()));rest.push_back(p);
            }
        }
        restOffsets.back()=uint32_t(rest.size());
        auto rp=upload(rest.data(),rest.size()*sizeof(FluidParticle));
        auto ro=upload(restOffsets.data(),restOffsets.size()*4);
        auto ri=upload(restIndices.data(),restIndices.size()*4);
        auto rv=view;rv.particles=rp.resource.Get();rv.offsets=ro.resource.Get();rv.indices=ri.resource.Get();
        flat.reset(ctx.cmd.Get());
        for(uint32_t i=0;i<24;++i)flat.couple(ctx.cmd.Get(),rv,constants.resource.Get());
        auto surface=ctx.read<XMFLOAT4>(flat.surface(),P*P);
        float maximumError=0;for(const auto &v:surface)maximumError=std::max(maximumError,std::abs(v.x-flatDesc.initialDepth));
        require(maximumError<.0025f,"A quiet particle/wave interface manufactured a raised region");
        std::cout << "PASS flat particle/wave equilibrium; maximum height error=" << maximumError << " m\n";
    }
    {
        FluidSystemDesc a = d;
        a.hamiltonian.adaptive = true;
        a.hamiltonian.amplitude = a.hamiltonian.epsilon = a.hamiltonian.relaxation = 0;
        a.minimum = a.waveMinimum = {-8, 0, -8};
        a.maximum = a.waveMaximum = {8, 2, 8};
        a.initialParticles = 10000; a.maxParticles = 15000;
        const XMUINT4 ag{64, 8, 64, 64 * 8 * 64};
        HamiltonianWave adaptive(ctx.device.Get(), argv[1], a, ag);
        std::vector<FluidParticle> empty(a.maxParticles);
        std::vector<uint32_t> emptyOffsets(ag.w + 1);
        auto ap = upload(empty.data(), empty.size() * sizeof(FluidParticle));
        auto ao = upload(emptyOffsets.data(), emptyOffsets.size() * 4);
        auto previous = upload(empty.data(), empty.size() * sizeof(FluidParticle));
        auto sphere = [](float x, float y, float z) {
            FluidCollider result{};
            XMStoreFloat4x4(&result.worldToLocal, XMMatrixTranslation(-x, -y, -z));
            result.centerRestitution = {x,y,z,0};
            result.extentType = {.6f,.6f,.6f,0};result.angularSlip.w = 1;
            return result;
        };
        const FluidCollider wet[]{sphere(-4,.6f,-4),sphere(4,.6f,4)};
        auto bodies = upload(wet, sizeof(wet));
        gpu::transition(ctx.cmd.Get(), bodies.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FluidSimulationConstants ac{};
        ac.minimumCell={-8,0,-8,.25f};ac.maximumRadius={8,2,8,.025f};
        ac.gravityDt={0,-9.81f,0,1.f/120};ac.counts.x=a.maxParticles;ac.grid=ag;
        ac.initialLattice={1,1,1,1}; // zero-size centre lattice, outside both regions
        ac.collision.x=2;
        memcpy(constants.mapped,&ac,sizeof(ac));
        FluidGpuView av{};
        av.particles=ap.resource.Get();av.previousPositions=previous.resource.Get();
        av.offsets=ao.resource.Get();av.indices=ids.resource.Get();av.meshPhi=ps.resource.Get();
        av.colliderAddress=bodies.resource->GetGPUVirtualAddress();
        auto regions = [&] {
            return ctx.read<XMFLOAT4>(adaptive.surface(), (HamiltonianWave::layers+3)*P*P);
        };
        auto edge = [&](const std::vector<XMFLOAT4> &values,float x,float z) {
            // Independent bilinear read of the production selection field.
            float gx=(x+8)*P/16-.5f,gz=(z+8)*P/16-.5f;
            uint32_t ix=uint32_t(gx),iz=uint32_t(gz);float fx=gx-ix,fz=gz-iz;
            uint32_t base=(HamiltonianWave::layers+1)*P*P+iz*P+ix;
            return (1-fz)*((1-fx)*values[base].x+fx*values[base+1].x)+
                   fz*((1-fx)*values[base+P].x+fx*values[base+P+1].x);
        };
        adaptive.reset(ctx.cmd.Get());adaptive.reseed(ctx.cmd.Get(),av,constants.resource.Get(),true);
        auto selected=regions();
        require(edge(selected,-4,-4)>2 && edge(selected,4,4)>2,
                "Separated submerged bodies did not both receive full 3D regions");
        require(edge(selected,0,0)<0 && edge(selected,-4,4)<0,
                "Calm water between disconnected bodies was promoted to full 3D");
        // A dry body must not demand fluid work, even with the camera unchanged.
        const FluidCollider oneDry[]{wet[0],sphere(4,3,4)};
        auto dryBodies=upload(oneDry,sizeof(oneDry));
        gpu::transition(ctx.cmd.Get(),dryBodies.resource.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        av.colliderAddress=dryBodies.resource->GetGPUVirtualAddress();
        adaptive.reset(ctx.cmd.Get());adaptive.reseed(ctx.cmd.Get(),av,constants.resource.Get(),true);
        selected=regions();
        require(edge(selected,-4,-4)>2 && edge(selected,4,4)<0,
                "A dry rigid body unnecessarily activated a 3D fluid region");
        // Removing all bodies retains the outgoing wake briefly, then lets it
        // return to the wave representation without an abrupt frame switch.
        ac.collision.x=0;memcpy(constants.mapped,&ac,sizeof(ac));
        adaptive.reseed(ctx.cmd.Get(),av,constants.resource.Get());
        selected=regions();require(edge(selected,-4,-4)>2,"Region demotion discarded wake history immediately");
        for(uint32_t i=0;i<800;++i)adaptive.reseed(ctx.cmd.Get(),av,constants.resource.Get());
        adaptive.recordReadback(ctx.cmd.Get());ctx.finish();adaptive.collect();
        require(adaptive.activeColumns()==0 && adaptive.regionChanges()>0,
                "Calm regions did not return to Hamiltonian water");
        // Strong vorticity can select 3D water without any body or emitter.
        empty[0].positionRadius={4.25f,.4f,-3.75f,.025f};
        empty[0].velocityFlags.w=1;empty[0].apic0={0,-8,0,1};
        uint32_t flowCell=(17*ag.y+1)*ag.x+49;
        for(uint32_t i=flowCell+1;i<=ag.w;++i)emptyOffsets[i]=1;
        auto flowParticles=upload(empty.data(),empty.size()*sizeof(FluidParticle));
        auto flowOffsets=upload(emptyOffsets.data(),emptyOffsets.size()*4);
        const uint32_t zero=0;auto flowIndices=upload(&zero,sizeof(zero));
        av.particles=flowParticles.resource.Get();av.offsets=flowOffsets.resource.Get();av.indices=flowIndices.resource.Get();
        adaptive.reset(ctx.cmd.Get());adaptive.reseed(ctx.cmd.Get(),av,constants.resource.Get());
        selected=regions();
        require(edge(selected,4.25f,-3.75f)>2,"Vortical fluid without a body did not activate a 3D region");
        std::cout << "PASS adaptive regions: separated wet bodies, dry exclusion, calm demotion and vorticity\n";
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
