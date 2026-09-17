#include "../src/fluid/cuda/fluid_cuda_kernels.h"
#include "../src/fluid/cuda/fluid_cuda_mac.h"
#include "../src/fluid/cuda/fluid_cuda_device.cuh"
#include "../src/fluid/cuda/fluid_cuda_transfer.h"
#include "../src/fluid/cuda/fluid_cuda_grid.h"
#include "../src/fluid/cuda/fluid_cuda_collision.h"
#include "../src/fluid/cuda/fluid_cuda_ownership.h"
#include "../src/fluid/cuda/fluid_cuda_geometric_transport.h"
#include "../src/fluid/cuda/fluid_cuda_geometry.cuh"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
using namespace lab::cuda_fluid;
using namespace lab::cuda_fluid::detail;
namespace {
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
template <class T> std::vector<T> read(const void *p, size_t count) {
    std::vector<T> out(count);
    check(cudaMemcpy(out.data(), p, count * sizeof(T), cudaMemcpyDeviceToHost));
    return out;
}
__global__ void seed(Frame f, Particle *particles, bool obstacle, bool empty) {
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    Particle p{};
    const uint32_t cell = id / 8, k = id % 8;
    if (cell < f.grid.w) {
        int3 c = cellFromIndex(cell, f);
        float3 q = (asFloat(c) +
                    make_float3(.25f + .5f * (k & 1), .25f + .5f * ((k >> 1) & 1), .25f + .5f * (k >> 2))) *
                   f.minimumCell.w;
        p.positionRadius = make_float4(q.x, q.y, q.z, f.maximumRadius.w);
        bool active = !empty && c.y < int(f.grid.y) - 2;
        if (obstacle && length(q - make_float3(.65f, .55f, .65f)) < .22f + f.maximumRadius.w)
            active = false;
        p.velocityFlags =
            make_float4(.003f * sinf(q.z), -.003f * cosf(q.x), .002f * sinf(q.y), active ? 1.f : 0.f);
        p.apic0 = make_float4(0, 0, .003f * cosf(q.z), 1);
        p.apic1 = make_float4(.003f * sinf(q.x), 0, 0, 17);
        p.apic2 = make_float4(0, .002f * cosf(q.y), 0, 29);
    }
    particles[id] = p;
}
__global__ void seedAuthority(Frame f, const Particle *particles, double4 *quantities, float4 *references) {
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= f.counts.x)
        return;
    const auto p = particles[id];
    const double volume = p.velocityFlags.w ? double(f.initialMinimum.w) * double(p.apic0.w) : 0;
    quantities[id] = make_double4(volume * p.velocityFlags.x, volume * p.velocityFlags.y,
                                  volume * p.velocityFlags.z, volume);
    references[id] = p.velocityFlags.w
                         ? make_float4(p.velocityFlags.x, p.velocityFlags.y, p.velocityFlags.z, 0)
                         : make_float4(0, 0, 0, 0);
}
Config config(bool graphs = false) {
    Config c{};
    c.nx = c.ny = c.nz = 12;
    c.capacity = 12 * 12 * 12 * 8 + 7;
    c.pressureIterations = 1000;
    c.densityIterations = 9;
    c.viscositySubsteps = 3;
    c.deterministic = true;
    c.graphs = graphs;
    c.mixedPressure = true;
    return c;
}
struct Fixture {
    Config cfg;
    Frame frame{};
    void *buffers[BufferCount]{};
    Ownership owned{};
    GridInventory inventory{};
    SurfaceGeometry surface{};
    Solver *solver = nullptr;
    Transfer *referenceTransfer = nullptr;
    Collider *referenceColliders = nullptr;
    cudaStream_t stream = nullptr;
    Collider colliders[17 * 16]{};
    std::vector<Particle> initial;
    bool obstacle = false;
    Fixture(Config c, bool moving = false, bool empty = false, bool joint = false, bool geometry = false)
        : cfg(c), obstacle(moving) {
        frame.grid = make_uint4(c.nx, c.ny, c.nz, c.nx * c.ny * c.nz);
        frame.counts.x = c.capacity;
        frame.minimumCell = make_float4(0, 0, 0, .13f);
        frame.maximumRadius = make_float4(c.nx * .13f, c.ny * .13f, c.nz * .13f, .0325f);
        frame.gravityDt = make_float4(0, -9.81f, 0, 1.f / 120);
        frame.solver = make_float4(1000, .95f, 0, .125f);
        frame.material = make_float4(.002f, .072f, float(c.viscositySubsteps), 0);
        frame.collision.x = moving ? 1 : 0;
        if (joint)
            frame.material.y = 0;
        if (c.ownedParticles) {
            frame.display.w = 2;
            frame.initialMinimum.w = float(std::pow(double(frame.minimumCell.w), 3) * frame.solver.w);
        }
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        try {
            for (uint32_t i = 0; i < BufferCount; ++i) {
                const auto bytes = bufferBytes(c, Buffer(i));
                check(cudaMalloc(buffers + i, bytes + 64));
                check(cudaMemsetAsync(buffers[i], 0, bytes, stream));
                check(cudaMemsetAsync(static_cast<char *>(buffers[i]) + bytes, 0xcd, 64, stream));
            }
            if (c.ownedParticles)
                for (uint32_t i = 0; i < owned.size(); ++i) {
                    const auto bytes = ownershipBytes(c, OwnershipBuffer(i));
                    check(cudaMalloc(&owned[i], bytes + 64));
                    check(cudaMemsetAsync(owned[i], 0, bytes, stream));
                    check(cudaMemsetAsync(static_cast<char *>(owned[i]) + bytes, 0xcd, 64, stream));
                }
            seed<<<(c.capacity + 127) / 128, 128, 0, stream>>>(
                frame, static_cast<Particle *>(buffers[Particles]), moving, empty);
            if (c.ownedParticles)
                seedAuthority<<<(c.capacity + 127) / 128, 128, 0, stream>>>(
                    frame, static_cast<Particle *>(buffers[Particles]),
                    static_cast<double4 *>(owned[OwnedQuantity]),
                    static_cast<float4 *>(owned[OwnedReference]));
            check(cudaStreamSynchronize(stream));
            if (joint) {
                for (uint32_t i = 0; i < inventory.size(); ++i) {
                    const auto bytes = gridInventoryBytes(c, GridInventoryBuffer(i));
                    check(cudaMalloc(&inventory[i], bytes + 64));
                    check(cudaMemset(static_cast<char *>(inventory[i]) + bytes, 0xcd, 64));
                }
                const double h = frame.minimumCell.w;
                std::vector<double> open(frame.grid.w, h * h * h);
                std::vector<std::array<double, 4>> grid(gridInventoryBytes(c, GridOwnedQuantity) / 32);
                const uint32_t cx = (c.nx + 1) / 2, cy = (c.ny + 1) / 2;
                for (uint32_t i = 0; i < open.size(); ++i) {
                    const uint32_t x = i % c.nx, y = (i / c.nx) % c.ny, z = i / (c.nx * c.ny);
                    if (!empty && y < c.ny - 2)
                        grid[((z / 2) * cy + y / 2) * cx + x / 2][3] += open[i] * .2;
                }
                for (auto &q : grid) {
                    q[0] = q[3] * .002;
                    q[1] = q[3] * -.001;
                    q[2] = q[3] * .003;
                }
                check(cudaMemcpy(inventory[GridFineCapacity], open.data(), open.size() * 8,
                                 cudaMemcpyHostToDevice));
                check(cudaMemcpy(inventory[GridOwnedQuantity], grid.data(), grid.size() * 32,
                                 cudaMemcpyHostToDevice));
                auto pp = particles();
                auto qq = read<double4>(owned[OwnedQuantity], c.capacity);
                for (uint32_t i = 0; i < c.capacity; ++i) {
                    if (pp[i].velocityFlags.w)
                        pp[i].apic0.w = .25f;
                    qq[i].x *= .25;
                    qq[i].y *= .25;
                    qq[i].z *= .25;
                    qq[i].w *= .25;
                }
                check(cudaMemcpy(buffers[Particles], pp.data(), pp.size() * sizeof(Particle),
                                 cudaMemcpyHostToDevice));
                check(cudaMemcpy(owned[OwnedQuantity], qq.data(), qq.size() * sizeof(double4),
                                 cudaMemcpyHostToDevice));
            }
            initial = particles();
            if (geometry)
                for (uint32_t i = 0; i < surface.size(); ++i) {
                    const auto bytes = surfaceGeometryBytes(c, SurfaceGeometryBuffer(i));
                    check(cudaMalloc(&surface[i], bytes + 64));
                    // Deliberately invalid output contents must not be read as
                    // phase input or published on a failed first submission.
                    check(cudaMemset(surface[i], 0xcd, bytes + 64));
                }
            solver = create(c, buffers, nullptr, 0, owned, inventory, surface);
            if (!c.mixedPressure && !c.ownedParticles) {
                referenceTransfer = createTransfer(c, buffers);
                check(cudaMalloc(&referenceColliders, sizeof(colliders)));
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Fixture() {
        cleanup();
    }
    void cleanup() {
        if (stream)
            cudaStreamSynchronize(stream);
        destroy(solver);
        destroyTransfer(referenceTransfer);
        if (referenceColliders)
            cudaFree(referenceColliders);
        for (auto p : buffers)
            if (p)
                cudaFree(p);
        for (auto p : owned)
            if (p)
                cudaFree(p);
        for (auto p : inventory)
            if (p)
                cudaFree(p);
        for (auto p : surface)
            if (p)
                cudaFree(p);
        if (stream)
            cudaStreamDestroy(stream);
    }
    void endpoints(uint32_t frameId, uint32_t steps) {
        for (uint32_t s = 0; s <= steps; ++s) {
            auto &c = colliders[s * 16];
            const float x = .65f + .001f * (frameId * steps + s);
            c.worldToLocal[0] = make_float4(1, 0, 0, 0);
            c.worldToLocal[1] = make_float4(0, 1, 0, 0);
            c.worldToLocal[2] = make_float4(0, 0, 1, 0);
            c.worldToLocal[3] = make_float4(-x, -.55f, -.65f, 1);
            c.centerRestitution = make_float4(x, .55f, .65f, .1f);
            c.extentType = make_float4(.22f, 0, 0, 0);
            c.velocityFriction = make_float4(.001f / frame.gravityDt.w, 0, 0, .1f);
            c.angularSlip = make_float4(0, .1f, 0, 1);
        }
    }
    void advance(uint32_t frameId = 0, uint32_t steps = 2, bool rebuild = false, bool reset = false) {
        endpoints(frameId, steps);
        prepare(solver, &frame);
        enqueue(solver, stream, &frame, colliders, steps, obstacle, rebuild, reset);
        check(cudaStreamSynchronize(stream));
        try {
            collect(solver);
        } catch (...) {
            if (const auto *view = solverGeometricMetrics(solver)) {
                const auto m = read<GeometricTransportMetrics>(view, 1)[0];
                std::cerr << "Geometric diagnostic: stage=" << m.failureStage << " index=" << m.failureIndex
                          << " completed=" << m.completed << "/" << m.substeps << '\n';
            }
            throw;
        }
        audit();
        require(statistics(solver).completedFrames == uint64_t(frameId) + 1,
                "Completion diagnostics lost a frame");
    }
    // Independent reference schedule with an actually converged fine Jacobi
    // solve. A 1000-sweep cap leaves ~0.0018 m/s of error in this tall closed-side
    // pool; it is not a qualified oracle for the MGPCG divergence target.
    void advanceReference(uint32_t frameId, uint32_t steps = 2) {
        endpoints(frameId, steps);
        check(cudaMemcpyAsync(referenceColliders, colliders, sizeof(colliders), cudaMemcpyHostToDevice,
                              stream));
        uint32_t pi = 0;
        auto grid = [&](GridStage stage, bool conditional = false) {
            enqueueGrid(stream, buffers, &frame, stage, pi, conditional);
        };
        auto transfer = [&](TransferStage stage) {
            enqueueTransfer(referenceTransfer, stream, &frame, stage, buffers[Faces]);
        };
        auto solve = [&](uint32_t iterations, bool conditional = false) {
            pi = 0;
            for (uint32_t i = 0; i < iterations; ++i) {
                grid(GridStage::Jacobi, conditional);
                pi = 1 - pi;
            }
        };
        grid(GridStage::DensityClearArguments);
        for (uint32_t step = 0; step < steps; ++step) {
            auto endpoint = referenceColliders + (obstacle ? step + 1 : 0) * 16;
            auto contact = [&](bool conditional = false) {
                if (obstacle)
                    enqueueCollision(stream, buffers, &frame, CollisionStage::Collide, endpoint, nullptr,
                                     conditional);
            };
            enqueueCollision(stream, buffers, &frame, CollisionStage::BakeSolids, endpoint, nullptr);
            transfer(TransferStage::Bin);
            transfer(TransferStage::ToGrid);
            pi = 0;
            grid(GridStage::Classify);
            grid(GridStage::Forces);
            for (uint32_t i = 0; i < cfg.viscositySubsteps; ++i) {
                grid(GridStage::Viscosity);
                std::swap(buffers[Faces], buffers[Scratch]);
            }
            grid(GridStage::SurfaceColor);
            grid(GridStage::SurfaceCurvature);
            grid(GridStage::Divergence);
            solve(6000);
            grid(GridStage::Project);
            grid(GridStage::Measure);
            for (uint32_t i = 0; i < 4; ++i) {
                grid(GridStage::Extrapolate);
                std::swap(buffers[Faces], buffers[Scratch]);
            }
            transfer(TransferStage::ToParticles);
            contact();
            transfer(TransferStage::Bin);
            grid(GridStage::DensityGather);
            solve(cfg.densityIterations);
            grid(GridStage::DensityDisplace);
            contact();
            transfer(TransferStage::Bin);
            grid(GridStage::DensityClearArguments);
            grid(GridStage::DensityGatherAdaptive);
            grid(GridStage::DensityPrepareArguments);
            solve(cfg.densityIterations, true);
            grid(GridStage::DensityDisplace, true);
            contact(true);
            transfer(TransferStage::Bin);
        }
        check(cudaStreamSynchronize(stream));
        for (auto cell : read<float4>(buffers[Cells], frame.grid.w))
            require(cell.z != 1 || std::abs(cell.y) < .000101f,
                    "Fine reference did not meet the same divergence gate");
        audit();
    }
    std::vector<Particle> particles() {
        return read<Particle>(buffers[Particles], cfg.capacity);
    }
    std::vector<float4> faces() {
        const auto s = state(solver);
        return read<float4>(buffers[s.facesSwapped ? Scratch : Faces], bufferBytes(cfg, Faces) / 16);
    }
    void audit() {
        if (cfg.ownedParticles)
            for (uint32_t i = 0; i < owned.size(); ++i) {
                const auto tail = read<unsigned char>(
                    static_cast<char *>(owned[i]) + ownershipBytes(cfg, OwnershipBuffer(i)), 64);
                require(std::all_of(tail.begin(), tail.end(), [](auto v) { return v == 0xcd; }),
                        "Ownership tail overwritten");
            }
        for (uint32_t i = 0; i < BufferCount; ++i) {
            auto tail =
                read<unsigned char>(static_cast<char *>(buffers[i]) + bufferBytes(cfg, Buffer(i)), 64);
            require(std::all_of(tail.begin(), tail.end(), [](auto v) { return v == 0xcd; }),
                    "Shared buffer tail overwritten");
        }
        const auto current = particles();
        for (uint32_t i = 0; i < cfg.capacity; ++i) {
            auto a = current[i], b = initial[i];
            require(a.velocityFlags.w == b.velocityFlags.w && a.apic0.w == b.apic0.w &&
                        a.apic1.w == b.apic1.w && a.apic2.w == b.apic2.w,
                    "Particle mass, flags or APIC metadata changed");
            const auto *values = reinterpret_cast<const float *>(&a);
            for (uint32_t j = 0; j < 20; ++j)
                require(std::isfinite(values[j]), "Nonfinite composed particle");
            if (!b.velocityFlags.w)
                require(!std::memcmp(&a, &b, sizeof(a)), "Inactive particle was changed");
        }
        if (cfg.mixedPressure) {
            const auto m = solverMac(solver);
            const auto counts = read<uint32_t>(macPressureView(m).counts, PressureCounterCount);
            const auto pool = read<uint32_t>(macView(m).pool.control, BrickCounterCount);
            require(!pool[BrickInvalid] && !counts[PressureTotalCapped],
                    "Composed pressure rejected a substep");
            require(pool[BrickFrameChanges] <= cfg.pressureChangesPerFrame,
                    "Substeps multiplied topology budget");
            require(!state(solver).facesSwapped && !state(solver).pressureIndex,
                    "Transactional views lost canonical bindings");
            for (auto cell : read<float4>(buffers[Cells], frame.grid.w))
                require(cell.z != 1 || std::abs(cell.y) < .000101f,
                        "Composed pressure divergence exceeds target");
        }
    }
};
void fineReference(bool graph, bool obstacle) {
    auto c = config(graph);
    c.forcedFinePressure = true;
    Fixture actual(c, obstacle);
    c.mixedPressure = c.forcedFinePressure = false;
    c.graphs = false;
    Fixture expected(c, obstacle);
    double position = 0, velocity = 0, affine = 0, face = 0;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        actual.advance(frame);
        expected.advanceReference(frame);
        auto a = actual.particles(), b = expected.particles();
        for (uint32_t i = 0; i < c.capacity; ++i) {
            auto av = reinterpret_cast<const float *>(&a[i]), bv = reinterpret_cast<const float *>(&b[i]);
            for (uint32_t j = 0; j < 20; ++j) {
                const double error = std::abs(double(av[j]) - bv[j]);
                if (j < 3)
                    position = std::max(position, error);
                else if (j >= 4 && j < 7)
                    velocity = std::max(velocity, error);
                else if (j >= 8 && (j % 4) != 3)
                    affine = std::max(affine, error);
            }
        }
        auto af = actual.faces(), bf = expected.faces();
        for (uint32_t i = 0; i < af.size(); ++i)
            face = std::max(face, std::abs(double(af[i].x) - bf[i].x));
    }
    std::cout << "{\"case\":\"solver-fine-"
              << (obstacle ? "moving-wall"
                  : graph  ? "graph"
                           : "direct")
              << "\",\"positionError\":" << position << ",\"velocityError\":" << velocity
              << ",\"affineError\":" << affine << ",\"faceError\":" << face;
    require(position < 2e-5 && velocity < 3e-5 && affine < .001 && face < 3e-5,
            "Composed fine MGPCG differs from converged Jacobi reference");
    std::cout << ",\"pass\":true}\n";
}
void mixedReplay(bool fallback) {
    auto c = config();
    c.pressureChangesPerFrame = 2;
    if (fallback)
        c.pressureBrickCapacity = 1;
    Fixture direct(c);
    c.graphs = true;
    Fixture graph(c);
    for (uint32_t frame = 0; frame < 15; ++frame) {
        direct.advance(frame);
        graph.advance(frame);
        const auto a = direct.particles(), b = graph.particles();
        require(!std::memcmp(a.data(), b.data(), a.size() * sizeof(Particle)),
                "Mixed composed direct/graph particles differ");
        const auto af = direct.faces(), bf = graph.faces();
        require(!std::memcmp(af.data(), bf.data(), af.size() * sizeof(float4)),
                "Mixed composed direct/graph faces differ");
    }
    const auto m = solverMac(graph.solver);
    const auto counters = read<uint32_t>(macView(m).counters, MacCounterCount);
    const auto pool = read<uint32_t>(macView(m).pool.control, BrickCounterCount);
    require(counters[MacCoarsePeak] > 0 || fallback, "No complete coarse/fine composed solve exercised");
    require(counters[MacFallbacks] > 0 && (fallback ? !pool[BrickVersion] : pool[BrickVersion] > 0),
            "No budget/capacity transition exercised");
    std::cout << "{\"case\":\"solver-" << (fallback ? "capacity" : "mixed-budget")
              << "-graph\",\"coarsePeak\":" << counters[MacCoarsePeak]
              << ",\"fallbacks\":" << counters[MacFallbacks]
              << ",\"stagingBytes\":" << statistics(graph.solver).stagingBytes << ",\"pass\":true}\n";
}
void reject(bool graph, bool fallback) {
    auto c = config(graph);
    c.cgIterations = 1;
    if (fallback)
        c.pressureBrickCapacity = 1;
    Fixture f(c, true);
    std::vector<std::vector<unsigned char>> before;
    for (uint32_t i = 0; i < BufferCount; ++i)
        before.push_back(read<unsigned char>(f.buffers[i], bufferBytes(c, Buffer(i)) + 64));
    f.endpoints(0, 4);
    prepare(f.solver, &f.frame);
    enqueue(f.solver, f.stream, &f.frame, f.colliders, 4, true, false);
    check(cudaStreamSynchronize(f.stream));
    for (uint32_t i = 0; i < BufferCount; ++i)
        require(before[i] == read<unsigned char>(f.buffers[i], before[i].size()),
                "Failed frame partially published shared fields");
    const auto particles = read<Particle>(solverParticles(f.solver), c.capacity);
    require(!std::memcmp(particles.data(), f.initial.data(), particles.size() * sizeof(Particle)),
            "Failed pressure still advanced private particles through G2P/contact/density");
    const auto m = solverMac(f.solver);
    const auto counts = read<uint32_t>(macPressureView(m).counts, PressureCounterCount);
    require(counts[PressureCapped] && counts[PressureCalls] == 1 && counts[PressureTotalCapped] == 1,
            "Later queued steps erased first failure");
    bool rejected = false;
    try {
        collect(f.solver);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Completion fence failed to report rejected frame");
    require(statistics(f.solver).rejectedFrames == 1 && statistics(f.solver).pressureCaps == 1,
            "Failure diagnostics did not preserve the capped solve");
    rejected = false;
    try {
        prepare(f.solver, &f.frame);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Poisoned solver accepted another frame");
    std::cout << "{\"case\":\"solver-reject-"
              << (fallback ? "fallback"
                  : graph  ? "graph"
                           : "direct")
              << "\",\"pass\":true}\n";
}
void pausedEmpty() {
    auto c = config(true);
    Fixture f(c, false, true);
    f.advance(0, 0, true);
    f.advance(1, 0, false);
    f.advance(2, 4, false);
    const auto counts = read<uint32_t>(macPressureView(solverMac(f.solver)).counts, PressureCounterCount);
    require(counts[PressureConverged] && !counts[PressureTotalIterations],
            "Empty/paused solver did unnecessary CG updates");
    require(counts[PressureLoopSolves] == 4 && !counts[PressureLoopIterations],
            "Empty conditional graph executed a pressure iteration body");
    std::cout << "{\"case\":\"solver-empty-paused\",\"pass\":true}\n";
}
void loopSchedule() {
    auto c = config(true);
    Fixture loop(c, true, true);
    c.pressureConditionalGraphs = false;
    Fixture unrolled(c, true, true);
    for (uint32_t frame = 0; frame < 12; ++frame) {
        for (auto *f : {&loop, &unrolled}) {
            // Change GPU occupancy without rebuilding the graph, then change
            // physical constants to exercise replacement of its owned bodies.
            if (frame == 1 || frame == 5 || frame == 7) {
                seed<<<(c.capacity + 127) / 128, 128, 0, f->stream>>>(
                    f->frame, static_cast<Particle *>(f->buffers[Particles]), true, frame == 5);
                check(cudaStreamSynchronize(f->stream));
                f->initial = f->particles();
            }
            if (frame == 3 || frame == 8)
                f->frame.gravityDt.y = frame == 3 ? -4.f : -9.81f;
        }
        const auto previous = statistics(loop.solver);
        loop.advance(frame, 2, false, frame == 7);
        unrolled.advance(frame, 2, false, frame == 7);
        const auto a = loop.particles(), b = unrolled.particles();
        const auto af = loop.faces(), bf = unrolled.faces();
        require(!std::memcmp(a.data(), b.data(), a.size() * sizeof(Particle)) &&
                    !std::memcmp(af.data(), bf.data(), af.size() * sizeof(float4)),
                "Conditional/unrolled graph changed particle or face results");
        const auto s = statistics(loop.solver), r = statistics(unrolled.solver);
        require(s.pressureLoopSolves == s.pressureSolves &&
                    s.pressureLoopIterations == s.pressureIterations &&
                    s.pressureIterations == r.pressureIterations && !r.pressureLoopSolves &&
                    !r.pressureLoopIterations && !r.graphBodyNodes && s.graphBodyNodes &&
                    s.graphNodes > s.graphBodyNodes && s.graphNodes < r.graphNodes,
                "Conditional pressure did not execute/count its bounded body correctly");
        if (frame == 0 || frame == 5 || frame == 6)
            require(s.pressureLoopIterations == previous.pressureLoopIterations,
                    "Empty replay retained a prior nonzero loop condition");
    }
    const auto s = statistics(loop.solver);
    require(s.graphBuilds == 12 && statistics(unrolled.solver).graphBuilds == 12,
            "Pressure graph replacement/reseed count changed");
    std::cout << "{\"case\":\"solver-conditional-replay-rebuild\",\"loopSolves\":" << s.pressureLoopSolves
              << ",\"loopIterations\":" << s.pressureLoopIterations << ",\"graphNodes\":" << s.graphNodes
              << ",\"bodyNodes\":" << s.graphBodyNodes << ",\"pass\":true}\n";
}
void resetHistory() {
    Fixture f(config(true));
    for (uint32_t i = 0; i < 8; ++i)
        f.advance(i);
    const auto view = macView(solverMac(f.solver));
    const auto before = read<uint32_t>(view.history, view.coarseCount);
    require(std::any_of(before.begin(), before.end(), [](uint32_t x) { return x > 0; }),
            "Reset test did not age any refinement decisions");
    // A paused reset rebuilds P2G/pressure without particle advection, so its
    // classification may age once, but must not retain the old coarse decision.
    f.advance(8, 0, true, true);
    const auto after = read<uint32_t>(view.history, view.coarseCount);
    require(std::all_of(after.begin(), after.end(), [](uint32_t x) { return x == 0 || x == 256; }),
            "Paused engine reset kept stale refinement history");
    f.advance(9);
    const auto counters = read<uint32_t>(view.counters, MacCounterCount);
    require(!counters[MacCoarse], "Reset immediately reused previously aged coarse cells");
    require(statistics(f.solver).graphBuilds == 4, "Reset rebuilt static CUDA graphs");
    std::cout << "{\"case\":\"solver-reset-history\",\"pass\":true}\n";
}
void lateReject(bool graph, bool authority = false) {
    auto c = config(graph);
    c.ownedParticles = authority;
    Fixture frame(c), first(c);
    frame.frame.collision.x = first.frame.collision.x = 1;
    auto lid = [](float y, float speed) {
        Collider c{};
        c.worldToLocal[0] = make_float4(1, 0, 0, 0);
        c.worldToLocal[1] = make_float4(0, 1, 0, 0);
        c.worldToLocal[2] = make_float4(0, 0, 1, 0);
        c.worldToLocal[3] = make_float4(-.78f, -y, -.78f, 1);
        c.centerRestitution = make_float4(.78f, y, .78f, 0);
        c.extentType = make_float4(2, .04f, 2, 1);
        c.velocityFriction = make_float4(0, speed, 0, 0);
        c.angularSlip = make_float4(0, 0, 0, 1);
        return c;
    };
    // First substep is a valid free surface. The next seals all liquid below
    // an inward-moving lid: incompatible closed Neumann flux cannot converge.
    // This deliberate failure tests rollback after real work, not just step 0.
    for (uint32_t s = 0; s <= 4; ++s)
        frame.colliders[s * 16] = lid(s < 2 ? 1.7f : 1.365f, s < 2 ? 0.f : -.01f);
    first.colliders[0] = first.colliders[16] = frame.colliders[0];
    prepare(first.solver, &first.frame);
    enqueue(first.solver, first.stream, &first.frame, first.colliders, 1, true, false);
    check(cudaStreamSynchronize(first.stream));
    collect(first.solver);
    const auto accepted = first.particles();
    require(std::memcmp(accepted.data(), first.initial.data(), accepted.size() * sizeof(Particle)) != 0,
            "First substep performed no particle work");
    std::vector<std::vector<unsigned char>> original;
    for (uint32_t i = 0; i < BufferCount; ++i)
        original.push_back(read<unsigned char>(frame.buffers[i], bufferBytes(c, Buffer(i)) + 64));
    std::array<std::vector<unsigned char>, OwnershipBufferCount> originalOwned;
    if (authority)
        for (uint32_t i = 0; i < originalOwned.size(); ++i)
            originalOwned[i] = read<unsigned char>(frame.owned[i], ownershipBytes(c, OwnershipBuffer(i)));
    prepare(frame.solver, &frame.frame);
    enqueue(frame.solver, frame.stream, &frame.frame, frame.colliders, 4, true, false);
    check(cudaStreamSynchronize(frame.stream));
    for (uint32_t i = 0; i < BufferCount; ++i)
        require(original[i] == read<unsigned char>(frame.buffers[i], original[i].size()),
                "Late failure published a partial frame");
    if (authority)
        for (uint32_t i = 0; i < originalOwned.size(); ++i)
            require(originalOwned[i] == read<unsigned char>(frame.owned[i], originalOwned[i].size()),
                    "Late rejected frame published only its ownership ledger");
    const auto stopped = read<Particle>(solverParticles(frame.solver), c.capacity);
    require(!std::memcmp(stopped.data(), accepted.data(), stopped.size() * sizeof(Particle)),
            "Particle integration continued beyond failed substep");
    const auto counts = read<uint32_t>(macPressureView(solverMac(frame.solver)).counts, PressureCounterCount);
    require(counts[PressureCalls] == 2 && !counts[PressureConverged] &&
                (counts[PressureCapped] || counts[PressureInvalid]),
            "Late pressure failure was not isolated at substep 2");
    bool rejected = false;
    try {
        collect(frame.solver);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Late failure was not reported at completion fence");
    std::cout << "{\"case\":\"solver-late-reject-" << (graph ? "graph" : "direct")
              << (authority ? "-owned" : "") << "\",\"pass\":true}\n";
}
void largeFlip(bool authority = false) {
    auto c = config(true);
    c.ownedParticles = authority;
    c.nx = c.ny = c.nz = 24;
    c.capacity = 24 * 24 * 24 * 8 + 7;
    Fixture f(c);
    f.frame.solver.z = 1;
    for (uint32_t frame = 0; frame < 6; ++frame)
        f.advance(frame);
    uint32_t active = 0;
    for (const auto &p : f.particles())
        active += p.velocityFlags.w != 0;
    const auto m = solverMac(f.solver);
    const auto counters = read<uint32_t>(macView(m).counters, MacCounterCount);
    const auto pressure = read<uint32_t>(macPressureView(m).counts, PressureCounterCount);
    require(active >= 100000 && counters[MacCoarsePeak] > 0 && pressure[PressureCalls] == 12,
            "100k FLIP test missed active coarse/fine pressure");
    const auto diagnostics = statistics(f.solver);
    require(diagnostics.pressureSolves == 12 && diagnostics.coarsePeak == counters[MacCoarsePeak] &&
                diagnostics.pressureIterations == pressure[PressureTotalIterations] &&
                !diagnostics.rejectedFrames,
            "Deferred pressure counters differ from completed GPU state");
    if (authority) {
        const auto particles = f.particles();
        const auto q = read<double4>(f.owned[OwnedQuantity], c.capacity);
        const auto refs = read<float4>(f.owned[OwnedReference], c.capacity);
        for (uint32_t i = 0; i < c.capacity; ++i) {
            require(q[i].w == (particles[i].velocityFlags.w ? double(f.frame.initialMinimum.w) : 0),
                    "Coarse/fine owned FLIP changed particle rest volume");
            if (!particles[i].velocityFlags.w)
                continue;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                require((&refs[i].x)[axis] == (&particles[i].velocityFlags.x)[axis],
                        "Coarse/fine owned FLIP lost the velocity reference");
                require(std::abs((&q[i].x)[axis] / q[i].w - (&particles[i].velocityFlags.x)[axis]) < 1e-12,
                        "Coarse/fine owned FLIP lost a momentum increment");
            }
        }
    }
    std::cout << "{\"case\":\"solver-100k-FLIP" << (authority ? "-owned" : "") << "\",\"active\":" << active
              << ",\"coarsePeak\":" << counters[MacCoarsePeak]
              << ",\"pressureIterationsPeak\":" << pressure[PressurePeakIterations]
              << ",\"stagingBytes\":" << statistics(f.solver).stagingBytes << ",\"pass\":true}\n";
}
void refinementCounterWrap() {
    Fixture f(config(true), false, true);
    auto v = macView(solverMac(f.solver));
    // Inject counter values around rollover; do not pretend this fixture ran
    // billions of cells. The empty liquid domain contributes no new events.
    const uint32_t before[3] = {0xfffffffbu, 0xfffffffeu, 0xfffffffdu};
    check(cudaMemcpy(v.counters + MacWakeRefinements, before, sizeof(before), cudaMemcpyHostToDevice));
    f.advance(0, 0, true);
    auto s = statistics(f.solver);
    require(s.refinementWakeCells == before[0] && s.refinementTemporalCells == before[1] &&
                s.refinementPaddedCells == before[2],
            "Refinement counter collection lost the high initial sample");
    const uint32_t after[3] = {3, 7, 1};
    check(cudaMemcpy(v.counters + MacWakeRefinements, after, sizeof(after), cudaMemcpyHostToDevice));
    f.advance(1, 0, true);
    collect(f.solver); // Already collected: must not count the same record twice.
    s = statistics(f.solver);
    require(s.refinementWakeCells == (uint64_t(1) << 32) + after[0] &&
                s.refinementTemporalCells == (uint64_t(1) << 32) + after[1] &&
                s.refinementPaddedCells == (uint64_t(1) << 32) + after[2],
            "Refinement counters wrapped or repeated collection duplicated events");
    std::cout << "{\"case\":\"solver-refinement-counter-wrap\",\"pass\":true}\n";
}
void ownedSubsteps(bool mixed, bool graph, bool moving) {
    auto c = config(graph);
    c.mixedPressure = mixed;
    c.pressureIterations = 120;
    c.ownedParticles = true;
    Fixture owned(c, moving);
    c.ownedParticles = false;
    Fixture reference(c, moving);
    double maximum = 0, momentumError = 0;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        const auto before = owned.particles();
        const auto old = read<double4>(owned.owned[OwnedQuantity], c.capacity);
        owned.advance(frame);
        reference.advance(frame);
        const auto a = owned.particles(), b = reference.particles();
        const auto q = read<double4>(owned.owned[OwnedQuantity], c.capacity);
        const auto refs = read<float4>(owned.owned[OwnedReference], c.capacity);
        for (uint32_t i = 0; i < c.capacity; ++i) {
            require(q[i].w == old[i].w, "A CUDA solver substep changed authoritative rest volume");
            if (!a[i].velocityFlags.w)
                continue;
            for (int axis = 0; axis < 3; ++axis) {
                const double target =
                    (&old[i].x)[axis] + q[i].w * (double((&a[i].velocityFlags.x)[axis]) -
                                                  double((&before[i].velocityFlags.x)[axis]));
                momentumError = std::max(momentumError, std::abs((&q[i].x)[axis] - target) / q[i].w);
                require((&refs[i].x)[axis] == (&a[i].velocityFlags.x)[axis],
                        "Owned reference velocity missed a substep");
            }
            for (uint32_t j = 0; j < 20; ++j)
                maximum = std::max(maximum, std::abs(double(reinterpret_cast<const float *>(&a[i])[j]) -
                                                     reinterpret_cast<const float *>(&b[i])[j]));
        }
    }
    require(maximum < .001 && momentumError < 1e-12,
            "Owned substeps changed baseline physics or lost a velocity increment");
    require(statistics(owned.solver).stagingBytes && !state(owned.solver).facesSwapped &&
                !state(owned.solver).pressureIndex,
            "Ownership was not transactionally published");
    std::cout << "{\"case\":\"solver-owned-" << (mixed ? "mixed" : "uniform")
              << (graph ? "-graph" : "-direct") << (moving ? "-moving" : "")
              << "\",\"maxParticleDifference\":" << maximum << ",\"momentumVelocityError\":" << momentumError
              << ",\"pass\":true}\n";
}
void ownedTransfers(bool mixed) {
    auto c = config(mixed);
    c.mixedPressure = mixed;
    c.nx = c.ny = c.nz = 6;
    c.capacity = 6 * 6 * 6 * 8 + 7;
    c.ownedParticles = true;
    Fixture f(c);
    auto particles = f.particles();
    auto q = read<double4>(f.owned[OwnedQuantity], c.capacity);
    for (uint32_t i = 0; i < c.capacity; ++i)
        if (particles[i].velocityFlags.w) {
            const double weight = .173 + double(i % 11) * .073123456789;
            const double volume = double(f.frame.initialMinimum.w) * weight;
            q[i] = make_double4(volume * particles[i].velocityFlags.x, volume * particles[i].velocityFlags.y,
                                volume * particles[i].velocityFlags.z, volume);
            // Consumer oracle: mass/linear velocity must come from the ledger,
            // even when the FP32 cache is deliberately inconsistent.
            particles[i].apic0.w = 7;
            particles[i].velocityFlags.x += 100;
            particles[i].velocityFlags.y -= 100;
        }
    check(cudaMemcpy(f.buffers[Particles], particles.data(), particles.size() * sizeof(Particle),
                     cudaMemcpyHostToDevice));
    check(cudaMemcpy(f.owned[OwnedQuantity], q.data(), q.size() * sizeof(double4), cudaMemcpyHostToDevice));
    auto transfer = createTransfer(c, f.buffers, f.owned);
    try {
        enqueueTransfer(transfer, f.stream, &f.frame, TransferStage::Bin);
        enqueueTransfer(transfer, f.stream, &f.frame, TransferStage::ToGrid);
        check(cudaStreamSynchronize(f.stream));
    } catch (...) {
        destroyTransfer(transfer);
        throw;
    }
    destroyTransfer(transfer);
    const auto mass = read<float>(f.owned[OwnedCellMass], f.frame.grid.w);
    std::vector<double> expectedMass(f.frame.grid.w);
    for (uint32_t i = 0; i < c.capacity; ++i)
        if (particles[i].velocityFlags.w) {
            const auto &p = particles[i];
            const uint32_t x = std::min(uint32_t(p.positionRadius.x / f.frame.minimumCell.w), c.nx - 1);
            const uint32_t y = std::min(uint32_t(p.positionRadius.y / f.frame.minimumCell.w), c.ny - 1);
            const uint32_t z = std::min(uint32_t(p.positionRadius.z / f.frame.minimumCell.w), c.nz - 1);
            expectedMass[(z * c.ny + y) * c.nx + x] += q[i].w;
        }
    for (uint32_t i = 0; i < f.frame.grid.w; ++i)
        require(mass[i] == float(expectedMass[i] / double(f.frame.initialMinimum.w)),
                "Fractional CUDA cell mass was quantized or read from stale caches");
    const auto faces = read<float4>(f.buffers[Faces], bufferBytes(c, Faces) / 16);
    const uint32_t stride = (c.nx + 1) * (c.ny + 1) * (c.nz + 1);
    const auto kernel = [](double v) {
        v = std::abs(v);
        return v < .5 ? .75 - v * v : v < 1.5 ? .5 * (1.5 - v) * (1.5 - v) : 0.;
    };
    double error = 0;
    for (uint32_t id = 0; id < faces.size(); ++id) {
        const uint32_t axis = id / stride, k = id % stride;
        const uint32_t cell[3] = {k % (c.nx + 1), (k / (c.nx + 1)) % (c.ny + 1),
                                  k / ((c.nx + 1) * (c.ny + 1))};
        if (cell[0] >= c.nx + (axis == 0) || cell[1] >= c.ny + (axis == 1) || cell[2] >= c.nz + (axis == 2))
            continue;
        double momentum = 0, weight = 0;
        for (uint32_t i = 0; i < c.capacity; ++i)
            if (particles[i].velocityFlags.w) {
                const auto &p = particles[i];
                double distance[3];
                for (uint32_t a = 0; a < 3; ++a)
                    distance[a] = double((&p.positionRadius.x)[a]) / f.frame.minimumCell.w -
                                  (cell[a] + (axis == a ? 0 : .5));
                const double w = kernel(distance[0]) * kernel(distance[1]) * kernel(distance[2]) *
                                 (q[i].w / f.frame.initialMinimum.w);
                const auto row = axis == 0 ? p.apic0 : axis == 1 ? p.apic1 : p.apic2;
                const double velocity =
                    (&q[i].x)[axis] / q[i].w -
                    f.frame.minimumCell.w * (row.x * distance[0] + row.y * distance[1] + row.z * distance[2]);
                momentum += w * velocity;
                weight += w;
            }
        const double velocity = weight > 1e-8 ? momentum / weight : 0;
        error = std::max({error, std::abs(faces[id].x - velocity), std::abs(faces[id].z - weight)});
    }
    require(error < 1e-5, "CUDA authoritative P2G differs from independent weighted gather");
    // Restore valid caches, then exercise the same fractional ledger through
    // actual pressure, G2P, density repair and whole-frame publication. A P2G
    // oracle alone cannot detect a later substep silently reseeding FP64 mass.
    particles = f.initial;
    for (uint32_t i = 0; i < c.capacity; ++i)
        if (particles[i].velocityFlags.w)
            particles[i].apic0.w = float(q[i].w / double(f.frame.initialMinimum.w));
    f.initial = particles;
    check(cudaMemcpy(f.buffers[Particles], particles.data(), particles.size() * sizeof(Particle),
                     cudaMemcpyHostToDevice));
    double momentumError = 0;
    const auto original = q;
    for (uint32_t frame = 0; frame < 3; ++frame) {
        const auto before = particles;
        const auto previous = q;
        f.advance(frame);
        particles = f.particles();
        q = read<double4>(f.owned[OwnedQuantity], c.capacity);
        const auto refs = read<float4>(f.owned[OwnedReference], c.capacity);
        for (uint32_t i = 0; i < c.capacity; ++i) {
            require(q[i].w == original[i].w, "Composed solver rounded fractional rest volume");
            if (!particles[i].velocityFlags.w)
                continue;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const double expected =
                    (&previous[i].x)[axis] + q[i].w * (double((&particles[i].velocityFlags.x)[axis]) -
                                                       double((&before[i].velocityFlags.x)[axis]));
                momentumError = std::max(momentumError, std::abs((&q[i].x)[axis] - expected) / q[i].w);
                require((&refs[i].x)[axis] == (&particles[i].velocityFlags.x)[axis],
                        "Fractional ledger missed a substep reference velocity");
            }
        }
    }
    require(momentumError < 1e-12, "Fractional composed momentum lost a solver increment");
    std::cout << "{\"case\":\"solver-owned-fractional-" << (mixed ? "mixed-graph" : "uniform-direct")
              << "\",\"maximumFaceError\":" << error << ",\"momentumVelocityError\":" << momentumError
              << ",\"pass\":true}\n";
}
void ownedTinyDelta() {
    auto c = config();
    c.ownedParticles = true;
    Fixture f(c);
    auto q = read<double4>(f.owned[OwnedQuantity], c.capacity);
    q[0].x = std::nextafter(q[0].x, std::numeric_limits<double>::infinity());
    const auto before = q;
    check(cudaMemcpy(f.owned[OwnedQuantity], q.data(), q.size() * sizeof(double4), cudaMemcpyHostToDevice));
    uint32_t *failure = nullptr;
    check(cudaMalloc(&failure, 4));
    try {
        check(cudaMemsetAsync(failure, 0, 4, f.stream));
        enqueueOwnership(f.stream, f.buffers[Particles], f.owned, &f.frame, OwnershipStage::VelocityDelta,
                         failure);
        check(cudaStreamSynchronize(f.stream));
        q = read<double4>(f.owned[OwnedQuantity], c.capacity);
        require(!std::memcmp(q.data(), before.data(), q.size() * sizeof(double4)),
                "Zero cached delta rounded authoritative sub-FP32 momentum");
        require(!read<uint32_t>(failure, 1)[0], "Valid tiny ownership delta was rejected");
    } catch (...) {
        cudaFree(failure);
        throw;
    }
    cudaFree(failure);
    std::cout << "{\"case\":\"solver-owned-sub-FP32-delta\",\"pass\":true}\n";
}
void ownedRejectedInput() {
    for (uint32_t mode = 0; mode < 5; ++mode) {
        auto c = config(true);
        c.ownedParticles = true;
        c.mixedPressure = false;
        Fixture f(c);
        auto q = read<double4>(f.owned[OwnedQuantity], c.capacity);
        auto particles = f.particles();
        if (mode == 0)
            q[0] = make_double4(0, 0, 0, 0);
        if (mode == 1)
            q[c.capacity - 1] = make_double4(0, 0, 0, 1);
        if (mode == 2)
            q[0].x = NAN;
        if (mode == 3)
            particles[0].apic0.w = 8;
        check(cudaMemcpy(f.buffers[Particles], particles.data(), particles.size() * sizeof(Particle),
                         cudaMemcpyHostToDevice));
        check(
            cudaMemcpy(f.owned[OwnedQuantity], q.data(), q.size() * sizeof(double4), cudaMemcpyHostToDevice));
        if (mode == 4) {
            uint32_t one = 1;
            check(cudaMemcpy(static_cast<uint32_t *>(f.owned[OwnedControl]) + 16, &one, 4,
                             cudaMemcpyHostToDevice));
        }
        std::array<std::vector<unsigned char>, BufferCount> original;
        std::array<std::vector<unsigned char>, OwnershipBufferCount> ledger;
        for (uint32_t i = 0; i < BufferCount; ++i)
            original[i] = read<unsigned char>(f.buffers[i], bufferBytes(c, Buffer(i)));
        for (uint32_t i = 0; i < ledger.size(); ++i)
            ledger[i] = read<unsigned char>(f.owned[i], ownershipBytes(c, OwnershipBuffer(i)));
        prepare(f.solver, &f.frame);
        enqueue(f.solver, f.stream, &f.frame, nullptr, 2, false, false);
        check(cudaStreamSynchronize(f.stream));
        bool rejected = false;
        try {
            collect(f.solver);
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected, "Malformed ownership input was not rejected");
        for (uint32_t i = 0; i < BufferCount; ++i)
            require(original[i] == read<unsigned char>(f.buffers[i], original[i].size()),
                    "Rejected ownership modified shared simulation state");
        for (uint32_t i = 0; i < ledger.size(); ++i)
            require(ledger[i] == read<unsigned char>(f.owned[i], ledger[i].size()),
                    "Rejected ownership published a partial ledger");
    }
    std::cout << "{\"case\":\"solver-owned-input-rejection\",\"pass\":true}\n";
}
void ownedInvalidBindings() {
    auto c = config();
    c.ownedParticles = true;
    Fixture f(c);
    uint32_t rejected = 0;
    for (uint32_t mode = 0; mode < 4; ++mode) {
        auto cfg = c;
        auto v = f.owned;
        if (mode == 0)
            v = {};
        if (mode == 1)
            cfg.ownedParticles = false;
        if (mode == 2)
            v[OwnedReference] = v[OwnedQuantity];
        if (mode == 3)
            v[OwnedCellMass] = f.buffers[Counts];
        try {
            auto *s = create(cfg, f.buffers, nullptr, 0, v);
            destroy(s);
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    require(rejected == 4, "Missing, aliased or undeclared ownership views were accepted");
    std::cout << "{\"case\":\"solver-owned-invalid-bindings\",\"pass\":true}\n";
}
using Snapshot = std::vector<std::vector<unsigned char>>;
Snapshot jointSnapshot(const Fixture &f) {
    Snapshot result;
    for (uint32_t i = 0; i < BufferCount; ++i)
        result.push_back(read<unsigned char>(f.buffers[i], bufferBytes(f.cfg, Buffer(i)) + 64));
    for (uint32_t i = 0; i < OwnershipBufferCount; ++i)
        result.push_back(read<unsigned char>(f.owned[i], ownershipBytes(f.cfg, OwnershipBuffer(i)) + 64));
    for (uint32_t i = 0; i < GridInventoryBufferCount; ++i)
        result.push_back(
            read<unsigned char>(f.inventory[i], gridInventoryBytes(f.cfg, GridInventoryBuffer(i)) + 64));
    if (f.surface[0])
        for (uint32_t i = 0; i < SurfaceGeometryBufferCount; ++i)
            result.push_back(read<unsigned char>(f.surface[i],
                                                 surfaceGeometryBytes(f.cfg, SurfaceGeometryBuffer(i)) + 64));
    return result;
}
Config jointConfig(bool graphs = false) {
    auto c = config(graphs);
    c.ownedParticles = true;
    c.densityIterations = 0;
    c.forcedFinePressure = true;
    c.pressureConditionalGraphs = false;
    c.nx = 11;
    c.ny = 9;
    c.nz = 7;
    c.capacity = c.nx * c.ny * c.nz * 8 + 7;
    return c;
}
double jointVolume(const Fixture &f) {
    double result = 0;
    for (const auto &q : read<double4>(f.owned[OwnedQuantity], f.cfg.capacity))
        result += q.w;
    for (const auto &q : read<std::array<double, 4>>(f.inventory[GridOwnedQuantity],
                                                     gridInventoryBytes(f.cfg, GridOwnedQuantity) / 32))
        result += q[3];
    return result;
}
void jointReplay(bool uniform) {
    auto c = jointConfig();
    if (uniform)
        c.forcedFinePressure = c.mixedPressure = false;
    Fixture direct(c, false, false, true);
    c.graphs = true;
    Fixture graph(c, false, false, true);
    const auto original = jointSnapshot(graph);
    const double volume = jointVolume(graph);
    double error = 0;
    for (uint32_t frame = 0; frame < 6; ++frame) {
        for (auto *f : {&direct, &graph}) {
            if (frame == 3) {
                f->frame.gravityDt.y = -4;
                f->frame.solver.z = 1;
            }
            f->advance(frame, frame == 1 || frame == 2 ? 0 : 3, frame == 1, frame == 1);
        }
        const auto a = jointSnapshot(direct), b = jointSnapshot(graph);
        require(a == b, "Joint Solver direct/graph publication differs");
        require(b.back() == original.back(), "Joint Solver modified external fine capacity");
        for (const auto &buffer : b)
            require(std::all_of(buffer.end() - 64, buffer.end(), [](auto v) { return v == 0xcd; }),
                    "Joint Solver damaged shared buffer guard");
        error = std::max(error, std::abs(jointVolume(graph) - volume));
    }
    const auto final = jointSnapshot(graph);
    require(final[Particles] != original[Particles] &&
                final[BufferCount + OwnershipBufferCount] != original[BufferCount + OwnershipBufferCount],
            "Joint Solver did not advance both representations");
    require(error < 1e-11 * volume, "Joint Solver lost total liquid volume");
    const auto s = statistics(graph.solver);
    require(s.graphBuilds == 8 && s.graphReplays == 12 && s.completedFrames == 6 && s.gridTransportBytes,
            "Joint Solver graph/lifecycle accounting differs");
    std::cout << "{\"case\":\"solver-joint-" << (uniform ? "uniform" : "MGPCG")
              << "-lifecycle\",\"volumeError\":" << error << ",\"stagingBytes\":" << s.stagingBytes
              << ",\"transportBytes\":" << s.gridTransportBytes << ",\"pass\":true}\n";
}
void jointRejection(bool graph) {
    for (uint32_t mode = 0; mode < 5; ++mode) {
        Fixture f(jointConfig(graph), false, false, true, true);
        if (mode == 0 || mode == 1) {
            const double bad = mode == 0 ? NAN : -1;
            check(cudaMemcpy(f.inventory[GridFineCapacity], &bad, 8, cudaMemcpyHostToDevice));
        } else {
            std::array<double, 4> q{};
            const double h = f.frame.minimumCell.w;
            // Mode 2 fits the grid owner's own capacity but exceeds the
            // combined particle+grid capacity. Mode 3 is nonfinite momentum.
            q[3] = 8 * h * h * h * .9;
            if (mode == 3 || mode == 4)
                q[0] = NAN;
            check(cudaMemcpy(f.inventory[GridOwnedQuantity], q.data(), 32, cudaMemcpyHostToDevice));
        }
        const auto before = jointSnapshot(f);
        prepare(f.solver, &f.frame);
        enqueue(f.solver, f.stream, &f.frame, nullptr, mode == 4 ? 0 : 4, false, false);
        check(cudaStreamSynchronize(f.stream));
        require(jointSnapshot(f) == before, "Invalid joint inventory partially published a frame");
        bool rejected = false;
        try {
            collect(f.solver);
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected && statistics(f.solver).rejectedFrames == 1,
                "Joint inventory failure was not reported at completion");
    }
    std::cout << "{\"case\":\"solver-joint-reject-" << (graph ? "graph" : "direct")
              << "\",\"variants\":5,\"pass\":true}\n";
}
void jointLateRejection(bool graph) {
    auto c = jointConfig();
    Fixture reference(c, false, false, true, true);
    // A released water block creates a real moving front. A calm full-width
    // pool may legitimately keep identical pressure work indefinitely.
    auto seedParticles = reference.particles();
    auto seedOwned = read<double4>(reference.owned[OwnedQuantity], c.capacity);
    auto seedReferences = read<float4>(reference.owned[OwnedReference], c.capacity);
    for (uint32_t i = 0; i < c.capacity; ++i)
        if (seedParticles[i].positionRadius.x >= 4 * reference.frame.minimumCell.w) {
            seedParticles[i].velocityFlags.w = 0;
            seedOwned[i] = make_double4(0, 0, 0, 0);
            seedReferences[i] = make_float4(0, 0, 0, 0);
        }
    auto seedGrid = read<std::array<double, 4>>(reference.inventory[GridOwnedQuantity],
                                                gridInventoryBytes(c, GridOwnedQuantity) / 32);
    for (uint32_t i = 0; i < seedGrid.size(); ++i)
        if (i % ((c.nx + 1) / 2) >= 2)
            seedGrid[i] = {};
    check(cudaMemcpy(reference.buffers[Particles], seedParticles.data(),
                     seedParticles.size() * sizeof(Particle), cudaMemcpyHostToDevice));
    check(cudaMemcpy(reference.owned[OwnedQuantity], seedOwned.data(), seedOwned.size() * sizeof(double4),
                     cudaMemcpyHostToDevice));
    check(cudaMemcpy(reference.owned[OwnedReference], seedReferences.data(),
                     seedReferences.size() * sizeof(float4), cudaMemcpyHostToDevice));
    check(cudaMemcpy(reference.inventory[GridOwnedQuantity], seedGrid.data(), seedGrid.size() * 32,
                     cudaMemcpyHostToDevice));
    reference.initial = seedParticles;
    uint32_t cap = 0, warmup = 0;
    Snapshot selectedStart;
    std::vector<Particle> acceptedParticles;
    std::vector<unsigned char> acceptedGrid;
    auto loadSnapshot = [&](Fixture &f, const Snapshot &start) {
        uint32_t field = 0;
        for (uint32_t i = 0; i < BufferCount; ++i, ++field)
            check(cudaMemcpy(f.buffers[i], start[field].data(), start[field].size(), cudaMemcpyHostToDevice));
        for (uint32_t i = 0; i < OwnershipBufferCount; ++i, ++field)
            check(cudaMemcpy(f.owned[i], start[field].data(), start[field].size(), cudaMemcpyHostToDevice));
        for (uint32_t i = 0; i < GridInventoryBufferCount; ++i, ++field)
            check(
                cudaMemcpy(f.inventory[i], start[field].data(), start[field].size(), cudaMemcpyHostToDevice));
        for (uint32_t i = 0; i < SurfaceGeometryBufferCount; ++i, ++field)
            check(cudaMemcpy(f.surface[i], start[field].data(), start[field].size(), cudaMemcpyHostToDevice));
    };
    // Find increasing work starting from a COLD solver, then its warmed second
    // step. A snapshot of physical buffers does not include pressure/refinement
    // history. Comparing a new capped solver with a long-running warm reference
    // would manufacture a false rollback failure after warm starts were added.
    for (uint32_t step = 0; step < 64; ++step) {
        const auto start = jointSnapshot(reference);
        Fixture cold(c, false, false, true, true);
        loadSnapshot(cold, start);
        cold.initial = cold.particles();
        cold.advance(0, 1);
        cap = read<uint32_t>(macPressureView(solverMac(cold.solver)).counts,
                             PressureCounterCount)[PressureIterations];
        auto particles = cold.particles();
        auto grid =
            read<unsigned char>(cold.inventory[GridOwnedQuantity], gridInventoryBytes(c, GridOwnedQuantity));
        cold.advance(1, 1);
        const uint32_t required = read<uint32_t>(macPressureView(solverMac(cold.solver)).counts,
                                                 PressureCounterCount)[PressureIterations];
        if (cap && required > cap) {
            warmup = step + 1;
            selectedStart = start;
            acceptedParticles = std::move(particles);
            acceptedGrid = std::move(grid);
            break;
        }
        reference.advance(step, 1);
    }
    require(warmup, "Joint pressure fixture did not produce adjacent steps with increasing work");
    c.graphs = graph;
    c.cgIterations = cap;
    Fixture f(c, false, false, true, true);
    loadSnapshot(f, selectedStart);
    const auto before = jointSnapshot(f);
    prepare(f.solver, &f.frame);
    enqueue(f.solver, f.stream, &f.frame, nullptr, 3, false, false);
    check(cudaStreamSynchronize(f.stream));
    require(jointSnapshot(f) == before, "Late joint pressure rejection published an accepted prefix");
    const auto particles = read<Particle>(solverParticles(f.solver), c.capacity);
    require(!std::memcmp(particles.data(), acceptedParticles.data(), particles.size() * sizeof(Particle)),
            "Late joint failure did not stop particle integration at the accepted prefix");
    require(read<unsigned char>(solverGridQuantity(f.solver), acceptedGrid.size()) == acceptedGrid,
            "Late joint failure did not stop grid transport at the accepted prefix");
    const auto counts = read<uint32_t>(macPressureView(solverMac(f.solver)).counts, PressureCounterCount);
    require(counts[PressureCalls] == 2 && counts[PressureTotalCapped] == 1,
            "Later joint steps erased the pressure failure latch");
    bool rejected = false;
    try {
        collect(f.solver);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Joint late pressure failure was not reported");
    std::cout << "{\"case\":\"solver-joint-late-reject-" << (graph ? "graph" : "direct")
              << "\",\"acceptedSteps\":1,\"referenceSteps\":" << warmup + 1 << ",\"pressureCap\":" << cap
              << ",\"pass\":true}\n";
}
void jointInvalidBindings() {
    Fixture f(jointConfig(), false, false, true);
    uint32_t rejected = 0;
    for (uint32_t mode = 0; mode < 5; ++mode) {
        auto c = f.cfg;
        auto v = f.inventory;
        if (mode == 0)
            v[GridFineCapacity] = nullptr;
        if (mode == 1)
            v[GridFineCapacity] = v[GridOwnedQuantity];
        if (mode == 2)
            v[GridOwnedQuantity] = f.owned[OwnedQuantity];
        if (mode == 3)
            c.densityIterations = 1;
        if (mode == 4)
            c.ballistic = true;
        try {
            auto *s = create(c, f.buffers, nullptr, 0, f.owned, v);
            destroy(s);
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    require(rejected == 5, "Joint Solver accepted unsupported or aliased inventory bindings");
    std::cout << "{\"case\":\"solver-joint-invalid-bindings\",\"pass\":true}\n";
}
void jointGridOnly() {
    Fixture f(jointConfig(true), false, false, true);
    check(cudaMemset(f.buffers[Particles], 0, bufferBytes(f.cfg, Particles)));
    for (uint32_t i = 0; i < OwnershipBufferCount; ++i)
        check(cudaMemset(f.owned[i], 0, ownershipBytes(f.cfg, OwnershipBuffer(i))));
    f.initial = f.particles();
    const double volume = jointVolume(f);
    for (uint32_t frame = 0; frame < 4; ++frame)
        f.advance(frame);
    require(std::abs(jointVolume(f) - volume) < 1e-11 * volume, "Grid-only Solver lost owned water");
    for (uint32_t count : read<uint32_t>(f.buffers[Counts], f.frame.grid.w))
        require(!count, "Grid-only Solver synthesized pressure-support particles");
    uint32_t active = 0;
    for (const auto &cell : read<float4>(f.buffers[Cells], f.frame.grid.w))
        active += cell.z == 1;
    require(active > 0 && active < f.frame.grid.w, "Grid-only Solver lost support or filled the dry domain");
    std::cout << "{\"case\":\"solver-joint-grid-only\",\"activeCells\":" << active << ",\"pass\":true}\n";
}
void jointFullPool() {
    auto c = jointConfig(true);
    Fixture unrolled(c, false, false, true);
    c.pressureConditionalGraphs = true;
    Fixture f(c, false, false, true);
    for (auto *p : {&f, &unrolled}) {
        check(cudaMemset(p->buffers[Particles], 0, bufferBytes(p->cfg, Particles)));
        for (uint32_t i = 0; i < OwnershipBufferCount; ++i)
            check(cudaMemset(p->owned[i], 0, ownershipBytes(p->cfg, OwnershipBuffer(i))));
        p->initial = p->particles();
        auto grid = read<double4>(p->inventory[GridOwnedQuantity],
                                  gridInventoryBytes(p->cfg, GridOwnedQuantity) / 32);
        for (auto &q : grid) {
            q.x *= 5;
            q.y *= 5;
            q.z *= 5;
            q.w *= 5;
        }
        check(cudaMemcpy(p->inventory[GridOwnedQuantity], grid.data(), grid.size() * sizeof(double4),
                         cudaMemcpyHostToDevice));
    }
    const double volume = jointVolume(f);
    uint32_t iterations = 0;
    double excess = 0, reduction = 0, publishedExcess = 0;
    for (uint32_t frame = 0; frame < 8; ++frame) {
        f.advance(frame, 1);
        unrolled.advance(frame, 1);
        require(jointSnapshot(f) == jointSnapshot(unrolled),
                "Conditional capacity admission changed a shared field");
        const auto m = read<GeometricTransportMetrics>(solverGeometricMetrics(f.solver), 1)[0];
        require(!m.capacityCapped, "Full pool exhausted its capacity iteration budget");
        iterations += m.capacityIterations;
        excess = std::max(excess, m.maximumAdmissionError);
        reduction = std::max(reduction, m.maximumFluxReduction);
        const auto values =
            read<double4>(f.inventory[GridOwnedQuantity], gridInventoryBytes(c, GridOwnedQuantity) / 32);
        const uint32_t nx = (c.nx + 1) / 2, ny = (c.ny + 1) / 2;
        const double h = f.frame.minimumCell.w;
        for (uint32_t i = 0; i < values.size(); ++i) {
            const uint32_t x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
            const double capacity = (2 * x + 1 < c.nx ? 2 : 1) * (2 * y + 1 < c.ny ? 2 : 1) *
                                    (2 * z + 1 < c.nz ? 2 : 1) * h * h * h;
            require(std::isfinite(values[i].w) && values[i].w >= 0 && values[i].w <= capacity * (1 + 2e-13),
                    "Independent full-pool capacity audit failed");
            publishedExcess = std::max(publishedExcess, (values[i].w - capacity) / capacity);
        }
    }
    const double volumeError = std::abs(jointVolume(f) - volume);
    require(volumeError < 1e-11 * volume, "Full grid-owned pool lost liquid volume");
    require(iterations > 0 && excess > 2e-13 && reduction > 0,
            "Full pool failed to exercise the pressure-rounding capacity correction");
    require(statistics(f.solver).graphBodyNodes && !statistics(unrolled.solver).graphBodyNodes &&
                statistics(f.solver).graphNodes < statistics(unrolled.solver).graphNodes,
            "Full pool did not compact its bounded iteration graphs");
    std::cout << "{\"case\":\"solver-joint-full-pool\",\"admissionIterations\":" << iterations
              << ",\"initialCapacityError\":" << excess << ",\"maximumFluxReduction\":" << reduction
              << ",\"publishedCapacityExcess\":" << publishedExcess << ",\"volumeError\":" << volumeError
              << ",\"graphNodes\":" << statistics(f.solver).graphNodes
              << ",\"unrolledNodes\":" << statistics(unrolled.solver).graphNodes << ",\"pass\":true}\n";
}
void surfaceLifecycle(bool graph) {
    auto c = jointConfig(graph);
    c.pressureConditionalGraphs = true;
    Fixture f(c, false, false, true, true);
    const uint32_t nx = (c.nx + 1) / 2, ny = (c.ny + 1) / 2, nz = (c.nz + 1) / 2;
    const double h = f.frame.minimumCell.w;
    double phaseError = 0;
    auto audit = [&] {
        const auto phase = read<double2>(f.surface[SurfacePhase], nx * ny * nz);
        const auto planes = read<double4>(f.surface[SurfacePlane], phase.size());
        auto total = read<double4>(f.inventory[GridOwnedQuantity], phase.size());
        const auto particles = f.particles();
        const auto owned = read<double4>(f.owned[OwnedQuantity], c.capacity);
        // Independent host-position enumeration, not the production GPU bins.
        for (uint32_t i = 0; i < particles.size(); ++i) {
            if (!particles[i].velocityFlags.w)
                continue;
            const auto p = particles[i].positionRadius;
            const uint32_t x = std::min(uint32_t(p.x / float(h)), c.nx - 1) / 2;
            const uint32_t y = std::min(uint32_t(p.y / float(h)), c.ny - 1) / 2;
            const uint32_t z = std::min(uint32_t(p.z / float(h)), c.nz - 1) / 2;
            total[(z * ny + y) * nx + x].w += owned[i].w;
        }
        for (uint32_t i = 0; i < phase.size(); ++i) {
            const uint32_t x = i % nx, y = i / nx % ny, z = i / (nx * ny);
            const double capacity = (2 * x + 1 < c.nx ? 2 : 1) * (2 * y + 1 < c.ny ? 2 : 1) *
                                    (2 * z + 1 < c.nz ? 2 : 1) * h * h * h;
            phaseError = std::max(phaseError, std::abs(total[i].w - phase[i].x));
            require(std::isfinite(phase[i].x) && std::isfinite(phase[i].y) &&
                        std::abs(phase[i].y - capacity) <= capacity * 2e-13 &&
                        std::abs(total[i].w - phase[i].x) <= capacity * 2e-13,
                    "Surface snapshot differs from final published owners/capacity");
            const auto p = planes[i];
            const double volume = geometry::fraction(make_double3(p.x, p.y, p.z), p.w) * phase[i].y;
            require(std::isfinite(volume) && std::abs(volume - phase[i].x) <= capacity * 2e-12,
                    "Published surface plane differs from its same-frame phase");
        }
        for (uint32_t i = 0; i < f.surface.size(); ++i) {
            const auto bytes = surfaceGeometryBytes(c, SurfaceGeometryBuffer(i));
            for (auto v : read<unsigned char>(static_cast<char *>(f.surface[i]) + bytes, 64))
                require(v == 0xcd, "Surface publication crossed its allocation guard");
        }
    };
    f.advance(0, 0); // Initial paused output; garbage destination is never an input.
    audit();
    const auto initial = read<unsigned char>(f.surface[SurfacePhase], surfaceGeometryBytes(c, SurfacePhase));
    f.advance(1, 2);
    audit();
    require(read<unsigned char>(f.surface[SurfacePhase], initial.size()) != initial,
            "Surface geometry did not follow accepted fluid motion");
    for (uint32_t i = 0; i < f.surface.size(); ++i)
        check(cudaMemset(f.surface[i], 0xcd, surfaceGeometryBytes(c, SurfaceGeometryBuffer(i))));
    f.advance(2, 0);
    audit();
    check(cudaMemset(f.buffers[Particles], 0, bufferBytes(c, Particles)));
    for (uint32_t i = 0; i < f.owned.size(); ++i)
        check(cudaMemset(f.owned[i], 0, ownershipBytes(c, OwnershipBuffer(i))));
    check(cudaMemset(f.inventory[GridOwnedQuantity], 0, gridInventoryBytes(c, GridOwnedQuantity)));
    f.initial = f.particles();
    f.advance(3, 0, true, true);
    audit();
    for (const auto p : read<double2>(f.surface[SurfacePhase], nx * ny * nz))
        require(p.x == 0, "Reset retained stale renderer phase");
    require(statistics(f.solver).stagingBytes == 895588,
            "Surface publication allocated a duplicate simulation working set");
    std::cout << "{\"case\":\"solver-surface-" << (graph ? "graph" : "direct")
              << "\",\"phaseError\":" << phaseError << ",\"sharedFields\":22,\"pass\":true}\n";
}
void surfaceInvalidBindings() {
    auto c = jointConfig();
    Fixture f(c, false, false, true, true);
    uint32_t rejected = 0;
    for (uint32_t mode = 0; mode < 6; ++mode) {
        auto outputs = f.surface;
        auto inventory = f.inventory;
        if (mode == 0)
            outputs[0] = nullptr;
        if (mode == 1)
            outputs[1] = outputs[0];
        if (mode == 2)
            outputs[0] = inventory[0];
        if (mode == 3)
            outputs[1] = f.owned[OwnedQuantity];
        if (mode == 4)
            inventory = {};
        try {
            if (mode == 5)
                surfaceGeometryBytes(c, SurfaceGeometryBuffer(SurfaceGeometryBufferCount));
            else {
                auto *s = create(c, f.buffers, nullptr, 0, f.owned, inventory, outputs);
                destroy(s);
            }
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    require(rejected == 6, "Invalid/aliased surface geometry API was accepted");
    std::cout << "{\"case\":\"solver-surface-invalid-bindings\",\"variants\":6,\"pass\":true}\n";
}
void jointBirthReset() {
    auto c = jointConfig(true);
    Fixture producer(c, false, false, true), f(c, false, true, true);
    f.advance(0);
    require(jointVolume(f) == 0, "Empty joint Solver invented water");
    const double volume = jointVolume(producer);
    check(cudaMemcpyAsync(f.buffers[Particles], producer.buffers[Particles], bufferBytes(c, Particles),
                          cudaMemcpyDeviceToDevice, f.stream));
    for (uint32_t i = 0; i < OwnershipBufferCount; ++i)
        check(cudaMemcpyAsync(f.owned[i], producer.owned[i], ownershipBytes(c, OwnershipBuffer(i)),
                              cudaMemcpyDeviceToDevice, f.stream));
    check(cudaMemcpyAsync(f.inventory[GridOwnedQuantity], producer.inventory[GridOwnedQuantity],
                          gridInventoryBytes(c, GridOwnedQuantity), cudaMemcpyDeviceToDevice, f.stream));
    check(cudaStreamSynchronize(f.stream));
    f.initial = f.particles();
    f.advance(1);
    require(std::abs(jointVolume(f) - volume) < 1e-11 * volume,
            "Joint Solver missed externally seeded owners");
    check(cudaMemsetAsync(f.buffers[Particles], 0, bufferBytes(c, Particles), f.stream));
    for (uint32_t i = 0; i < OwnershipBufferCount; ++i)
        check(cudaMemsetAsync(f.owned[i], 0, ownershipBytes(c, OwnershipBuffer(i)), f.stream));
    check(cudaMemsetAsync(f.inventory[GridOwnedQuantity], 0, gridInventoryBytes(c, GridOwnedQuantity),
                          f.stream));
    check(cudaStreamSynchronize(f.stream));
    f.initial = f.particles();
    f.advance(2, 0, true, true);
    f.advance(3);
    require(jointVolume(f) == 0 && statistics(f.solver).graphBuilds == 4,
            "Joint reset retained stale inventory or rebuilt unchanged graphs");
    std::cout << "{\"case\":\"solver-joint-birth-reset\",\"pass\":true}\n";
}
} // namespace
int main() try {
    fineReference(false, false);
    fineReference(true, false);
    fineReference(true, true);
    mixedReplay(false);
    mixedReplay(true);
    reject(false, false);
    reject(true, false);
    reject(true, true);
    pausedEmpty();
    resetHistory();
    lateReject(false);
    lateReject(true);
    largeFlip();
    loopSchedule();
    refinementCounterWrap();
    ownedSubsteps(false, false, false);
    ownedSubsteps(false, true, false);
    ownedSubsteps(true, true, false);
    ownedSubsteps(true, true, true);
    ownedTransfers(false);
    ownedTransfers(true);
    ownedTinyDelta();
    ownedRejectedInput();
    ownedInvalidBindings();
    lateReject(true, true);
    largeFlip(true);
    jointReplay(false);
    jointReplay(true);
    jointRejection(false);
    jointRejection(true);
    jointInvalidBindings();
    jointLateRejection(false);
    jointLateRejection(true);
    jointGridOnly();
    jointFullPool();
    jointBirthReset();
    surfaceLifecycle(false);
    surfaceLifecycle(true);
    surfaceInvalidBindings();
    std::cout << "PASS CUDA composed MGPCG: 39 cases; full substeps and guarded publication, not "
                 "renderer/performance acceptance\n";
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA composed MGPCG: " << e.what() << '\n';
    return 1;
}
