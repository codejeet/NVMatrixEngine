#include "../src/fluid/fluid_spacetime.h"
#include "../src/fluid/fluid_uniforms.h"
#include "fluid_test_binning.h"
#include <dxgi1_6.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
using namespace lab;
using namespace DirectX;
using Microsoft::WRL::ComPtr;
struct Particle {
    XMFLOAT4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
static_assert(sizeof(Particle) == 80);
void require(bool test, const char *message) {
    if (!test)
        throw std::runtime_error(message);
}
uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}
double kernel(double tau) {
    if (tau < -.5 || tau > .5)
        return 0;
    const double q = 1 - (tau - .5) * (tau - .5);
    return 35. / 16 * q * q * q;
}
double quadratic(double q) {
    q = std::abs(q);
    return q < .5 ? .75 - q * q : (q < 1.5 ? .5 * (1.5 - q) * (1.5 - q) : 0);
}
struct Event {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() {
        if (value)
            CloseHandle(value);
    }
};

int main(int argc, char **argv) try {
    require(argc == 2, "Pass the built lab runtime folder");
    ComPtr<ID3D12Debug> debug;
    bool debugEnabled = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugEnabled)
        debug->EnableDebugLayer();
    std::cout << "D3D12 debug layer: " << (debugEnabled ? "enabled" : "unavailable") << '\n';
    ComPtr<IDXGIFactory6> factory;
    gpu::check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Spacetime test DXGI");
    ComPtr<ID3D12Device> device;
    for (uint32_t i = 0; !device; i++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            throw std::runtime_error("No hardware DX12 adapter");
        DXGI_ADAPTER_DESC1 d{};
        gpu::check(adapter->GetDesc1(&d), "Adapter description");
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    }
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    gpu::check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "Spacetime queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "Spacetime allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;
    gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd)),
               "Spacetime list");
    gpu::check(cmd->Close(), "Initial close");
    ComPtr<ID3D12Fence> fence;
    gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Spacetime fence");
    Event event;
    require(event.value != nullptr, "Completion event");
    uint64_t serial = 0;
    const XMUINT4 grid{24, 24, 24, 24 * 24 * 24};
    const uint32_t slots = 513, faceCount = 3 * 25 * 25 * 25;
    const XMFLOAT3 velocity{6, -.25f, .5f};
    auto make = [&](uint64_t bytes) {
        return gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    FluidSpacetime st(device.Get(), argv[1], grid, slots);
    FluidTestBinning bins(device.Get(), argv[1], grid.w, slots);
    auto frame = gpu::buffer(device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD);
    FluidSimulationConstants c{};
    c.minimumCell = {0, 0, 0, 1};
    c.maximumRadius = {24, 24, 24, .02f};
    c.counts = {slots, slots - 1, 0, 0};
    c.grid = grid;
    c.solver = {1000, .95f, 0, .125f};
    memcpy(frame.mapped, &c, sizeof(c));
    auto particles = make(slots * 80), faces = make(faceCount * 16), offsets = make((grid.w + 1) * 4),
         indices = make(slots * 4);
    FluidSpacetime::View view{frame.resource.Get(), particles.resource.Get(), offsets.resource.Get(),
                              indices.resource.Get(), faces.resource.Get()};
    std::vector<Particle> initial(slots);
    for (uint32_t i = 0; i < slots - 1; i++) {
        auto &p = initial[i];
        p.positionRadius = {6.25f + float(i % 8) * .5f, 6.25f + float(i / 8 % 8) * .5f,
                            6.25f + float(i / 64) * .5f, .02f};
        p.velocityFlags = {velocity.x, velocity.y, velocity.z, 1};
        p.apic0 = {.015f, -.025f, .01f, 1};
        p.apic1 = {-.02f, .01f, 0, 0};
        p.apic2 = {0, .02f, -.015f, 0};
        // Non-dyadic physical sample weights must participate in both numerator and denominator.
        p.apic0.w = .75f + float(i % 7) * .083f;
    }
    std::vector<XMFLOAT4> gridVelocity(faceCount);
    for (uint32_t i = 0; i < faceCount; i++) {
        float v = (&velocity.x)[i / (faceCount / 3)];
        gridVelocity[i] = {v, v, 1, 1};
    }
    auto uploadParticles = gpu::buffer(device.Get(), slots * 80, D3D12_HEAP_TYPE_UPLOAD);
    auto uploadFaces = gpu::buffer(device.Get(), faceCount * 16, D3D12_HEAP_TYPE_UPLOAD);
    memcpy(uploadParticles.mapped, initial.data(), slots * 80);
    memcpy(uploadFaces.mapped, gridVelocity.data(), faceCount * 16);
    const uint64_t sampleBytes = slots * (80ull * 2 + 4) + 16;
    auto readback = gpu::buffer(device.Get(), sampleBytes * 8 + faceCount * 24ull, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> firstRun;
    for (uint32_t test = 0; test < 5; test++) {
        gpu::check(allocator->Reset(), "Allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "List reset");
        auto upload = [&](ID3D12Resource *dst, ID3D12Resource *src, uint64_t bytes) {
            gpu::transition(cmd.Get(), dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(dst, 0, src, 0, bytes);
            gpu::transition(cmd.Get(), dst, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        upload(particles.resource.Get(), uploadParticles.resource.Get(), slots * 80);
        upload(faces.resource.Get(), uploadFaces.resource.Get(), faceCount * 16);
        auto copy = [&](ID3D12Resource *src, uint64_t offset, uint64_t bytes) {
            gpu::transition(cmd.Get(), src, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(), offset, src, 0, bytes);
            gpu::transition(cmd.Get(), src, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        auto capture = [&](uint32_t stage) {
            uint64_t o = stage * sampleBytes;
            copy(particles.resource.Get(), o, slots * 80);
            o += slots * 80;
            copy(st.renderParticles(), o, slots * 80);
            o += slots * 80;
            copy(st.timeOffsets(), o, slots * 4);
            o += slots * 4;
            copy(st.diagnostics(), o, 16);
        };
        auto rejects = [&](auto operation) {
            bool rejected = false;
            try {
                operation();
            } catch (const std::runtime_error &) {
                rejected = true;
            }
            require(rejected, "Invalid spacetime API input accepted");
        };
        if (test == 0)
            rejects([&] { st.synchronize(cmd.Get(), view); });
        st.reset(cmd.Get(), view);
        st.synchronize(cmd.Get(), view);
        capture(0);
        rejects([&] { st.seed(cmd.Get(), view, slots, 1); });
        rejects([&] { st.deposit(cmd.Get(), view, 0); });
        rejects([&] { st.advect(cmd.Get(), view, -1, 0, 0); });
        rejects([&] { st.advect(cmd.Get(), view, .1f, 0, 0, 1.01f); });
        const float steps[]{.03125f, .25f, .0078125f, .125f};
        const float strength = test == 3 ? 0.f : 1.f;
        const uint32_t seed = test == 2 ? 0x9021u : 0x5017u;
        for (uint32_t step = 0; step < 4; step++) {
            st.advect(cmd.Get(), view, steps[step], step, seed, strength);
            st.synchronize(cmd.Get(), view);
            capture(step + 1);
        }
        // Pause and rendering must not feed synchronized samples back into physics.
        st.advect(cmd.Get(), view, 0, 77, 99);
        st.synchronize(cmd.Get(), view);
        capture(5);
        st.seed(cmd.Get(), view, 7, 1);
        st.synchronize(cmd.Get(), view);
        capture(6);
        bins.record(cmd.Get(), frame.resource.Get(), particles.resource.Get(), offsets.resource.Get(),
                    indices.resource.Get());
        st.deposit(cmd.Get(), view, steps[3]);
        copy(faces.resource.Get(), 8 * sampleBytes, faceCount * 16);
        copy(st.phaseFaces(), 8 * sampleBytes + faceCount * 16, faceCount * 8);
        st.reset(cmd.Get(), view);
        st.synchronize(cmd.Get(), view);
        capture(7);
        gpu::check(cmd->Close(), "Spacetime close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Spacetime signal");
        gpu::check(fence->SetEventOnCompletion(serial, event.value), "Spacetime completion");
        require(WaitForSingleObject(event.value, 30000) == WAIT_OBJECT_0, "Spacetime hardware timeout");
        void *mapped;
        D3D12_RANGE range{0, SIZE_T(sampleBytes * 8 + faceCount * 24ull)}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Spacetime map");
        const auto *bytes = static_cast<const char *>(mapped);
        std::vector<Particle> finalParticles(slots);
        std::vector<float> finalResidual(slots);
        double maxRenderError = 0, maxTimeError = 0;
        for (uint32_t stage = 0; stage < 8; stage++) {
            const auto *p = reinterpret_cast<const Particle *>(bytes + stage * sampleBytes);
            const auto *r = p + slots;
            const auto *time = reinterpret_cast<const float *>(r + slots);
            const auto *diagnostics = reinterpret_cast<const uint32_t *>(time + slots);
            require(diagnostics[0] == 0, "Spacetime numerical failure flag");
            for (uint32_t id = 0; id < slots; id++) {
                require(std::isfinite(time[id]), "Nonfinite residual");
                if (id == slots - 1) {
                    require(std::memcmp(p + id, &initial[id], 80) == 0 && time[id] == 0,
                            "Inactive particle changed");
                    continue;
                }
                double residual = 0, advanced = 0, global = 0;
                for (uint32_t step = 0; step < std::min(stage, 4u); step++) {
                    const double dt = steps[step], speed = std::sqrt(36. + .0625 + .25);
                    const double cfl = std::min(1., speed * dt), gamma = strength * cfl * cfl * (3 - 2 * cfl);
                    const double u = (hash32(id ^ hash32(seed) ^ hash32(step)) >> 8) / 16777216.;
                    const double actual = std::clamp(dt + residual + (u - .5) * gamma * dt, 0., 2 * dt);
                    residual = dt + residual - actual;
                    advanced += actual;
                    global += dt;
                }
                if (stage >= 6 && id == 7)
                    residual = 0;
                if (stage == 7)
                    residual = 0;
                maxTimeError = std::max(maxTimeError, std::abs(time[id] - residual));
                require(std::abs(time[id] - residual) < 2e-7,
                        "GPU residual differs from independent double time oracle");
                require(std::abs(time[id]) <= .125001, "Residual exceeded maximum-step bound");
                for (uint32_t a = 0; a < 3; a++) {
                    const double expected = (&initial[id].positionRadius.x)[a] + (&velocity.x)[a] * advanced;
                    require(std::abs((&p[id].positionRadius.x)[a] - expected) < 8e-5,
                            "RK3 particle time/position mismatch");
                    const double rendered = expected + (&velocity.x)[a] * residual;
                    const double error = std::abs((&r[id].positionRadius.x)[a] - rendered);
                    maxRenderError = std::max(maxRenderError, error);
                    require(error < 8e-5, "Render cache not synchronized to global time");
                }
                require(std::memcmp(&p[id].velocityFlags, &initial[id].velocityFlags, 64) == 0,
                        "Advection altered velocity/APIC/mass");
                require(std::memcmp(&r[id].velocityFlags, &p[id].velocityFlags, 64) == 0,
                        "Rendering altered velocity/APIC/mass");
            }
            if (stage == 5)
                require(std::memcmp(bytes + 4 * sampleBytes, bytes + 5 * sampleBytes, size_t(sampleBytes)) ==
                            0,
                        "Paused frame mutated time/sample state");
            if (stage == 6) {
                memcpy(finalParticles.data(), p, slots * 80);
                memcpy(finalResidual.data(), time, slots * 4);
            }
        }
        if (test == 0)
            firstRun = finalResidual;
        if (test == 1 || test == 4)
            require(finalResidual == firstRun, "Fixed-seed GPU run is not reproducible");
        if (test == 2)
            require(finalResidual != firstRun, "Different seed did not alter temporal sampling");
        const auto *out = reinterpret_cast<const XMFLOAT4 *>(bytes + 8 * sampleBytes);
        const auto *phase = reinterpret_cast<const XMFLOAT2 *>(out + faceCount);
        double maxMassError = 0, maxVelocityError = 0;
        for (uint32_t id = 0; id < faceCount; id++) {
            const uint32_t axis = id / (faceCount / 3), k = id % (faceCount / 3),
                           coord[]{k % 25, k / 25 % 25, k / 625};
            bool valid = true;
            for (uint32_t a = 0; a < 3; a++)
                valid = valid && coord[a] < 24u + (a == axis);
            double mass = 0, momentum = 0;
            if (valid)
                for (uint32_t j = 0; j < slots - 1; j++) {
                    const auto &p = finalParticles[j];
                    double w = p.apic0.w * kernel(-double(finalResidual[j]) / steps[3]), affine = 0;
                    const auto &row = axis == 0 ? p.apic0 : axis == 1 ? p.apic1 : p.apic2;
                    for (uint32_t a = 0; a < 3; a++) {
                        const double d = coord[a] + (a == axis ? 0. : .5) - (&p.positionRadius.x)[a];
                        w *= quadratic(d);
                        affine += (&row.x)[a] * d;
                    }
                    mass += w;
                    momentum += w * ((&p.velocityFlags.x)[axis] + affine);
                }
            const double expected = mass > 1e-8 ? momentum / mass : 0;
            maxMassError = std::max(maxMassError, std::abs(out[id].z - mass));
            maxVelocityError = std::max(maxVelocityError, std::abs(out[id].x - expected));
            require(std::isfinite(out[id].x) && std::abs(out[id].z - mass) < 1e-5 &&
                        std::abs(out[id].x - expected) < 1e-5 && out[id].y == out[id].x,
                    "4D production P2G differs from independent gather");
            const double phi = std::min(1., std::sqrt(mass * .125 / .5)), beta = 1 / std::max(1000 * phi, .1);
            require(std::isfinite(phase[id].x) && std::isfinite(phase[id].y) &&
                        std::abs(phase[id].x - phi) < 2e-6 && std::abs(phase[id].y - beta) < 2e-5,
                    "Phase/density coefficient mismatch");
        }
        readback.resource->Unmap(0, &written);
        std::cout << "PASS ST transfer case " << test << " | time " << maxTimeError << " | sync position "
                  << maxRenderError << " | mass " << maxMassError << " | velocity " << maxVelocityError
                  << '\n';
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
