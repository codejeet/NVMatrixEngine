#include "../src/gpu_cuda_interop.h"
#include "cuda_fluid_fixture.h"
#include "../src/fluid/fluid_uniforms.h"
#include "../src/fluid/cuda/fluid_cuda_transfer.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
using namespace lab;
using namespace DirectX;
using Microsoft::WRL::ComPtr;
namespace {
struct Particle {
    XMFLOAT4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
static_assert(sizeof(Particle) == 80);
void require(bool condition, const char *text) {
    if (!condition)
        throw std::runtime_error(text);
}
using lab::cuda_test::Fixture;
struct TransferOwner {
    gpu::CudaInterop &interop;
    cuda_fluid::Transfer *pointer;
    ~TransferOwner() {
        interop.drainForTeardown();
        cuda_fluid::destroyTransfer(pointer);
    }
};
struct Case {
    const char *name;
    uint32_t nx, ny, nz, capacity;
    bool flip, empty, deterministic, weighted, perturb;
    float cellSize = .125f;
};
void run(Fixture &d, const Case &test) {
    const uint32_t cells = test.nx * test.ny * test.nz,
                   faces = 3 * (test.nx + 1) * (test.ny + 1) * (test.nz + 1);
    const uint32_t capacity = test.capacity;
    FluidSimulationConstants f{};
    const float h = test.cellSize;
    f.minimumCell = {-3.25f, .125f, -1.75f, h};
    f.maximumRadius = {f.minimumCell.x + test.nx * h, f.minimumCell.y + test.ny * h,
                       f.minimumCell.z + test.nz * h, .02f};
    f.gravityDt = {0, -9.81f, 1.1f, 1.f / 120};
    f.grid = {test.nx, test.ny, test.nz, cells};
    f.counts = {capacity, capacity, 0, 0};
    f.solver = {998.207f, .95f, test.flip ? 1.f : 0.f, .125f};
    std::vector<Particle> initial(capacity);
    uint32_t rng = 7717, active = 0;
    auto random = [&] {
        rng = rng * 1664525u + 1013904223u;
        return float(rng >> 8) * (1.f / 16777216);
    };
    for (uint32_t i = 0; i < capacity; ++i) {
        auto &p = initial[i];
        float x = random(), y = random(), z = random();
        // Nonuniform occupancy, inactive holes, fractional mass and faces close
        // to the domain boundary. Last cases span several prefix-scan blocks.
        p.positionRadius = {f.minimumCell.x + .02f + x * (test.nx * h - .04f),
                            f.minimumCell.y + .02f + y * (test.ny * h - .04f),
                            f.minimumCell.z + .02f + z * (test.nz * h - .04f), .02f};
        p.velocityFlags = {.7f + .2f * x - .4f * y, -.2f + .4f * x + .1f * z, .1f - .2f * z,
                           (!test.empty && i % 7 != 0) ? 1.f : 0.f};
        p.apic0 = {.2f, -.4f, 0, test.weighted ? (i % 3 == 0 ? .25f : (i % 3 == 1 ? .5f : 2.f)) : 1.f};
        p.apic1 = {.4f, 0, .1f, 17};
        p.apic2 = {0, 0, -.2f, 29};
        active += p.velocityFlags.w != 0;
    }
    const std::array<uint64_t, 18> sizes{uint64_t(capacity) * 80,
                                         uint64_t(cells) * 4,
                                         uint64_t(cells + 1) * 4,
                                         uint64_t(cells) * 4,
                                         uint64_t(capacity) * 4,
                                         uint64_t((cells + 255) / 256) * 4,
                                         uint64_t(faces) * 16,
                                         uint64_t(cells) * 4,
                                         uint64_t(cells) * 4,
                                         uint64_t(cells) * 16,
                                         uint64_t(faces) * 16,
                                         uint64_t(cells) * 16,
                                         uint64_t(capacity) * 16,
                                         uint64_t(cells) * 16,
                                         uint64_t(cells) * 16,
                                         uint64_t(cells) * 4,
                                         256,
                                         256};
    std::array<gpu::Buffer, 18> ref, cuda;
    for (uint32_t i = 0; i < 18; ++i) {
        ref[i] = d.make(sizes[i]);
        cuda[i] = d.make(sizes[i], true);
    }
    auto frame = gpu::buffer(d.device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD);
    std::memcpy(frame.mapped, &f, sizeof(f));
    auto upload = gpu::buffer(d.device.Get(), sizes[0], D3D12_HEAP_TYPE_UPLOAD);
    std::memcpy(upload.mapped, initial.data(), size_t(sizes[0]));
    constexpr uint32_t registers[]{0, 1, 2, 3, 4, 6};
    std::vector<gpu::CudaInterop::Binding> bindings;
    for (auto r : registers)
        bindings.push_back({cuda[r].resource.Get(), sizes[r]});
    gpu::CudaInterop interop(d.device.Get(), bindings);
    void *pointers[cuda_fluid::BufferCount]{};
    for (uint32_t i = 0; i < 6; ++i)
        pointers[i] = interop.pointers()[i];
    cuda_fluid::Config config{};
    config.nx = test.nx;
    config.ny = test.ny;
    config.nz = test.nz;
    config.capacity = capacity;
    config.deterministic = test.deterministic;
    TransferOwner transfer{interop, cuda_fluid::createTransfer(config, pointers)};
    const uint64_t binBytes = sizes[1] + sizes[2] + sizes[4],
                   maxBytes = std::max({binBytes, sizes[0], sizes[6]});
    auto readback = gpu::buffer(d.device.Get(), maxBytes * 2, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto read = [&](uint64_t size) {
        d.submit();
        void *mapped = nullptr;
        D3D12_RANGE range{0, size_t(maxBytes + size)};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Transfer audit map");
        std::vector<std::byte> result(size_t(maxBytes + size));
        std::memcpy(result.data(), mapped, result.size());
        D3D12_RANGE written{0, 0};
        readback.resource->Unmap(0, &written);
        interop.collect();
        return result;
    };
    d.begin();
    for (auto *buffers : {&ref, &cuda}) {
        gpu::transition(d.cmd.Get(), (*buffers)[0].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
        d.cmd->CopyBufferRegion((*buffers)[0].resource.Get(), 0, upload.resource.Get(), 0, sizes[0]);
        gpu::transition(d.cmd.Get(), (*buffers)[0].resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    d.bind(frame.resource.Get(), ref);
    const uint32_t groups[]{(cells + 255) / 256, (capacity + 255) / 256, (cells + 255) / 256, 1,
                            (cells + 255) / 256, (capacity + 255) / 256, (cells + 255) / 256};
    for (uint32_t i = 0; i < 7; ++i)
        d.pass(i, groups[i]);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cuda_fluid::enqueueTransfer(transfer.pointer, stream, &f, cuda_fluid::TransferStage::Bin);
    });
    for (uint32_t b = 0; b < 2; ++b) {
        auto &buffers = b ? cuda : ref;
        uint64_t offset = b * maxBytes;
        for (uint32_t r : {1u, 2u, 4u}) {
            d.copyOut(buffers[r].resource.Get(), readback.resource.Get(), sizes[r], offset);
            offset += sizes[r];
        }
    }
    auto bins = read(binBytes);
    auto *a = reinterpret_cast<const uint32_t *>(bins.data()),
         *b = reinterpret_cast<const uint32_t *>(bins.data() + maxBytes);
    require(!std::memcmp(a, b, size_t(sizes[1] + sizes[2])), "CUDA/HLSL counts or offsets differ");
    const uint32_t *offsets = a + cells, *ai = offsets + cells + 1, *bi = b + cells + cells + 1;
    require(offsets[cells] == active, "Binning lost/duplicated active particles");
    std::vector<bool> seen(capacity);
    for (uint32_t c = 0; c < cells; ++c) {
        std::vector<uint32_t> lhs(ai + offsets[c], ai + offsets[c + 1]),
            rhs(bi + offsets[c], bi + offsets[c + 1]);
        if (!test.deterministic) {
            std::sort(lhs.begin(), lhs.end());
            std::sort(rhs.begin(), rhs.end());
        }
        require(lhs == rhs, "CUDA/HLSL bin membership/order differs");
        for (auto id : rhs) {
            require(id < capacity && !seen[id] && initial[id].velocityFlags.w != 0,
                    "Invalid CUDA bin ownership");
            seen[id] = true;
        }
    }
    d.begin();
    d.bind(frame.resource.Get(), ref);
    d.pass(7, (faces + 127) / 128);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cuda_fluid::enqueueTransfer(transfer.pointer, stream, &f, cuda_fluid::TransferStage::ToGrid);
    });
    d.copyOut(ref[6].resource.Get(), readback.resource.Get(), sizes[6], 0);
    d.copyOut(cuda[6].resource.Get(), readback.resource.Get(), sizes[6], maxBytes);
    auto grid = read(sizes[6]);
    double faceError = 0;
    auto compare = [&](const auto &data, uint64_t bytes, double absolute, double relative) {
        const auto *lhs = reinterpret_cast<const float *>(data.data()),
                   *rhs = reinterpret_cast<const float *>(data.data() + maxBytes);
        double maximum = 0;
        for (size_t i = 0; i < bytes / 4; ++i) {
            double error = std::abs(double(lhs[i]) - rhs[i]);
            maximum = std::max(maximum, error);
            require(std::isfinite(lhs[i]) && std::isfinite(rhs[i]) &&
                        error <= absolute + relative * std::abs(double(lhs[i])),
                    "CUDA/HLSL numerical transfer mismatch");
        }
        return maximum;
    };
    faceError = compare(grid, sizes[6], .00003, .00001);
    d.begin();
    if (test.perturb) {
        d.bind(frame.resource.Get(), ref);
        d.pass(9, (faces + 127) / 128);
        d.bind(frame.resource.Get(), cuda);
        d.pass(9, (faces + 127) / 128);
    }
    d.bind(frame.resource.Get(), ref);
    d.pass(8, (capacity + 127) / 128);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cuda_fluid::enqueueTransfer(transfer.pointer, stream, &f, cuda_fluid::TransferStage::ToParticles);
    });
    d.copyOut(ref[0].resource.Get(), readback.resource.Get(), sizes[0], 0);
    d.copyOut(cuda[0].resource.Get(), readback.resource.Get(), sizes[0], maxBytes);
    auto particles = read(sizes[0]);
    double particleError = compare(particles, sizes[0], .00005, .00001);
    const auto *result = reinterpret_cast<const Particle *>(particles.data() + maxBytes);
    for (uint32_t i = 0; i < capacity; ++i) {
        if (!initial[i].velocityFlags.w)
            require(!std::memcmp(&initial[i], result + i, sizeof(Particle)), "Inactive particle changed");
        require(result[i].apic0.w == initial[i].apic0.w && result[i].apic1.w == 17 && result[i].apic2.w == 29,
                "APIC side-channel/particle mass corrupted");
    }
    std::cout << "{\"case\":\"" << test.name << "\",\"cells\":" << cells << ",\"particles\":" << capacity
              << ",\"active\":" << active << ",\"maxFaceError\":" << faceError
              << ",\"maxParticleError\":" << particleError << ",\"pass\":true}\n";
}
} // namespace
int main(int argc, char **argv) try {
    require(argc == 2, "Pass the CUDA fixture runtime folder");
    Fixture d(argv[1]);
    const Case cases[]{{"empty", 5, 4, 3, 257, false, true, true, false, false},
                       {"apic-sorted", 8, 6, 4, 1537, false, false, true, false, false},
                       {"flip-delta", 9, 7, 3, 2051, true, false, true, false, true},
                       {"apic-weighted", 13, 6, 5, 3123, false, false, true, true, true},
                       {"apic-atomic-scatter", 17, 8, 4, 4103, false, false, false, false, true},
                       {"prefix-many-blocks", 65, 9, 7, 8197, true, false, false, false, true},
                       {"nonbinary-cell-size", 17, 13, 7, 16387, false, false, true, false, true, .08f},
                       {"100k-active", 64, 32, 24, 116669, false, false, false, false, true, .08f}};
    for (const auto &test : cases)
        run(d, test);
    std::cout << "PASS CUDA transfers: isolated production HLSL bin/P2G/G2P parity\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA transfers: " << e.what() << '\n';
    return 1;
}
