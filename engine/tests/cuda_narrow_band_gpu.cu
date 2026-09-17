#include "../src/fluid/cuda/fluid_cuda_narrow_band.h"
#include "../src/fluid/cuda/fluid_cuda_device.cuh"
#include "../src/fluid/cuda/fluid_cuda_transfer.h"
#include "../src/fluid/cuda/fluid_cuda_grid.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace lab::cuda_fluid;
using namespace lab::cuda_fluid::detail;
namespace {
struct alignas(16) Quantity {
    double x, y, z, w;
};
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
void require(bool v, const char *s) {
    if (!v)
        throw std::runtime_error(s);
}
template <class T> std::vector<T> read(const void *p, size_t n) {
    std::vector<T> v(n);
    check(cudaMemcpy(v.data(), p, n * sizeof(T), cudaMemcpyDeviceToHost));
    return v;
}
struct Fixture {
    Config c{};
    Frame f{};
    void *b[BufferCount]{};
    Ownership o{};
    GridInventory g{};
    float4 *previous = nullptr;
    uint32_t *failure = nullptr;
    cudaStream_t stream = nullptr;
    Transfer *transfer = nullptr;
    NarrowBand *band = nullptr;
    Solver *solver = nullptr;
    std::array<long double, 4> initial{};
    bool irregular = false;
    Fixture(bool graph = false, bool composed = false, bool irregularLattice = false)
        : irregular(irregularLattice) {
        c.nx = c.ny = c.nz = 24;
        c.capacity = 24 * 24 * 24 * (irregular ? 12 : 8) + 64;
        c.ownedParticles = c.narrowBand = true;
        c.pressureIterations = 48;
        c.densityIterations = 4;
        c.viscositySubsteps = 1;
        c.graphs = graph;
        f.grid = make_uint4(24, 24, 24, 24 * 24 * 24);
        f.counts = make_uint4(c.capacity, irregular ? 54 * 49 * 54 : c.capacity - 64, 0, 0);
        f.minimumCell = make_float4(0, 0, 0, .125f);
        f.maximumRadius = make_float4(3, 3, 3, .025f);
        f.gravityDt = make_float4(0, 0, 0, 1.f / 120);
        f.solver = make_float4(1000, .95f, 0, .125f);
        f.material = make_float4(0, 0, 1, 0);
        f.display.w = 2;
        f.initialMinimum.w = irregular ? float(std::pow(3. / 54, 3)) : .125f * .125f * .125f / 8;
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        auto alloc = [&](void **p, size_t n) {
            check(cudaMalloc(p, n));
            check(cudaMemset(*p, 0, n));
        };
        for (uint32_t i = 0; i < BufferCount; ++i)
            alloc(b + i, bufferBytes(c, Buffer(i)));
        for (uint32_t i = 0; i < o.size(); ++i)
            alloc(&o[i], ownershipBytes(c, OwnershipBuffer(i)));
        for (uint32_t i = 0; i < g.size(); ++i)
            alloc(&g[i], gridInventoryBytes(c, GridInventoryBuffer(i)));
        alloc(reinterpret_cast<void **>(&previous), size_t(c.capacity) * 16);
        alloc(reinterpret_cast<void **>(&failure), 4);
        seed();
        initial = total();
        if (composed)
            solver = create(c, b, nullptr, 0, o, g, {}, previous);
        else {
            transfer = createTransfer(c, b, o, {g[0], g[1], failure, true});
            band = createNarrowBand(c, b, o, g, gridOwnershipPhase(transfer), previous, failure);
            resetNarrowBand(band, stream, &f);
            beginNarrowBandFrame(band, stream, &f);
        }
    }
    ~Fixture() {
        if (stream)
            cudaStreamSynchronize(stream);
        destroy(solver);
        destroyNarrowBand(band);
        destroyTransfer(transfer);
        for (auto p : b)
            if (p)
                cudaFree(p);
        for (auto p : o)
            if (p)
                cudaFree(p);
        for (auto p : g)
            if (p)
                cudaFree(p);
        if (previous)
            cudaFree(previous);
        if (failure)
            cudaFree(failure);
        if (stream)
            cudaStreamDestroy(stream);
    }
    void seed() {
        std::vector<Particle> particles(c.capacity);
        std::vector<Quantity> quantities(c.capacity);
        std::vector<float4> references(c.capacity), history(c.capacity, make_float4(77, 77, 77, 77));
        for (uint32_t i = 0; i < f.counts.y; ++i) {
            const uint32_t cell = i / 8, k = i % 8;
            const uint32_t x = cell % 24, y = (cell / 24) % 24, z = cell / (24 * 24);
            if (!irregular && y >= 22)
                continue;
            auto &p = particles[i];
            p.positionRadius =
                make_float4((x + .25f + .5f * (k & 1)) * .125f, (y + .25f + .5f * ((k >> 1) & 1)) * .125f,
                            (z + .25f + .5f * (k >> 2)) * .125f, .025f);
            if (irregular)
                p.positionRadius =
                    make_float4((i % 54 + .5f) * (3.f / 54), ((i / 54) % 49 + .5f) * (3.f / 54),
                                (i / (54 * 49) + .5f) * (3.f / 54), .025f);
            p.velocityFlags = make_float4(0, 0, 0, 1);
            p.apic0.w = 1;
            quantities[i] = {0, 0, 0, f.initialMinimum.w};
            history[i] = p.positionRadius;
        }
        check(cudaMemcpy(b[Particles], particles.data(), particles.size() * 80, cudaMemcpyHostToDevice));
        check(
            cudaMemcpy(o[OwnedQuantity], quantities.data(), quantities.size() * 32, cudaMemcpyHostToDevice));
        check(
            cudaMemcpy(o[OwnedReference], references.data(), references.size() * 16, cudaMemcpyHostToDevice));
        check(cudaMemcpy(previous, history.data(), history.size() * 16, cudaMemcpyHostToDevice));
        check(cudaMemset(g[0], 0, gridInventoryBytes(c, GridOwnedQuantity)));
        std::vector<double> capacities(f.grid.w, .125 * .125 * .125);
        check(cudaMemcpy(g[1], capacities.data(), capacities.size() * 8, cudaMemcpyHostToDevice));
        solids(1e6f);
    }
    void solids(float phi) {
        std::vector<float4> s(f.grid.w, make_float4(phi, 0, 0, 0));
        check(cudaMemcpy(b[Solid], s.data(), s.size() * 16, cudaMemcpyHostToDevice));
    }
    void bin() {
        enqueueTransfer(transfer, stream, &f, TransferStage::Bin, nullptr, failure);
    }
    void exchange(bool force = false, bool age = true) {
        bin();
        exchangeNarrowBand(band, stream, &f, nullptr, nullptr, c.capacity, force, age);
        bin();
    }
    NarrowBandMetrics metrics() {
        measureNarrowBand(band, stream, &f);
        check(cudaStreamSynchronize(stream));
        return read<NarrowBandMetrics>(narrowBandMetrics(band), 1)[0];
    }
    std::array<long double, 4> total() {
        check(cudaStreamSynchronize(stream));
        std::array<long double, 4> q{};
        for (auto values : {read<std::array<double, 4>>(o[0], c.capacity),
                            read<std::array<double, 4>>(g[0], gridInventoryBytes(c, GridOwnedQuantity) / 32)})
            for (const auto &p : values)
                for (uint32_t a = 0; a < 4; ++a)
                    q[a] += p[a];
        return q;
    }
    void audit() {
        auto q = total();
        for (uint32_t a = 0; a < 4; ++a)
            require(std::abs(q[a] - initial[a]) < 1e-11L, "Narrow-band physical inventory changed");
        auto particles = read<Particle>(b[Particles], c.capacity);
        auto owned = read<Quantity>(o[0], c.capacity);
        auto motion = read<float4>(previous, c.capacity);
        for (const auto &q : read<Quantity>(g[0], gridInventoryBytes(c, GridOwnedQuantity) / 32))
            require(std::isfinite(q.w) && q.w >= 0 && q.w <= 8 * .125 * .125 * .125 * (1 + 2e-13),
                    "Narrow-band grid overfilled");
        for (uint32_t i = 0; i < c.capacity; ++i) {
            require((owned[i].w > 0) == bool(particles[i].velocityFlags.w),
                    "Narrow-band cache has duplicate/missing owner");
            if (owned[i].w > 0)
                require(std::abs(double(particles[i].apic0.w) * f.initialMinimum.w / owned[i].w - 1) < 2e-7,
                        "Narrow-band mass cache mismatch");
            if (i >= f.counts.y)
                require(owned[i].w == 0 && motion[i].x == 77, "Restoration consumed a future emitter slot");
        }
        require(read<uint32_t>(failure, 1)[0] == 0, "Narrow-band policy failed");
    }
    void advance(uint32_t steps, bool reset = false, bool rebuild = false) {
        prepare(solver, &f);
        enqueue(solver, stream, &f, nullptr, steps, false, rebuild, reset);
        check(cudaStreamSynchronize(stream));
        collect(solver);
    }
};
void lifecycle() {
    Fixture f;
    // A translating calm core is eligible too: this is not the old stationary
    // dormant-lattice proxy. Audit all three physical momentum components.
    auto particles = read<Particle>(f.b[Particles], f.c.capacity);
    auto quantities = read<Quantity>(f.o[0], f.c.capacity);
    auto refs = read<float4>(f.o[OwnedReference], f.c.capacity);
    for (uint32_t i = 0; i < f.c.capacity; ++i)
        if (particles[i].velocityFlags.w) {
            particles[i].velocityFlags = make_float4(.125f, -.0625f, .25f, 1);
            quantities[i].x = quantities[i].w * .125;
            quantities[i].y = -quantities[i].w * .0625;
            quantities[i].z = quantities[i].w * .25;
            refs[i] = make_float4(.125f, -.0625f, .25f, 0);
        }
    // Slight point-bin overfill must leave a fractional particle owner, not
    // block every neighboring retirement or discard the excess volume.
    const uint32_t extra = ((6 * 24 + 6) * 24 + 6) * 8;
    particles[extra].apic0.w = 1.5f;
    auto &eq = quantities[extra];
    eq.x *= 1.5;
    eq.y *= 1.5;
    eq.z *= 1.5;
    eq.w *= 1.5;
    check(cudaMemcpy(f.b[Particles], particles.data(), particles.size() * 80, cudaMemcpyHostToDevice));
    check(cudaMemcpy(f.o[0], quantities.data(), quantities.size() * 32, cudaMemcpyHostToDevice));
    check(cudaMemcpy(f.o[OwnedReference], refs.data(), refs.size() * 16, cudaMemcpyHostToDevice));
    f.initial = f.total();
    for (uint32_t i = 0; i < 16; ++i)
        f.exchange();
    auto m = f.metrics();
    f.audit();
    require(m.retired > 1000 && m.gridVolume > 0 && m.activeParticles < f.f.counts.y,
            "Calm interior particles were not removed");
    enqueueGrid(f.stream, f.b, &f.f, GridStage::Classify, 0, false, f.failure, f.o[OwnedCellMass], f.g[0]);
    enqueueGrid(f.stream, f.b, &f.f, GridStage::DensityMeasure, 0, false, f.failure, f.o[OwnedCellMass],
                f.g[0]);
    check(cudaStreamSynchronize(f.stream));
    const uint32_t center = (12 * 24 + 12) * 24 + 12;
    require(read<float4>(f.b[Cells], f.f.grid.w)[center].z == 1, "Grid-only cell disappeared from pressure");
    require(std::abs(read<float4>(f.b[Density], f.f.grid.w)[center].w - 1) < 2e-6,
            "Grid-owned density missing");
    const auto gridBefore = read<Quantity>(f.g[0], gridInventoryBytes(f.c, GridOwnedQuantity) / 32);
    auto disturbed = gridBefore;
    const uint32_t owner = (6 * 12 + 6) * 12 + 6;
    disturbed[owner].x += disturbed[owner].w * .25;
    check(cudaMemcpy(f.g[0], disturbed.data(), disturbed.size() * 32, cudaMemcpyHostToDevice));
    f.initial = f.total(); // Independently account for the explicit test impulse.
    f.exchange();
    auto promoted = f.metrics();
    f.audit();
    require(promoted.restored > 0 && promoted.gridCells > 0,
            "Resolved disturbance did not automatically restore a local particle band");
    f.solids(-1);
    f.exchange(true);
    auto blocked = f.metrics();
    require(blocked.deferred > 0 && blocked.gridVolume == promoted.gridVolume,
            "Blocked restoration destroyed or relocated water");
    f.audit();
    f.solids(1e6f);
    f.exchange(true);
    auto restored = f.metrics();
    f.audit();
    require(restored.gridCells == 0 && restored.restored > 1000,
            "Grid water was not restored to real particles");
    auto tiny = read<Quantity>(f.g[0], gridBefore.size());
    tiny[owner] = {0, 0, 0, 1e-300};
    check(cudaMemcpy(f.g[0], tiny.data(), tiny.size() * 32, cudaMemcpyHostToDevice));
    f.exchange(true);
    f.audit();
    require(read<Quantity>(f.g[0], tiny.size())[owner].w == 1e-300 && f.metrics().deferred > blocked.deferred,
            "Sub-FP32 physical volume was lost or became a zero-weight particle");
    tiny[owner].w = .5 * double(f.f.initialMinimum.w);
    check(cudaMemcpy(f.g[0], tiny.data(), tiny.size() * 32, cudaMemcpyHostToDevice));
    f.initial = f.total();
    f.exchange(true);
    f.audit();
    require(read<Quantity>(f.g[0], tiny.size())[owner].w == tiny[owner].w,
            "A sub-stencil remnant consumed recycled particle slots");
    tiny[owner].w += 8 * double(f.f.initialMinimum.w);
    check(cudaMemcpy(f.g[0], tiny.data(), tiny.size() * 32, cudaMemcpyHostToDevice));
    f.initial = f.total();
    f.exchange(true);
    f.audit();
    require(f.metrics().gridCells == 0, "Accumulated remnant did not become eligible for restoration");
    std::cout << "{\"case\":\"narrow-retire-restore\",\"retired\":" << m.retired
              << ",\"gridVolume\":" << m.gridVolume << ",\"restored\":" << restored.restored
              << ",\"pass\":true}\n";
}
void incommensurateLattice() {
    // Like the deep room: 4 or 5 particles per coarse-cell axis produces raw
    // occupancies near .70 and 1.37 even though the bulk is uniformly filled.
    // A local point-bin threshold must not veto the entire narrow band.
    Fixture f(false, false, true);
    for (uint32_t i = 0; i < 16; ++i)
        f.exchange();
    const auto m = f.metrics();
    f.audit();
    require(m.retired > 1000 && m.gridVolume > 0,
            "Incommensurate lattice was mistaken for empty/compressed bulk");
    f.exchange(true);
    f.audit();
    require(f.metrics().gridCells == 0, "Incommensurate grid owner could not restore");
    std::cout << "{\"case\":\"narrow-incommensurate-lattice\",\"retired\":" << m.retired
              << ",\"gridVolume\":" << m.gridVolume << ",\"pass\":true}\n";
}
void composed(bool graph) {
    Fixture f(graph, true);
    f.advance(16, true);
    auto s = statistics(f.solver);
    f.audit();
    require(s.narrowRetired > 1000 && s.narrowGridVolume > 0,
            "Live solver never invoked narrow-band retirement");
    auto before = f.total();
    const auto owned = read<Quantity>(f.g[0], gridInventoryBytes(f.c, GridOwnedQuantity) / 32);
    const auto particles = read<Particle>(f.b[Particles], f.c.capacity);
    f.advance(0, false, true);
    auto paused = statistics(f.solver);
    f.audit();
    require(paused.narrowRetired == s.narrowRetired && paused.narrowRestored == s.narrowRestored,
            "Paused rebuild changed ownership");
    auto after = read<Quantity>(f.g[0], owned.size());
    require(!std::memcmp(owned.data(), after.data(), owned.size() * 32), "Pause transported grid owners");
    f.seed();
    f.advance(0, true, true);
    auto reset = statistics(f.solver);
    f.audit();
    require(reset.narrowGridVolume == 0 && reset.narrowRetired == 0,
            "Reset retained retired owners or dwell history");
    // Fail a frame after retirement exists and prove that ALL 21 public views,
    // including recycled-particle motion history, remain unchanged.
    f.advance(16);
    auto quantities = read<Quantity>(f.o[0], f.c.capacity);
    quantities[0].w = -1;
    check(cudaMemcpy(f.o[0], quantities.data(), quantities.size() * 32, cudaMemcpyHostToDevice));
    auto snapshot = [&]() {
        std::vector<std::vector<unsigned char>> views;
        for (uint32_t i = 0; i < BufferCount; ++i)
            views.push_back(read<unsigned char>(f.b[i], bufferBytes(f.c, Buffer(i))));
        for (uint32_t i = 0; i < f.o.size(); ++i)
            views.push_back(read<unsigned char>(f.o[i], ownershipBytes(f.c, OwnershipBuffer(i))));
        for (uint32_t i = 0; i < f.g.size(); ++i)
            views.push_back(read<unsigned char>(f.g[i], gridInventoryBytes(f.c, GridInventoryBuffer(i))));
        views.push_back(read<unsigned char>(f.previous, size_t(f.c.capacity) * 16));
        return views;
    };
    const auto accepted = snapshot();
    bool rejected = false;
    try {
        f.advance(3);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected && snapshot() == accepted,
            "Narrow-band rejection published partial inventory or motion history");
    std::cout << "{\"case\":\"narrow-live-" << (graph ? "graph" : "direct")
              << "\",\"retired\":" << s.narrowRetired << ",\"active\":" << s.narrowActiveParticles
              << ",\"pass\":true}\n";
}
} // namespace
int main() {
    try {
        lifecycle();
        incommensurateLattice();
        composed(false);
        composed(true);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
