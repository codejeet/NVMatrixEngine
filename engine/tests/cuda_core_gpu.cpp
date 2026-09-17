#include "cuda_fluid_fixture.h"
#include "../src/gpu_cuda_interop.h"
#include "../src/fluid/fluid_colliders.h"
#include "../src/fluid/fluid_uniforms.h"
#include "../src/fluid/cuda/fluid_cuda_transfer.h"
#include "../src/fluid/cuda/fluid_cuda_grid.h"
#include "../src/fluid/cuda/fluid_cuda_collision.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <numeric>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
using namespace lab;
using namespace lab::cuda_test;
using namespace DirectX;
using cuda_fluid::GridStage;
namespace {
struct Particle {
    XMFLOAT4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
struct Case {
    const char *name;
    uint32_t collider, pressure, density, viscosity;
    bool empty = false, flip = false;
    uint32_t frames = 0, substeps = 1;
    bool moving = false, graphs = false, changeParameters = false;
    bool mixedPressure = false;
};
struct Owner {
    gpu::CudaInterop &interop;
    cuda_fluid::Transfer *transfer;
    cuda_fluid::Solver *solver = nullptr;
    ~Owner() {
        interop.drainForTeardown();
        cuda_fluid::destroyTransfer(transfer);
        cuda_fluid::destroy(solver);
    }
};
void run(Fixture &d, const Case &test) {
    constexpr uint32_t nx = 12, ny = 10, nz = 10, cells = nx * ny * nz,
                       faces = 3 * (nx + 1) * (ny + 1) * (nz + 1), ppc = 8;
    constexpr uint32_t active = (nx - 4) * (ny / 2) * (nz - 4) * ppc, capacity = active + 7;
    FluidSimulationConstants f{};
    f.minimumCell = {-.64f, .03f, -.56f, .08f};
    f.maximumRadius = {f.minimumCell.x + nx * .08f, f.minimumCell.y + ny * .08f, f.minimumCell.z + nz * .08f,
                       .02f};
    f.gravityDt = {0, -9.81f, 0, 1.f / 120};
    f.grid = {nx, ny, nz, cells};
    f.counts = {capacity, active, 0, 0};
    f.solver = {998.207f, .95f, test.flip ? 1.f : 0.f, 1.f / ppc};
    f.material = {.03f / test.viscosity, .0728f, float(test.viscosity), 0};
    std::array<FluidCollider, 16> colliders{};
    std::vector<float> mesh(7 + 16 * 16 * 16, 123.f);
    for (uint32_t z = 0; z < 16; ++z)
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) {
                float a = float(x) * .04f - .30f, b = float(y) * .04f - .30f, c = float(z) * .04f - .30f;
                mesh[7 + (z * 16 + y) * 16 + x] = std::sqrt(a * a + b * b + c * c) - .11f;
            }
    auto add = [&](uint32_t type, float x, float y, float z, XMFLOAT3 extent) {
        auto &c = colliders[f.collision.x++];
        auto world = XMMatrixRotationRollPitchYaw(type == 1 ? .13f : 0, type == 1 ? .31f : 0, 0) *
                     XMMatrixTranslation(x, y, z);
        XMStoreFloat4x4(&c.worldToLocal, XMMatrixInverse(nullptr, world));
        c.centerRestitution = {x, y, z, .15f};
        c.extentType = {extent.x, extent.y, extent.z, float(type)};
        c.velocityFriction = {.22f, .03f, -.08f, .2f};
        c.angularSlip = {.1f, 1.3f, .2f, .7f};
        c.meshMinimumSpacing = {-.30f, -.30f, -.30f, .04f};
        c.meshDimensions = {16, 16, 16, 7};
    };
    if (test.collider == 1 || test.collider == 2)
        add(test.collider == 1 ? 0 : 5, -.15f, .24f, -.16f, {.11f, 0, 0});
    if (test.collider == 3) {
        add(0, -.36f, .23f, -.18f, {.065f, 0, 0});
        add(1, -.19f, .11f, -.19f, {.065f, .105f, .07f});
        add(2, -.02f, .23f, -.20f, {.05f, .08f, 0});
        add(3, .12f, .23f, -.21f, {.05f, .09f, 0});
        add(4, 0, .07f, 0, {});
        colliders[4].velocityFriction = {0, 0, 0, .3f};
        colliders[4].angularSlip = {0, 0, 0, 1};
    }
    std::vector<Particle> initial(capacity);
    uint32_t random = 1917, id = 0;
    auto rng = [&] {
        random = random * 1664525u + 1013904223u;
        return float(random >> 8) / 16777216.f;
    };
    for (uint32_t z = 2; z < nz - 2; ++z)
        for (uint32_t y = 1; y <= ny / 2; ++y)
            for (uint32_t x = 2; x < nx - 2; ++x)
                for (uint32_t k = 0; k < ppc; ++k) {
                    float a = f.minimumCell.x + (float(x) + float(k & 1) * .5f + .12f + rng() * .26f) * .08f;
                    float b =
                        f.minimumCell.y + (float(y) + float((k >> 1) & 1) * .5f + .12f + rng() * .26f) * .08f;
                    float c = f.minimumCell.z + (float(z) + float(k >> 2) * .5f + .12f + rng() * .26f) * .08f;
                    auto &p = initial[id++];
                    p.positionRadius = {a, b, c, .02f};
                    p.velocityFlags = {.15f + .09f * b, -.2f + .06f * a, -.08f + .12f * c,
                                       test.empty ? 0.f : 1.f};
                    p.apic0 = {0, .09f, 0, 1};
                    p.apic1 = {.06f, 0, 0, 17};
                    p.apic2 = {0, 0, .12f, 29};
                }
    const std::array<uint64_t, 19> sizes{capacity * 80ull,
                                         cells * 4ull,
                                         (cells + 1) * 4ull,
                                         cells * 4ull,
                                         capacity * 4ull,
                                         ((cells + 255) / 256) * 4ull,
                                         faces * 16ull,
                                         cells * 4ull,
                                         cells * 4ull,
                                         cells * 16ull,
                                         faces * 16ull,
                                         cells * 16ull,
                                         capacity * 16ull,
                                         cells * 16ull,
                                         cells * 16ull,
                                         cells * 4ull,
                                         256,
                                         256,
                                         256};
    std::array<gpu::Buffer, 19> ref, cuda;
    for (uint32_t i = 0; i < 19; ++i) {
        ref[i] = d.make(sizes[i]);
        cuda[i] = d.make(sizes[i], true);
    }
    auto frame = gpu::buffer(d.device.Get(), 512, D3D12_HEAP_TYPE_UPLOAD);
    std::memcpy(frame.mapped, &f, sizeof(f));
    auto zero =
        gpu::buffer(d.device.Get(), *std::max_element(sizes.begin(), sizes.end()), D3D12_HEAP_TYPE_UPLOAD);
    std::memset(zero.mapped, 0, size_t(zero.resource->GetDesc().Width));
    auto particleUpload = gpu::buffer(d.device.Get(), sizes[0], D3D12_HEAP_TYPE_UPLOAD);
    std::memcpy(particleUpload.mapped, initial.data(), size_t(sizes[0]));
    std::array<FluidCollider, 16 * 17> timeline{};
    std::copy(colliders.begin(), colliders.end(), timeline.begin());
    auto colliderUpload = gpu::buffer(d.device.Get(), sizeof(timeline), D3D12_HEAP_TYPE_UPLOAD);
    std::memcpy(colliderUpload.mapped, timeline.data(), sizeof(timeline));
    auto meshUpload = gpu::buffer(d.device.Get(), mesh.size() * 4, D3D12_HEAP_TYPE_UPLOAD);
    std::memcpy(meshUpload.mapped, mesh.data(), mesh.size() * 4);
    auto colliderGpu =
        gpu::buffer(d.device.Get(), sizeof(timeline), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"CUDA collider fixture", D3D12_HEAP_FLAG_SHARED);
    auto meshGpu =
        gpu::buffer(d.device.Get(), mesh.size() * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST, L"CUDA mesh SDF fixture", D3D12_HEAP_FLAG_SHARED);
    constexpr uint32_t regs[]{0, 1, 2, 3, 4, 6, 7, 8, 9, 10, 11, 13, 14, 18};
    std::vector<gpu::CudaInterop::Binding> bindings;
    for (auto r : regs)
        bindings.push_back({cuda[r].resource.Get(), sizes[r]});
    bindings.push_back(
        {colliderGpu.resource.Get(), sizeof(timeline), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    bindings.push_back(
        {meshGpu.resource.Get(), mesh.size() * 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE});
    gpu::CudaInterop interop(d.device.Get(), bindings);
    void *pointers[cuda_fluid::BufferCount]{};
    for (uint32_t i = 0; i < cuda_fluid::BufferCount; ++i)
        pointers[i] = interop.pointers()[i];
    cuda_fluid::Config config{};
    config.nx = nx;
    config.ny = ny;
    config.nz = nz;
    config.capacity = capacity;
    config.pressureIterations = test.pressure;
    config.densityIterations = test.density;
    config.viscositySubsteps = test.viscosity;
    config.deterministic = true;
    config.graphs = test.graphs;
    config.mixedPressure = config.forcedFinePressure = test.mixedPressure;
    Owner owner{interop, cuda_fluid::createTransfer(config, pointers)};
    const auto *cudaMesh = static_cast<const float *>(interop.pointers()[15]);
    const auto *cudaColliders = interop.pointers()[14];
    uint32_t colliderSlice = 0;
    auto bind = [&] {
        d.bind(frame.resource.Get(), ref, colliderGpu.resource.Get(), meshGpu.resource.Get());
        d.cmd->SetComputeRootShaderResourceView(20, colliderGpu.resource->GetGPUVirtualAddress() +
                                                        uint64_t(colliderSlice) * sizeof(colliders));
    };
    auto refGrid = [&](GridStage stage, uint32_t pi = 0, bool conditional = false) {
        bind();
        d.cmd->SetComputeRootUnorderedAccessView(8, ref[7 + pi].resource->GetGPUVirtualAddress());
        d.cmd->SetComputeRootUnorderedAccessView(9, ref[8 - pi].resource->GetGPUVirtualAddress());
        uint32_t pipeline = 10 + uint32_t(stage), count = cells, threads = 256;
        if (stage == GridStage::Forces || stage == GridStage::Viscosity ||
            stage == GridStage::MaterialFixture || stage == GridStage::Project ||
            stage == GridStage::Extrapolate) {
            count = faces;
            threads = 128;
        }
        if (stage == GridStage::DensityGather || stage == GridStage::DensityGatherAdaptive ||
            stage == GridStage::DensityMeasure)
            threads = 128;
        if (stage == GridStage::DensityDisplace) {
            count = capacity;
            threads = 128;
        }
        if (stage == GridStage::DensityClearArguments || stage == GridStage::DensityContinueArguments ||
            stage == GridStage::DensityPrepareArguments) {
            count = threads = 1;
        }
        if (conditional) {
            if (stage == GridStage::Jacobi)
                pipeline = 28;
            gpu::transition(d.cmd.Get(), ref[18].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            d.indirect(pipeline, ref[18].resource.Get(), stage == GridStage::Jacobi ? 6 : 7);
            gpu::transition(d.cmd.Get(), ref[18].resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        } else
            d.pass(pipeline, (count + threads - 1) / threads);
    };
    auto cudaGrid = [&](void *stream, GridStage stage, uint32_t pi = 0, bool conditional = false) {
        cuda_fluid::enqueueGrid(stream, pointers, &f, stage, pi, conditional);
    };
    auto refCollision = [&](bool bake) {
        bind();
        d.pass(bake ? 29 : 30, bake ? (cells + 255) / 256 : (capacity + 127) / 128);
    };
    auto cudaCollision = [&](void *stream, bool bake, bool conditional = false) {
        cuda_fluid::enqueueCollision(stream, pointers, &f,
                                     bake ? cuda_fluid::CollisionStage::BakeSolids
                                          : cuda_fluid::CollisionStage::Collide,
                                     cudaColliders, cudaMesh, conditional);
    };
    auto refBin = [&] {
        bind();
        uint32_t groups[]{(cells + 255) / 256, (capacity + 255) / 256, (cells + 255) / 256, 1,
                          (cells + 255) / 256, (capacity + 255) / 256, (cells + 255) / 256};
        for (uint32_t i = 0; i < 7; ++i)
            d.pass(i, groups[i]);
    };
    auto cudaBin = [&](void *stream) {
        cuda_fluid::enqueueTransfer(owner.transfer, stream, &f, cuda_fluid::TransferStage::Bin);
    };
    const uint64_t region = std::accumulate(sizes.begin(), sizes.end(), uint64_t(0));
    auto readback = gpu::buffer(d.device.Get(), region * 2, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    double maximumParticle = 0, maximumFaces = 0, maximumPressure = 0;
    uint32_t repairs = 0;
    std::vector<std::byte> contactSnapshot;
    auto audit = [&](const char *stage, std::initializer_list<uint32_t> fields) {
        uint64_t size = 0;
        for (auto r : fields) {
            d.copyOut(ref[r].resource.Get(), readback.resource.Get(), sizes[r], size);
            d.copyOut(cuda[r].resource.Get(), readback.resource.Get(), sizes[r], region + size);
            size += sizes[r];
        }
        d.submit();
        interop.collect();
        if (owner.solver)
            cuda_fluid::collect(owner.solver);
        void *mapped = nullptr;
        D3D12_RANGE range{0, size_t(region + size)};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Core comparison readback");
        std::vector<std::byte> copy(size_t(region + size));
        std::memcpy(copy.data(), mapped, copy.size());
        D3D12_RANGE written{0, 0};
        readback.resource->Unmap(0, &written);
        uint64_t offset = 0;
        for (auto r : fields) {
            const auto *a = reinterpret_cast<const float *>(copy.data() + offset),
                       *b = reinterpret_cast<const float *>(copy.data() + region + offset);
            if (r == 1 || r == 2 || r == 18) {
                if (std::memcmp(a, b, size_t(sizes[r])))
                    throw std::runtime_error(std::string(test.name) + " / " + stage +
                                             " integer field mismatch " + std::to_string(r));
                if (r == 18)
                    repairs = reinterpret_cast<const uint32_t *>(a)[25];
            } else
                for (uint32_t i = 0; i < sizes[r] / 4; ++i) {
                    double error = std::abs(double(a[i]) - b[i]), absolute = .00005, relative = .00003;
                    if (r == 7 || r == 8 || r == 10 || (r == 9 && i % 4 == 3)) {
                        absolute = .02;
                        relative = .00003;
                        maximumPressure = std::max(maximumPressure, error);
                    } else if (r == 9)
                        absolute = .001;
                    else if (r == 14) {
                        absolute = i % 4 == 1 ? .003 : .00003;
                        relative = .0001;
                    }
                    if (r == 0) {
                        maximumParticle = std::max(maximumParticle, error);
                        absolute = i % 20 < 4 ? .00001 : .00015;
                    }
                    if (r == 6)
                        maximumFaces = std::max(maximumFaces, error);
                    if (!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
                        error > absolute + relative * std::abs(double(a[i]))) {
                        if (r == 0) {
                            std::cerr << "Particle " << i / 20 << " HLSL/CUDA fields:\n";
                            for (uint32_t j = i / 20 * 20; j < (i / 20 + 1) * 20; ++j)
                                std::cerr << j % 20 << ": " << a[j] << " / " << b[j] << '\n';
                        }
                        throw std::runtime_error(std::string(test.name) + " / " + stage + " field " +
                                                 std::to_string(r) + " element " + std::to_string(i) +
                                                 " HLSL=" + std::to_string(a[i]) +
                                                 " CUDA=" + std::to_string(b[i]));
                    }
                }
            if (r == 0) {
                if (test.collider == 1) {
                    const size_t bytes = size_t(sizes[0]);
                    if (std::string(stage) == "initial-contact/P2G") {
                        contactSnapshot.resize(bytes * 2);
                        std::memcpy(contactSnapshot.data(), a, bytes);
                        std::memcpy(contactSnapshot.data() + bytes, b, bytes);
                    } else if (std::string(stage) == "stable-contact") {
                        require(!std::memcmp(contactSnapshot.data(), a, bytes) &&
                                    !std::memcmp(contactSnapshot.data() + bytes, b, bytes),
                                "Repeated sphere contact changed particle state without motion");
                    }
                }
                const auto *p = reinterpret_cast<const Particle *>(b);
                double mass = 0;
                uint32_t alive = 0;
                for (uint32_t i = 0; i < capacity; ++i) {
                    require(p[i].apic0.w == initial[i].apic0.w && p[i].apic1.w == initial[i].apic1.w &&
                                p[i].apic2.w == initial[i].apic2.w,
                            "Core APIC metadata/rest mass changed");
                    if (p[i].velocityFlags.w) {
                        mass += p[i].apic0.w;
                        alive++;
                    }
                }
                require(alive == (test.empty ? 0 : active) && mass == (test.empty ? 0 : active),
                        "Core particle mass/count changed");
            }
            if (r == 9 && std::string(stage) == "pressure") {
                double before = 0, after = 0;
                for (uint32_t i = 0; i < cells; ++i) {
                    before += double(b[i * 4]) * b[i * 4];
                    after += double(b[i * 4 + 1]) * b[i * 4 + 1];
                }
                require(after <= before + 1e-5, "Pressure increased global squared divergence");
            }
            offset += sizes[r];
        }
    };
    d.begin();
    for (auto *buffers : {&ref, &cuda})
        for (uint32_t i = 0; i < 19; ++i) {
            gpu::transition(d.cmd.Get(), (*buffers)[i].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
            d.cmd->CopyBufferRegion((*buffers)[i].resource.Get(), 0,
                                    i ? zero.resource.Get() : particleUpload.resource.Get(), 0, sizes[i]);
            gpu::transition(d.cmd.Get(), (*buffers)[i].resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    d.cmd->CopyBufferRegion(colliderGpu.resource.Get(), 0, colliderUpload.resource.Get(), 0,
                            sizeof(timeline));
    d.cmd->CopyBufferRegion(meshGpu.resource.Get(), 0, meshUpload.resource.Get(), 0, mesh.size() * 4);
    gpu::transition(d.cmd.Get(), colliderGpu.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    gpu::transition(d.cmd.Get(), meshGpu.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (test.frames) {
        owner.solver = cuda_fluid::create(config, pointers, mesh.data(), mesh.size());
        bool swapped = false;
        auto refSolve = [&](uint32_t iterations, bool conditional = false) {
            uint32_t pi = 0;
            for (uint32_t i = 0; i < iterations; ++i) {
                refGrid(GridStage::Jacobi, pi, conditional);
                pi = 1 - pi;
            }
            return pi;
        };
        auto refContactConditional = [&] {
            if (!f.collision.x)
                return;
            bind();
            gpu::transition(d.cmd.Get(), ref[18].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            d.indirect(30, ref[18].resource.Get(), 7);
            gpu::transition(d.cmd.Get(), ref[18].resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        for (uint32_t frameIndex = 0; frameIndex < test.frames; ++frameIndex) {
            if (frameIndex)
                d.begin();
            // Render-only constants must never invalidate graph executables.
            f.cameraPosition.x = float(frameIndex);
            f.emission.x = frameIndex;
            if (test.changeParameters && frameIndex == 1) {
                f.gravityDt.y = -4.5f;
                f.material.y *= .5f;
            }
            std::memcpy(frame.mapped, &f, sizeof(f));
            if (test.graphs && !frameIndex) {
                auto invalid = f;
                invalid.grid.x++;
                bool rejected = false;
                try {
                    cuda_fluid::prepare(owner.solver, &invalid);
                } catch (const std::exception &) {
                    rejected = true;
                }
                require(rejected && !cuda_fluid::statistics(owner.solver).graphBuilds,
                        "Invalid graph frame changed published state");
            }
            cuda_fluid::prepare(owner.solver, &f);
            for (uint32_t s = 0; s <= test.substeps; ++s)
                for (uint32_t i = 0; i < 16; ++i) {
                    auto value = colliders[i];
                    if (test.moving) {
                        float dx = .003f * (frameIndex * test.substeps + s);
                        value.centerRestitution.x += dx;
                        value.worldToLocal._41 -= dx;
                        value.velocityFriction.x = .003f / f.gravityDt.w;
                    }
                    timeline[s * 16 + i] = value;
                }
            std::memcpy(colliderUpload.mapped, timeline.data(), sizeof(timeline));
            gpu::transition(d.cmd.Get(), colliderGpu.resource.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            d.cmd->CopyBufferRegion(colliderGpu.resource.Get(), 0, colliderUpload.resource.Get(), 0,
                                    sizeof(timeline));
            gpu::transition(d.cmd.Get(), colliderGpu.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            colliderSlice = 0;
            refGrid(GridStage::DensityClearArguments);
            refCollision(true);
            for (uint32_t step = 0; step < test.substeps; ++step) {
                if (test.moving) {
                    colliderSlice = step + 1;
                    refCollision(true);
                }
                if (!step)
                    refBin();
                bind();
                d.pass(7, (faces + 127) / 128);
                refGrid(GridStage::Classify);
                refGrid(GridStage::Forces);
                for (uint32_t i = 0; i < test.viscosity; ++i) {
                    refGrid(GridStage::Viscosity);
                    std::swap(ref[6], ref[10]);
                }
                refGrid(GridStage::SurfaceColor);
                refGrid(GridStage::SurfaceCurvature);
                refGrid(GridStage::Divergence);
                auto pi = refSolve(test.pressure);
                refGrid(GridStage::Project, pi);
                refGrid(GridStage::Measure, pi);
                for (uint32_t i = 0; i < 4; ++i) {
                    refGrid(GridStage::Extrapolate);
                    std::swap(ref[6], ref[10]);
                }
                bind();
                d.pass(8, (capacity + 127) / 128);
                if (f.collision.x)
                    refCollision(false);
                if (test.density) {
                    refBin();
                    refGrid(GridStage::DensityGather);
                    pi = refSolve(test.density);
                    refGrid(GridStage::DensityDisplace, pi);
                }
                if (f.collision.x)
                    refCollision(false);
                refBin();
                if (test.density) {
                    refGrid(GridStage::DensityClearArguments);
                    refGrid(GridStage::DensityGatherAdaptive);
                    refGrid(GridStage::DensityPrepareArguments);
                    pi = refSolve(test.density, true);
                    refGrid(GridStage::DensityDisplace, pi, true);
                    refContactConditional();
                    refBin();
                }
            }
            interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
                cuda_fluid::enqueue(owner.solver, stream, &f, timeline.data(), test.substeps, test.moving,
                                    false);
                auto status = cuda_fluid::state(owner.solver);
                if (status.facesSwapped != swapped) {
                    std::swap(cuda[6], cuda[10]);
                    swapped = status.facesSwapped;
                }
            });
            audit("composed-frame", {0, 1, 2, 6, 7, 8, 9, 10, 11, 13, 14, 18});
        }
        const auto stats = cuda_fluid::statistics(owner.solver);
        const uint64_t steps = uint64_t(test.frames) * test.substeps;
        require(test.graphs ? (stats.graphReplays == steps && !stats.directSteps &&
                               stats.graphBuilds == (test.changeParameters ? 8u : 4u) && stats.graphNodes > 0)
                            : (stats.directSteps == steps && !stats.graphReplays && !stats.graphBuilds),
                "Incorrect graph variants, invalidation or replay count");
        std::cout << "{\"case\":\"" << test.name << "\",\"frames\":" << test.frames
                  << ",\"substeps\":" << test.substeps << ",\"moving\":" << (test.moving ? "true" : "false")
                  << ",\"maxParticleError\":" << maximumParticle << ",\"maxFaceError\":" << maximumFaces
                  << ",\"maxPressureError\":" << maximumPressure << ",\"graphBuilds\":" << stats.graphBuilds
                  << ",\"graphReplays\":" << stats.graphReplays << ",\"graphNodes\":" << stats.graphNodes
                  << ",\"graphPreparationMs\":" << stats.preparationMs
                  << ",\"mixedPressure\":" << (test.mixedPressure ? "true" : "false")
                  << ",\"stagingBytes\":" << stats.stagingBytes << ",\"pass\":true}\n";
        return;
    }
    refCollision(true);
    refCollision(false);
    refBin();
    bind();
    d.pass(7, (faces + 127) / 128);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cudaCollision(stream, true);
        cudaCollision(stream, false);
        cudaBin(stream);
        cuda_fluid::enqueueTransfer(owner.transfer, stream, &f, cuda_fluid::TransferStage::ToGrid,
                                    pointers[cuda_fluid::Faces]);
    });
    audit("initial-contact/P2G", {0, 1, 2, 6, 13});
    if (test.collider == 1) {
        d.begin();
        refCollision(false);
        interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(),
                        [&](void *stream) { cudaCollision(stream, false); });
        audit("stable-contact", {0});
    }
    d.begin();
    refGrid(GridStage::Classify);
    refGrid(GridStage::Forces);
    for (uint32_t i = 0; i < test.viscosity; ++i) {
        refGrid(GridStage::Viscosity);
        std::swap(ref[6], ref[10]);
    }
    refGrid(GridStage::SurfaceColor);
    refGrid(GridStage::SurfaceCurvature);
    refGrid(GridStage::Divergence);
    uint32_t pi = 0;
    for (uint32_t i = 0; i < test.pressure; ++i) {
        refGrid(GridStage::Jacobi, pi);
        pi = 1 - pi;
    }
    refGrid(GridStage::Project, pi);
    refGrid(GridStage::Measure, pi);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cudaGrid(stream, GridStage::Classify);
        cudaGrid(stream, GridStage::Forces);
        for (uint32_t i = 0; i < test.viscosity; ++i) {
            cudaGrid(stream, GridStage::Viscosity);
            std::swap(pointers[cuda_fluid::Faces], pointers[cuda_fluid::Scratch]);
            std::swap(cuda[6], cuda[10]);
        }
        cudaGrid(stream, GridStage::SurfaceColor);
        cudaGrid(stream, GridStage::SurfaceCurvature);
        cudaGrid(stream, GridStage::Divergence);
        uint32_t index = 0;
        for (uint32_t i = 0; i < test.pressure; ++i) {
            cudaGrid(stream, GridStage::Jacobi, index);
            index = 1 - index;
        }
        cudaGrid(stream, GridStage::Project, index);
        cudaGrid(stream, GridStage::Measure, index);
    });
    audit("pressure", {6, 7, 8, 9, 10, 14});
    d.begin();
    for (uint32_t i = 0; i < 4; ++i) {
        refGrid(GridStage::Extrapolate);
        std::swap(ref[6], ref[10]);
    }
    bind();
    d.pass(8, (capacity + 127) / 128);
    refCollision(false);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        for (uint32_t i = 0; i < 4; ++i) {
            cudaGrid(stream, GridStage::Extrapolate);
            std::swap(pointers[cuda_fluid::Faces], pointers[cuda_fluid::Scratch]);
            std::swap(cuda[6], cuda[10]);
        }
        cuda_fluid::enqueueTransfer(owner.transfer, stream, &f, cuda_fluid::TransferStage::ToParticles,
                                    pointers[cuda_fluid::Faces]);
        cudaCollision(stream, false);
    });
    audit("extrapolate/G2P/contact", {0, 6, 13});
    d.begin();
    refBin();
    refGrid(GridStage::DensityGather);
    pi = 0;
    for (uint32_t i = 0; i < test.density; ++i) {
        refGrid(GridStage::Jacobi, pi);
        pi = 1 - pi;
    }
    refGrid(GridStage::DensityDisplace, pi);
    refCollision(false);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cudaBin(stream);
        cudaGrid(stream, GridStage::DensityGather);
        uint32_t index = 0;
        for (uint32_t i = 0; i < test.density; ++i) {
            cudaGrid(stream, GridStage::Jacobi, index);
            index = 1 - index;
        }
        cudaGrid(stream, GridStage::DensityDisplace, index);
        cudaCollision(stream, false);
    });
    audit("density/contact", {0, 7, 8, 10, 11});
    d.begin();
    refBin();
    refGrid(GridStage::DensityClearArguments);
    refGrid(GridStage::DensityGatherAdaptive);
    refGrid(GridStage::DensityPrepareArguments);
    pi = 0;
    for (uint32_t i = 0; i < test.density; ++i) {
        refGrid(GridStage::Jacobi, pi, true);
        pi = 1 - pi;
    }
    refGrid(GridStage::DensityDisplace, pi, true);
    bind();
    gpu::transition(d.cmd.Get(), ref[18].resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    d.indirect(30, ref[18].resource.Get(), 7);
    gpu::transition(d.cmd.Get(), ref[18].resource.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Unconditional rebin is a validation observation after either branch, not
    // a replacement for the production solver's conditional scheduling.
    refBin();
    refGrid(GridStage::DensityMeasure);
    interop.execute(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), [&](void *stream) {
        cudaBin(stream);
        cudaGrid(stream, GridStage::DensityClearArguments);
        cudaGrid(stream, GridStage::DensityGatherAdaptive);
        cudaGrid(stream, GridStage::DensityPrepareArguments);
        uint32_t index = 0;
        for (uint32_t i = 0; i < test.density; ++i) {
            cudaGrid(stream, GridStage::Jacobi, index, true);
            index = 1 - index;
        }
        cudaGrid(stream, GridStage::DensityDisplace, index, true);
        cudaCollision(stream, false, true);
        cudaBin(stream);
        cudaGrid(stream, GridStage::DensityMeasure);
    });
    audit("conditional-repair", {0, 1, 2, 7, 8, 10, 11, 18});
    require(test.empty ? repairs == 0 : repairs == 1,
            "Density repair did not exercise the expected GPU flag branch");
    std::cout << "{\"case\":\"" << test.name << "\",\"pressureIterations\":" << test.pressure
              << ",\"densityIterations\":" << test.density << ",\"colliders\":" << f.collision.x
              << ",\"repairs\":" << repairs << ",\"maxParticleError\":" << maximumParticle
              << ",\"maxFaceError\":" << maximumFaces << ",\"maxPressureError\":" << maximumPressure
              << ",\"pass\":true}\n";
}
} // namespace
int main(int argc, char **argv) try {
    require(argc == 2, "Pass the CUDA test runtime directory");
    Fixture d(argv[1], true);
    const Case cases[]{
        {"empty-core", 0, 120, 60, 1, true},
        {"free-surface", 0, 120, 60, 1},
        {"odd-iterations/flip", 0, 61, 31, 3, false, true},
        {"moving-sphere", 1, 120, 60, 1},
        {"mesh-SDF", 2, 120, 60, 2},
        {"primitive-contacts", 3, 120, 60, 3},
        {"composed-free-surface", 0, 120, 60, 1, false, false, 3, 2},
        {"composed-moving-sphere", 1, 120, 60, 3, false, false, 3, 2, true},
        {"composed-FLIP-no-density", 0, 61, 0, 2, false, true, 3, 2},
        {"graph-free-surface", 0, 120, 60, 1, false, false, 3, 2, false, true},
        {"graph-moving-sphere", 1, 120, 60, 3, false, false, 3, 2, true, true},
        {"graph-odd-parity", 2, 61, 31, 1, false, false, 3, 1, false, true},
        {"graph-FLIP-no-density", 0, 61, 0, 2, false, true, 3, 2, false, true},
        {"graph-four-substeps", 1, 120, 60, 1, false, false, 3, 4, true, true},
        {"graph-paused", 1, 120, 60, 1, false, false, 3, 0, false, true},
        {"graph-parameter-change", 1, 120, 60, 1, false, false, 3, 2, true, true, true},
        {"mgpcg-HLSL-free-surface", 0, 1000, 60, 1, false, false, 3, 2, false, false, false, true},
        {"mgpcg-HLSL-moving-graph", 1, 1000, 60, 3, false, false, 3, 2, true, true, false, true},
        {"mgpcg-HLSL-FLIP-graph", 0, 1000, 0, 2, false, true, 3, 2, false, true, false, true}};
    for (const auto &test : cases)
        run(d, test);
    std::cout
        << "PASS CUDA core kernels: HLSL pressure/material/collision/density and composed substep parity; "
           "renderer tested separately\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA core kernels: " << e.what() << '\n';
    return 1;
}
