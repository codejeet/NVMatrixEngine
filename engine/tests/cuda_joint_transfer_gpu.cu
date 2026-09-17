#include "../src/fluid/cuda/fluid_cuda_transfer.h"
#include "../src/fluid/cuda/fluid_cuda_device.cuh"
#include "../src/fluid/cuda/fluid_cuda_grid.h"
#include "../src/fluid/cuda/fluid_cuda_mac.h"
#include "../src/fluid/cuda/fluid_cuda_ownership.h"
#include "../src/fluid/cuda/fluid_cuda_owned_transport.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace lab::cuda_fluid;
using namespace lab::cuda_fluid::detail;
namespace {
using Q = std::array<double, 4>;
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
void require(bool b, const char *why) {
    if (!b)
        throw std::runtime_error(why);
}
struct BufferStorage {
    void *p = nullptr;
    size_t bytes;
    explicit BufferStorage(size_t n) : bytes(n) {
        check(cudaMalloc(&p, n + 64));
        check(cudaMemset(p, 0, n));
        check(cudaMemset(static_cast<char *>(p) + n, 0xcd, 64));
    }
    ~BufferStorage() {
        cudaFree(p);
    }
    template <class T> void upload(const std::vector<T> &v) {
        require(v.size() * sizeof(T) == bytes, "Wrong joint fixture upload size");
        check(cudaMemcpy(p, v.data(), bytes, cudaMemcpyHostToDevice));
    }
    template <class T> std::vector<T> read() const {
        std::vector<T> v(bytes / sizeof(T));
        check(cudaMemcpy(v.data(), p, bytes, cudaMemcpyDeviceToHost));
        return v;
    }
    void guard() const {
        std::array<unsigned char, 64> tail{};
        check(cudaMemcpy(tail.data(), static_cast<char *>(p) + bytes, 64, cudaMemcpyDeviceToHost));
        require(std::all_of(tail.begin(), tail.end(), [](auto b) { return b == 0xcd; }),
                "Joint buffer guard changed");
    }
};
double quadraticCPU(double x) {
    x = std::abs(x);
    return x < .5 ? .75 - x * x : x < 1.5 ? .5 * (1.5 - x) * (1.5 - x) : 0;
}
// Independent piecewise Simpson integration, exact for each quadratic piece.
// Does not use the CUDA operator's integer weight tables.
double integral(double a, double b) {
    std::vector<double> split{a, b};
    for (double p : {-1.5, -.5, .5, 1.5})
        if (p > a && p < b)
            split.push_back(p);
    std::sort(split.begin(), split.end());
    double sum = 0;
    for (size_t i = 1; i < split.size(); ++i) {
        double l = split[i - 1], r = split[i];
        sum += (r - l) * (quadraticCPU(l) + 4 * quadraticCPU((l + r) * .5) + quadraticCPU(r)) / 6;
    }
    return sum;
}
struct Fixture {
    Config c{};
    Frame f{};
    uint32_t nx = 11, ny = 9, nz = 7, cx = 6, cy = 5, cz = 4, coarse = 120;
    std::array<std::unique_ptr<BufferStorage>, BufferCount> memory;
    std::array<std::unique_ptr<BufferStorage>, OwnershipBufferCount> ledger;
    void *buffers[BufferCount]{};
    Ownership owned{};
    BufferStorage grid{coarse * 32}, capacities{nx * ny * nz * 8}, output{coarse * 32}, failure{4};
    cudaStream_t stream = nullptr;
    Transfer *transfer = nullptr;
    std::vector<Particle> particles;
    std::vector<Q> pq, gq, fine;
    std::vector<double> open;
    std::vector<float4> references;
    std::vector<std::array<double, 3>> support;
    Fixture(bool particle = true, bool bulk = true, bool affine = false, bool edge = false) {
        c.nx = nx;
        c.ny = ny;
        c.nz = nz;
        c.capacity = 129;
        c.ownedParticles = true;
        c.deterministic = true;
        f.grid = make_uint4(nx, ny, nz, nx * ny * nz);
        f.counts = make_uint4(c.capacity, 0, 0, 4);
        f.minimumCell = make_float4(0, 0, 0, .25f);
        f.maximumRadius = make_float4(nx * .25f, ny * .25f, nz * .25f, .02f);
        f.gravityDt = make_float4(0, -9.81f, 0, .005f);
        f.solver = make_float4(1000, .95f, 0, .125f);
        f.display.w = 2;
        f.initialMinimum.w = .001953125f;
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        for (uint32_t i = 0; i < BufferCount; ++i) {
            memory[i] = std::make_unique<BufferStorage>(bufferBytes(c, Buffer(i)));
            buffers[i] = memory[i]->p;
        }
        for (uint32_t i = 0; i < OwnershipBufferCount; ++i) {
            ledger[i] = std::make_unique<BufferStorage>(ownershipBytes(c, OwnershipBuffer(i)));
            owned[i] = ledger[i]->p;
        }
        particles.resize(c.capacity);
        pq.resize(c.capacity);
        references.resize(c.capacity);
        if (particle)
            for (uint32_t i = 0; i < 32; ++i) {
                auto &p = particles[i];
                p.positionRadius = make_float4(.6f + .17f * (i % 7), .61f + .19f * ((i / 7) % 4),
                                               .62f + .12f * (i % 3), .02f);
                p.velocityFlags = make_float4(.11f + .003f * i, -.09f + .001f * i, .04f - .002f * i, 1);
                p.apic0 = make_float4(0, affine ? .03f : 0, 0, .31f + .007f * i);
                p.apic1 = make_float4(0, 0, affine ? -.02f : 0, 0);
                p.apic2 = make_float4(affine ? .01f : 0, 0, 0, 0);
                double v = double(f.initialMinimum.w) * double(p.apic0.w);
                pq[i] = {v * p.velocityFlags.x, v * p.velocityFlags.y, v * p.velocityFlags.z, v};
                references[i] = make_float4(p.velocityFlags.x, p.velocityFlags.y, p.velocityFlags.z, 0);
            }
        open.resize(f.grid.w);
        gq.resize(coarse);
        fine.resize(f.grid.w);
        support.resize(f.grid.w);
        for (uint32_t i = 0; i < f.grid.w; ++i) {
            uint32_t voxel[]{i % nx, (i / nx) % ny, i / (nx * ny)}, dims[]{nx, ny, nz};
            for (uint32_t axis = 0; axis < 3; ++axis) {
                double value = 1;
                for (uint32_t a = 0; a < 3; ++a) {
                    double sum = 0;
                    for (uint32_t j = 0; j < dims[a] + (a == axis); ++j) {
                        double lo = double(voxel[a]) - j - (a == axis ? 0 : .5);
                        sum += integral(lo, lo + 1);
                    }
                    value *= sum;
                }
                support[i][axis] = value;
            }
        }
        for (uint32_t i = 0; i < f.grid.w; ++i)
            open[i] = .015625 * (.35 + .65 * double(i % 5) / 4);
        for (uint32_t z = 0; z < cz; ++z)
            for (uint32_t y = 0; y < cy; ++y)
                for (uint32_t x = 0; x < cx; ++x) {
                    uint32_t id = (z * cy + y) * cx + x;
                    double capacity = 0;
                    for (uint32_t j = 0; j < 8; ++j) {
                        uint32_t a = 2 * x + (j & 1), b = 2 * y + ((j >> 1) & 1), d = 2 * z + (j >> 2);
                        if (a < nx && b < ny && d < nz)
                            capacity += open[(d * ny + b) * nx + a];
                    }
                    bool active = bulk && (edge || (x >= 1 && x <= 3 && y >= 1 && y <= 2 && z == 1));
                    double v = active ? capacity * (.2 + .025 * (id % 7)) : 0;
                    gq[id] = {v * (-.12 + .01 * (id % 5)), v * (.06 - .013 * (id % 3)),
                              v * (.04 + .002 * (id % 7)), v};
                    for (uint32_t j = 0; j < 8; ++j) {
                        uint32_t a = 2 * x + (j & 1), b = 2 * y + ((j >> 1) & 1), d = 2 * z + (j >> 2);
                        if (a < nx && b < ny && d < nz) {
                            uint32_t i = (d * ny + b) * nx + a;
                            for (uint32_t k = 0; k < 4; ++k)
                                fine[i][k] = gq[id][k] * open[i] / capacity;
                        }
                    }
                }
        memory[Particles]->upload(particles);
        ledger[OwnedQuantity]->upload(pq);
        ledger[OwnedReference]->upload(references);
        memory[Solid]->upload(std::vector<float4>(f.grid.w, make_float4(100, 0, 0, 0)));
        grid.upload(gq);
        capacities.upload(open);
        transfer =
            createTransfer(c, buffers, owned, {grid.p, capacities.p, static_cast<uint32_t *>(failure.p)});
    }
    ~Fixture() {
        cudaStreamSynchronize(stream);
        destroyTransfer(transfer);
        cudaStreamDestroy(stream);
    }
    void predict() {
        enqueueTransfer(transfer, stream, &f, TransferStage::Bin);
        enqueueTransfer(transfer, stream, &f, TransferStage::ToGrid);
        check(cudaStreamSynchronize(stream));
    }
    uint32_t face(uint32_t x, uint32_t y, uint32_t z, uint32_t a) const {
        return a * (nx + 1) * (ny + 1) * (nz + 1) + (z * (ny + 1) + y) * (nx + 1) + x;
    }
    double weight(uint32_t cell, uint32_t faceID) const {
        uint32_t stride = (nx + 1) * (ny + 1) * (nz + 1), axis = faceID / stride, k = faceID % stride;
        uint32_t pos[]{k % (nx + 1), (k / (nx + 1)) % (ny + 1), k / ((nx + 1) * (ny + 1))};
        uint32_t voxel[]{cell % nx, (cell / nx) % ny, cell / (nx * ny)};
        double w = 1;
        for (uint32_t a = 0; a < 3; ++a) {
            double lo = double(voxel[a]) - pos[a] - (a == axis ? 0 : .5);
            w *= integral(lo, lo + 1);
        }
        return w / support[cell][axis];
    }
    void guards() {
        for (auto &b : memory)
            b->guard();
        for (auto &b : ledger)
            b->guard();
        for (auto b : {&grid, &capacities, &output, &failure})
            b->guard();
    }
    double auditPrediction() {
        require(!failure.read<uint32_t>()[0], "Joint prediction rejected valid owners");
        auto faces = memory[Faces]->read<float4>();
        auto mass = ledger[OwnedCellMass]->read<float>();
        std::vector<double> expectedMass(f.grid.w);
        for (uint32_t i = 0; i < fine.size(); ++i)
            expectedMass[i] = fine[i][3];
        for (uint32_t i = 0; i < particles.size(); ++i)
            if (particles[i].velocityFlags.w) {
                const auto &p = particles[i].positionRadius;
                uint32_t x = uint32_t(p.x / f.minimumCell.w), y = uint32_t(p.y / f.minimumCell.w),
                         z = uint32_t(p.z / f.minimumCell.w);
                expectedMass[(z * ny + y) * nx + x] += pq[i][3];
            }
        double error = 0, total = 0, expectedTotal = 0;
        for (uint32_t i = 0; i < mass.size(); ++i) {
            double expected = expectedMass[i] / double(f.initialMinimum.w);
            require(std::abs(mass[i] - expected) < 2e-6,
                    "Joint mass cache disagrees with independent ownership sum");
            total += mass[i] * double(f.initialMinimum.w);
            expectedTotal += expectedMass[i];
        }
        require(std::abs(total - expectedTotal) < 2e-7 * expectedTotal,
                "Joint mass cache lost grid-owned volume");
        const uint32_t stride = (nx + 1) * (ny + 1) * (nz + 1);
        for (uint32_t id = 0; id < faces.size(); ++id) {
            uint32_t axis = id / stride, k = id % stride,
                     pos[]{k % (nx + 1), (k / (nx + 1)) % (ny + 1), k / ((nx + 1) * (ny + 1))},
                     dims[]{nx, ny, nz};
            bool valid = true;
            for (uint32_t a = 0; a < 3; ++a)
                valid &= pos[a] < dims[a] + (a == axis);
            if (!valid) {
                require(faces[id].x == 0 && faces[id].z == 0, "Joint face padding changed");
                continue;
            }
            double m = 0, mom = 0;
            for (uint32_t i = 0; i < fine.size(); ++i)
                if (fine[i][3]) {
                    double w = weight(i, id) / double(f.initialMinimum.w);
                    m += w * fine[i][3];
                    mom += w * fine[i][axis];
                }
            for (uint32_t i = 0; i < particles.size(); ++i)
                if (particles[i].velocityFlags.w) {
                    const auto &p = particles[i];
                    double w = pq[i][3] / double(f.initialMinimum.w), affine = 0;
                    const float4 row = axis == 0 ? p.apic0 : axis == 1 ? p.apic1 : p.apic2;
                    for (uint32_t a = 0; a < 3; ++a) {
                        double q = double((&p.positionRadius.x)[a]) / f.minimumCell.w - pos[a] -
                                   (a == axis ? 0 : .5);
                        w *= quadraticCPU(q);
                        affine += (&row.x)[a] * q * f.minimumCell.w;
                    }
                    m += w;
                    mom += w * (pq[i][axis] / pq[i][3] - affine);
                }
            double velocity = m > 0 ? mom / m : 0;
            error = std::max(error, std::abs(faces[id].x - velocity));
            require(std::abs(faces[id].z - m) < 3e-6, "Joint face mass disagrees with integrated oracle");
            require(std::abs(faces[id].x - velocity) < 3e-6 && faces[id].y == faces[id].x,
                    "Joint face prediction disagrees with oracle");
        }
        guards();
        return error;
    }
    std::vector<Q> returnOracle(const std::vector<float4> &faces) {
        auto result = gq;
        double flip = f.solver.z > 0 ? f.solver.y : 0;
        for (uint32_t i = 0; i < fine.size(); ++i)
            if (fine[i][3]) {
                uint32_t x = i % nx, y = (i / nx) % ny, z = i / (nx * ny),
                         owner = ((z / 2) * cy + y / 2) * cx + x / 2;
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    double pic = 0, delta = 0;
                    for (uint32_t k = axis * (nx + 1) * (ny + 1) * (nz + 1);
                         k < (axis + 1) * (nx + 1) * (ny + 1) * (nz + 1); ++k) {
                        uint32_t j = k % ((nx + 1) * (ny + 1) * (nz + 1)),
                                 p[]{j % (nx + 1), (j / (nx + 1)) % (ny + 1), j / ((nx + 1) * (ny + 1))},
                                 dims[]{nx, ny, nz};
                        bool valid = true;
                        for (uint32_t a = 0; a < 3; ++a)
                            valid &= p[a] < dims[a] + (a == axis);
                        if (!valid)
                            continue;
                        double w = weight(i, k) * fine[i][3];
                        pic += w * faces[k].x;
                        delta += w * (double(faces[k].x) - faces[k].y);
                    }
                    result[owner][axis] += (1 - flip) * (pic - fine[i][axis]) + flip * delta;
                }
            }
        return result;
    }
};
void prediction(const char *name, bool particle, bool affine, bool edge) {
    Fixture f(particle, true, affine, edge);
    f.predict();
    double error = f.auditPrediction();
    enqueueGrid(f.stream, f.buffers, &f.f, GridStage::Classify, 0, false,
                static_cast<uint32_t *>(f.failure.p), f.owned[OwnedCellMass]);
    check(cudaStreamSynchronize(f.stream));
    auto counts = f.memory[Counts]->read<uint32_t>();
    auto cells = f.memory[Cells]->read<float4>();
    uint32_t gridOnly = 0;
    for (uint32_t i = 0; i < cells.size(); ++i)
        if (!counts[i] && f.fine[i][3] > 0) {
            require(cells[i].z == 1, "Grid-owned water classified as air");
            ++gridOnly;
        }
    require(gridOnly > 0, "No grid-only pressure support exercised");
    std::cout << "{\"case\":\"" << name << "\",\"faceError\":" << error << ",\"gridOnlyCells\":" << gridOnly
              << ",\"pass\":true}\n";
}
Q total(const std::vector<Q> &a, const std::vector<Q> &b) {
    Q sum{};
    for (const auto *v : {&a, &b})
        for (auto q : *v)
            for (uint32_t k = 0; k < 4; ++k)
                sum[k] += q[k];
    return sum;
}
double energy(const std::vector<Q> &a, const std::vector<Q> &b) {
    double sum = 0;
    for (const auto *v : {&a, &b})
        for (auto q : *v)
            if (q[3])
                for (uint32_t k = 0; k < 3; ++k)
                    sum += .5 * q[k] * q[k] / q[3];
    return sum;
}
void returned(const char *name, bool flip, bool pressure) {
    Fixture f;
    f.f.solver.z = flip ? 1.f : 0;
    f.predict();
    f.auditPrediction();
    if (pressure) {
        for (auto stage : {GridStage::Classify, GridStage::Forces, GridStage::Divergence})
            enqueueGrid(f.stream, f.buffers, &f.f, stage, 0, false, static_cast<uint32_t *>(f.failure.p),
                        f.owned[OwnedCellMass]);
        uint32_t bricks = ((f.nx + 3) / 4) * ((f.ny + 3) / 4) * ((f.nz + 3) / 4);
        std::unique_ptr<Mac, decltype(&destroyMac)> mac(
            createMac({f.nx, f.ny, f.nz, 120, bricks, bricks, true, .15f, true, 32, false}), destroyMac);
        beginMacFrame(mac.get(), f.stream, true);
        enqueueMac(mac.get(), f.stream, f.buffers, &f.f, f.owned[OwnedCellMass]);
        check(cudaStreamSynchronize(f.stream));
        uint32_t bad;
        check(cudaMemcpy(&bad, macView(mac.get()).pool.control + BrickInvalid, 4, cudaMemcpyDeviceToHost));
        require(!bad, "Joint pressure solve failed");
        auto cells = f.memory[Cells]->read<float4>();
        auto faces = f.memory[Faces]->read<float4>();
        double divergence = 0;
        for (uint32_t i = 0; i < cells.size(); ++i)
            if (cells[i].z == 1) {
                uint32_t p[]{i % f.nx, (i / f.nx) % f.ny, i / (f.nx * f.ny)};
                double d = 0;
                for (uint32_t a = 0; a < 3; ++a) {
                    d -= faces[f.face(p[0], p[1], p[2], a)].x;
                    ++p[a];
                    d += faces[f.face(p[0], p[1], p[2], a)].x;
                    --p[a];
                }
                divergence = std::max(divergence, std::abs(d / f.f.minimumCell.w));
            }
        require(divergence < 1e-4, "Grid-owner pressure support remains divergent");
    }
    auto faces = f.memory[Faces]->read<float4>();
    auto expected = f.returnOracle(faces);
    enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.output.p);
    enqueueTransfer(f.transfer, f.stream, &f.f, TransferStage::ToParticles);
    enqueueOwnership(f.stream, f.buffers[Particles], f.owned, &f.f, OwnershipStage::VelocityDelta,
                     static_cast<uint32_t *>(f.failure.p));
    check(cudaStreamSynchronize(f.stream));
    require(!f.failure.read<uint32_t>()[0], "Joint return rejected valid fields");
    auto g = f.output.read<Q>(), p = f.ledger[OwnedQuantity]->read<Q>();
    double error = 0;
    for (uint32_t i = 0; i < g.size(); ++i) {
        require(g[i][3] == f.gq[i][3], "Grid return changed rest volume");
        for (uint32_t a = 0; a < 3; ++a)
            error = std::max(error, std::abs(g[i][a] - expected[i][a]));
    }
    require(error < 2e-14, "Grid return disagrees with independently integrated oracle");
    auto initial = total(f.pq, f.gq), end = total(p, g);
    for (uint32_t a = 0; a < 4; ++a) {
        double faceImpulse = 0;
        if (a < 3) {
            uint32_t stride = (f.nx + 1) * (f.ny + 1) * (f.nz + 1);
            for (uint32_t i = a * stride; i < (a + 1) * stride; ++i)
                faceImpulse += double(f.f.initialMinimum.w) * faces[i].z * (double(faces[i].x) - faces[i].y);
        }
        require(std::abs(end[a] - initial[a] - faceImpulse) < 2e-7 * initial[3],
                "Joint transfer did not conserve the face impulse");
    }
    if (!pressure)
        require(energy(p, g) <= energy(f.pq, f.gq) + 2e-8, "Joint PIC/FLIP transfer injected energy");
    require(f.grid.read<Q>() == f.gq, "Grid return modified source inventory");
    f.guards();
    std::cout << "{\"case\":\"" << name << "\",\"returnError\":" << error << ",\"pass\":true}\n";
}
void emptyParity() {
    Fixture f(true, false);
    f.predict();
    auto a = f.memory[Faces]->read<float4>();
    std::unique_ptr<Transfer, decltype(&destroyTransfer)> baseline(createTransfer(f.c, f.buffers, f.owned),
                                                                   destroyTransfer);
    enqueueTransfer(baseline.get(), f.stream, &f.f, TransferStage::Bin);
    enqueueTransfer(baseline.get(), f.stream, &f.f, TransferStage::ToGrid);
    check(cudaStreamSynchronize(f.stream));
    auto b = f.memory[Faces]->read<float4>();
    require(!std::memcmp(a.data(), b.data(), a.size() * sizeof(float4)),
            "Empty grid owner changed particle baseline");
    std::cout << "{\"case\":\"joint-empty-grid-parity\",\"pass\":true}\n";
}
void invalid() {
    for (uint32_t kind = 0; kind < 9; ++kind) {
        Fixture f;
        auto q = f.gq;
        auto v = f.open;
        if (kind == 0)
            v[0] = std::numeric_limits<double>::quiet_NaN();
        if (kind == 1)
            v[0] = -.1;
        if (kind == 2)
            v[0] = 1;
        if (kind == 3)
            q[0] = {0, 0, 0, 1};
        if (kind == 4) {
            q[0] = {0, 0, 0, 1e-30};
            for (uint32_t j = 0; j < 8; ++j)
                v[((j >> 2) * f.ny + ((j >> 1) & 1)) * f.nx + (j & 1)] = 0;
        }
        if (kind == 5)
            q[0] = {std::numeric_limits<double>::quiet_NaN(), 0, 0, .001};
        if (kind == 6)
            q[0] = {1, 0, 0, 0};
        if (kind == 7)
            q[0] = {0, 0, 0, -.001};
        if (kind == 8)
            q[0] = {1e100, 0, 0, .001};
        f.grid.upload(q);
        f.capacities.upload(v);
        check(cudaMemset(f.buffers[Faces], 0xab, f.memory[Faces]->bytes));
        check(cudaMemset(f.output.p, 0xcd, f.output.bytes));
        auto before = f.memory[Faces]->read<unsigned char>(), out = f.output.read<unsigned char>();
        f.predict();
        enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.output.p);
        check(cudaStreamSynchronize(f.stream));
        require(f.failure.read<uint32_t>()[0] && f.memory[Faces]->read<unsigned char>() == before &&
                    f.output.read<unsigned char>() == out,
                "Invalid joint source published prediction/return");
        f.guards();
    }
    Fixture f;
    f.predict();
    auto faces = f.memory[Faces]->read<float4>();
    faces[f.face(3, 3, 3, 0)].x = std::numeric_limits<float>::quiet_NaN();
    f.memory[Faces]->upload(faces);
    check(cudaMemset(f.output.p, 0xcd, f.output.bytes));
    auto out = f.output.read<unsigned char>();
    enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.output.p);
    check(cudaStreamSynchronize(f.stream));
    require(f.failure.read<uint32_t>()[0] && f.output.read<unsigned char>() == out,
            "Invalid returned face published grid momentum");
    std::cout << "{\"case\":\"joint-invalid-publication\",\"pass\":true}\n";
}
void graph() {
    Fixture f;
    cudaGraph_t graph;
    cudaGraphExec_t exec;
    check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
    enqueueTransfer(f.transfer, f.stream, &f.f, TransferStage::Bin);
    enqueueTransfer(f.transfer, f.stream, &f.f, TransferStage::ToGrid);
    enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.output.p);
    check(cudaStreamEndCapture(f.stream, &graph));
    check(cudaGraphInstantiate(&exec, graph, 0));
    std::vector<Q> previous;
    for (uint32_t i = 0; i < 4; ++i) {
        check(cudaGraphLaunch(exec, f.stream));
        check(cudaStreamSynchronize(f.stream));
        f.auditPrediction();
        auto q = f.output.read<Q>();
        if (i)
            require(q == previous, "Joint graph replay changed unchanged owners");
        previous = q;
    }
    check(cudaGraphExecDestroy(exec));
    check(cudaGraphDestroy(graph));
    std::cout << "{\"case\":\"joint-graph-replay\",\"scratchBytes\":"
              << gridOwnershipTransferBytes(f.transfer) << ",\"pass\":true}\n";
}
void api() {
    Fixture f;
    uint32_t rejected = 0;
    for (uint32_t i = 0; i < 4; ++i)
        try {
            if (i < 3) {
                GridOwnership g{f.grid.p, f.capacities.p, static_cast<uint32_t *>(f.failure.p)};
                if (i == 0)
                    g.failure = nullptr;
                if (i == 1)
                    g.quantity = f.owned[OwnedQuantity];
                if (i == 2)
                    g.fineCapacity = g.quantity;
                auto p = createTransfer(f.c, f.buffers, f.owned, g);
                destroyTransfer(p);
            } else
                enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.grid.p);
        } catch (const std::exception &) {
            ++rejected;
        }
    require(rejected == 4, "Invalid joint API accepted");
    std::cout << "{\"case\":\"joint-invalid-api\",\"pass\":true}\n";
}
void constantBoundary() {
    Fixture f(false, true, false, true);
    const double velocity[]{.125, -.0625, .03125};
    for (auto &q : f.gq)
        for (uint32_t a = 0; a < 3; ++a)
            q[a] = velocity[a] * q[3];
    for (auto &q : f.fine)
        for (uint32_t a = 0; a < 3; ++a)
            q[a] = velocity[a] * q[3];
    f.grid.upload(f.gq);
    f.predict();
    f.auditPrediction();
    enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.output.p);
    check(cudaStreamSynchronize(f.stream));
    double error = 0;
    const auto result = f.output.read<Q>();
    for (uint32_t i = 0; i < result.size(); ++i)
        for (uint32_t a = 0; a < 4; ++a)
            error = std::max(error, std::abs(result[i][a] - f.gq[i][a]));
    require(error < 2e-15, "Clipped-domain grid transfer damps constant velocity");
    f.guards();
    std::cout << "{\"case\":\"joint-constant-boundary\",\"quantityError\":" << error << ",\"pass\":true}\n";
}
void sequence() {
    Fixture f;
    f.f.counts.w = 0; // Actual particle advection, not the transfer-only fixture.
    f.f.solver.z = 1;
    BufferStorage capacity(f.coarse * 16), rates(3 * (f.cx + 1) * (f.cy + 1) * (f.cz + 1) * 8),
        flux(3 * (f.cx + 1) * (f.cy + 1) * (f.cz + 1) * 32);
    std::vector<std::array<double, 2>> open(f.coarse);
    for (uint32_t i = 0; i < f.open.size(); ++i) {
        uint32_t x = i % f.nx, y = (i / f.nx) % f.ny, z = i / (f.nx * f.ny),
                 owner = ((z / 2) * f.cy + y / 2) * f.cx + x / 2;
        open[owner][0] += f.open[i];
        open[owner][1] += f.open[i];
    }
    capacity.upload(open);
    std::unique_ptr<OwnedTransport, decltype(&destroyOwnedTransport)> transport(
        createOwnedTransport({f.cx, f.cy, f.cz}), destroyOwnedTransport);
    uint32_t bricks = ((f.nx + 3) / 4) * ((f.ny + 3) / 4) * ((f.nz + 3) / 4);
    std::unique_ptr<Mac, decltype(&destroyMac)> mac(
        createMac({f.nx, f.ny, f.nz, 120, bricks, bricks, true, .15f, true, 32, false}), destroyMac);
    const auto initial = total(f.pq, f.gq);
    double volumeError = 0, quantityError = 0;
    for (uint32_t step = 0; step < 4; ++step) {
        f.predict();
        for (auto stage : {GridStage::Classify, GridStage::Forces, GridStage::Divergence})
            enqueueGrid(f.stream, f.buffers, &f.f, stage, 0, false, static_cast<uint32_t *>(f.failure.p),
                        f.owned[OwnedCellMass]);
        beginMacFrame(mac.get(), f.stream, step == 0);
        enqueueMac(mac.get(), f.stream, f.buffers, &f.f, f.owned[OwnedCellMass]);
        check(cudaStreamSynchronize(f.stream));
        uint32_t bad;
        check(cudaMemcpy(&bad, macView(mac.get()).pool.control + BrickInvalid, 4, cudaMemcpyDeviceToHost));
        require(!bad, "Joint sequence pressure rejected");
        enqueueGridOwnershipReturn(f.transfer, f.stream, &f.f, f.output.p);
        enqueueTransfer(f.transfer, f.stream, &f.f, TransferStage::ToParticles);
        enqueueOwnership(f.stream, f.buffers[Particles], f.owned, &f.f, OwnershipStage::VelocityDelta,
                         static_cast<uint32_t *>(f.failure.p));
        check(cudaStreamSynchronize(f.stream));
        const auto before = total({}, f.output.read<Q>());
        enqueueOwnedTransportRates(transport.get(), f.stream, &f.f, f.buffers[Faces], rates.p,
                                   static_cast<uint32_t *>(f.failure.p));
        enqueueOwnedTransport(
            transport.get(), f.stream,
            {f.output.p, capacity.p, rates.p, f.grid.p, flux.p, static_cast<uint32_t *>(f.failure.p)},
            f.f.gravityDt.w);
        check(cudaStreamSynchronize(f.stream));
        require(!f.failure.read<uint32_t>()[0], "Joint sequence rejected grid transport");
        const auto grid = f.grid.read<Q>(), particles = f.ledger[OwnedQuantity]->read<Q>();
        const auto after = total({}, grid), joint = total(particles, grid);
        for (uint32_t a = 0; a < 4; ++a)
            quantityError = std::max(quantityError, std::abs(after[a] - before[a]));
        volumeError = std::max(volumeError, std::abs(joint[3] - initial[3]));
    }
    require(volumeError < 2e-11 * initial[3] && quantityError < 2e-12,
            "Joint sequence transport lost physical quantities");
    auto particles = f.memory[Particles]->read<Particle>();
    double travel = 0;
    for (uint32_t i = 0; i < particles.size(); ++i)
        for (uint32_t a = 0; a < 3; ++a)
            travel = std::max(travel, std::abs(double((&particles[i].positionRadius.x)[a]) -
                                               (&f.particles[i].positionRadius.x)[a]));
    require(travel > 1e-5 && f.grid.read<Q>() != f.gq, "Joint sequence did not advance both representations");
    f.guards();
    capacity.guard();
    rates.guard();
    flux.guard();
    std::cout << "{\"case\":\"joint-transport-sequence\",\"volumeError\":" << volumeError
              << ",\"gridQuantityError\":" << quantityError << ",\"particleTravel\":" << travel
              << ",\"pass\":true}\n";
}
} // namespace
int main() try {
    emptyParity();
    prediction("joint-grid-only", false, false, false);
    prediction("joint-affine-prediction", true, true, false);
    prediction("joint-odd-boundary-capacity", true, false, true);
    returned("joint-PIC-return", false, false);
    returned("joint-FLIP-return", true, false);
    returned("joint-projected-return", true, true);
    invalid();
    graph();
    api();
    constantBoundary();
    sequence();
    std::cout << "PASS CUDA joint transfers: 12 cases; prediction/pressure/return and bounded transport "
                 "sequence, not live interface or rendering acceptance\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA joint transfers: " << e.what() << "\n";
    return 1;
}
