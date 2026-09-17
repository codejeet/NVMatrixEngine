#include "../src/fluid/fluid_flowing_band.h"
#include "../src/fluid/fluid_particle_grid_exchange.h"
#include "../src/fluid/fluid_implicit_transport.h"
#include "../src/fluid/fluid_uniforms.h"
#include "../src/fluid/fluid_colliders.h"
#include "fluid_test_binning.h"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {
using namespace lab;
using namespace DirectX;
using Microsoft::WRL::ComPtr;
using Q = std::array<double, 4>;
struct Sample {
    XMFLOAT4 positionRadius, velocityFlags, apic0, apic1, apic2;
};
struct Complexity {
    XMFLOAT4 importance, dynamics, velocity;
    XMUINT4 state;
};
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
constexpr uint32_t n = 16, cells = n * n * n, slots = cells * 8, coarseN = n / 2, coarseCells = cells / 8;
constexpr uint32_t focus = (3 * coarseN + 3) * coarseN + 3;
uint32_t index(uint32_t x, uint32_t y, uint32_t z, uint32_t width) {
    return (z * width + y) * width + x;
}
// Independent oracle assembled from positions, not GPU bins/moment buffers.
XMFLOAT4 measure(const std::vector<Sample> &samples, uint32_t coarse) {
    const double origin[]{double(coarse % coarseN * 2 + 1), double(coarse / coarseN % coarseN * 2 + 1),
                          double(coarse / (coarseN * coarseN) * 2 + 1)};
    std::vector<const Sample *> local;
    double mean[3]{}, center[3]{};
    for (const auto &p : samples) {
        if (!p.velocityFlags.w)
            continue;
        const auto &x = p.positionRadius;
        if (index(uint32_t(x.x) / 2, uint32_t(x.y) / 2, uint32_t(x.z) / 2, coarseN) != coarse)
            continue;
        local.push_back(&p);
        for (uint32_t a = 0; a < 3; a++) {
            mean[a] += (&p.velocityFlags.x)[a];
            center[a] += (&x.x)[a] - origin[a];
        }
    }
    if (local.empty())
        return {};
    for (uint32_t a = 0; a < 3; a++) {
        mean[a] /= local.size();
        center[a] /= local.size();
    }
    double covariance[3][3]{}, angular[3]{}, variance = 0;
    for (const auto *p : local) {
        double displacement[3]{}, velocity[3]{};
        const XMFLOAT4 rows[]{p->apic0, p->apic1, p->apic2};
        for (uint32_t a = 0; a < 3; a++) {
            displacement[a] = (&p->positionRadius.x)[a] - origin[a];
            velocity[a] = (&p->velocityFlags.x)[a] - mean[a];
            variance += velocity[a] * velocity[a];
            for (uint32_t b = 0; b < 3; b++) {
                const double value = (&rows[a].x)[b];
                variance += .25 * value * value;
            }
        }
        for (uint32_t a = 0; a < 3; a++) {
            const uint32_t b = (a + 1) % 3, c = (a + 2) % 3;
            angular[a] += displacement[b] * velocity[c] - displacement[c] * velocity[b] +
                          .25 * ((&rows[c].x)[b] - (&rows[b].x)[c]);
            for (uint32_t j = 0; j < 3; j++)
                covariance[a][j] += (displacement[a] - center[a]) * (displacement[j] - center[j]);
        }
    }
    double covarianceError = 0, centerError = 0, angularError = 0;
    for (uint32_t a = 0; a < 3; a++) {
        centerError += center[a] * center[a];
        angularError += angular[a] * angular[a];
        for (uint32_t b = 0; b < 3; b++)
            covarianceError =
                std::max(covarianceError, std::abs(covariance[a][b] / local.size() - (a == b ? .3125 : 0)));
    }
    return {float(std::sqrt(variance / local.size())), float(std::sqrt(centerError)), float(covarianceError),
            float(std::sqrt(angularError) / local.size())};
}
struct Fixture {
    ID3D12Device *device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t serial = 0, frequency = 0;
    std::vector<gpu::Buffer> uploads;
    explicit Fixture(ID3D12Device *d) : device(d) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        gpu::check(d->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "Band test queue");
        gpu::check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                   "Band allocator");
        gpu::check(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                        IID_PPV_ARGS(&cmd)),
                   "Band command list");
        gpu::check(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Band fence");
        gpu::check(queue->GetTimestampFrequency(&frequency), "Band timestamp frequency");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        check(event != nullptr, "Band event allocation");
    }
    ~Fixture() {
        if (event)
            CloseHandle(event);
    }
    template <class T> gpu::Buffer upload(const std::vector<T> &data) {
        const uint64_t bytes = data.size() * sizeof(T);
        auto staging = gpu::buffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD);
        memcpy(staging.mapped, data.data(), size_t(bytes));
        auto target = gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(target.resource.Get(), 0, staging.resource.Get(), 0, bytes);
        gpu::transition(cmd.Get(), target.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uploads.push_back(std::move(staging));
        return target;
    }
    gpu::Buffer capture(ID3D12Resource *source, uint64_t bytes) {
        auto target = gpu::buffer(device, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(target.resource.Get(), 0, source, 0, bytes);
        gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return target;
    }
    void execute() {
        gpu::check(cmd->Close(), "Band close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Band signal");
        gpu::check(fence->SetEventOnCompletion(serial, event), "Band completion");
        check(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "Band GPU test timeout");
    }
};
template <class T> std::vector<T> read(gpu::Buffer &b, uint32_t count) {
    void *mapped = nullptr;
    D3D12_RANGE range{0, SIZE_T(count * sizeof(T))}, written{0, 0};
    gpu::check(b.resource->Map(0, &range, &mapped), "Band readback");
    std::vector<T> result(count);
    memcpy(result.data(), mapped, count * sizeof(T));
    b.resource->Unmap(0, &written);
    return result;
}
} // namespace

void runFlowingBandTests(ID3D12Device *device, const std::filesystem::path &folder) {
    const char *names[]{"translating interior",
                        "Galilean boost",
                        "shear rejection",
                        "APIC spin rejection",
                        "current free-surface veto",
                        "current solid veto and valid sites",
                        "spatial moment rejection",
                        "physics importance veto",
                        "swept moving-solid margin",
                        "deformed smooth interior",
                        "geometry-blocked restoration"};
    std::vector<uint32_t> referenceDecisions;
    for (uint32_t test = 0; test < std::size(names); test++) {
        Fixture gpu(device);
        std::vector<Sample> samples(slots);
        std::vector<uint32_t> offsets(cells + 1), indices(slots);
        std::vector<XMFLOAT2> fineVolume(cells, XMFLOAT2{1, 1});
        std::vector<std::array<double, 2>> capacity(coarseCells, {8, 8});
        std::vector<XMFLOAT4> solids(cells, XMFLOAT4{100, 0, 0, 0});
        std::vector<Complexity> importance(5 * 5 * 5);
        for (auto &v : importance)
            v.state.x = 2;
        uint32_t live = 0;
        for (uint32_t z = 0; z < n; z++)
            for (uint32_t y = 0; y < n; y++)
                for (uint32_t x = 0; x < n; x++) {
                    const uint32_t cell = index(x, y, z, n);
                    offsets[cell] = live;
                    if (test == 4 && x == 6 && y == 6 && z == 6)
                        continue;
                    for (uint32_t i = 0; i < 8; i++) {
                        auto &p = samples[live];
                        const float jitter = test == 9 ? ((i & 1) ? -.005f : .005f) : 0;
                        p.positionRadius = {x + .25f + .5f * float(i & 1) + jitter,
                                            y + .25f + .5f * float((i >> 1) & 1),
                                            z + .25f + .5f * float(i >> 2), .02f};
                        p.velocityFlags = {test == 1 ? 32.f : .5f, test == 2 ? float(x) * .2f : 0.f, 0, 1};
                        // Small resolvable difference makes circulation carry measurable
                        // momentum while remaining below the policy's velocity error.
                        p.velocityFlags.z = float(x / 2) * .001f;
                        const bool target = x / 2 == 3 && y / 2 == 3 && z / 2 == 3;
                        if (test == 3 && target) {
                            p.apic0.y = -.2f;
                            p.apic1.x = .2f;
                        }
                        if (test == 6 && target)
                            p.positionRadius.y += .12f;
                        p.apic0.w = 1;
                        indices[live] = live;
                        live++;
                    }
                }
        offsets[cells] = live;
        if (test == 5) {
            for (uint32_t z = 6; z < 8; z++)
                for (uint32_t y = 6; y < 8; y++) {
                    solids[index(6, y, z, n)].x = -.5f;
                    fineVolume[index(6, y, z, n)].x = 0;
                }
            capacity[focus][0] = 4;
        }
        if (test == 7)
            importance[index(2, 2, 2, 5)].state.x = 0;
        if (test == 8)
            solids[index(6, 6, 6, n)] = {.6f, 30, 0, 0};
        FluidSimulationConstants c{};
        c.minimumCell = {0, 0, 0, 1};
        c.maximumRadius = {n, n, n, .02f};
        c.gravityDt.w = 1.f / 120;
        c.counts.x = slots;
        c.grid = {n, n, n, cells};
        c.initialMinimum.w = .125f;
        std::vector<FluidCollider> colliders(1);
        XMStoreFloat4x4(&colliders[0].worldToLocal, XMMatrixTranslation(-6.5f, -7.f, -7.f));
        colliders[0].extentType = {.5f, 1, 1, 1}; // left half of target block
        if (test == 5)
            c.collision.x = 1;
        auto frame = gpu::buffer(device, 512, D3D12_HEAP_TYPE_UPLOAD);
        memcpy(frame.mapped, &c, sizeof(c));
        auto particles = gpu.upload(samples), bins = gpu.upload(offsets), ids = gpu.upload(indices),
             volumes = gpu.upload(fineVolume), capacities = gpu.upload(capacity), solid = gpu.upload(solids),
             complexity = gpu.upload(importance), collider = gpu.upload(colliders),
             mesh = gpu.upload(std::vector<float>{100}),
             previous = gpu.upload(std::vector<XMFLOAT4>(slots, XMFLOAT4{-999, -999, -999, -999}));
        // Structured SRVs must use a legal read state, not the UAV state of
        // unrelated fixture inputs. Production bindings already have this state.
        for (auto *r : {collider.resource.Get(), mesh.resource.Get()})
            gpu::transition(gpu.cmd.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        FluidParticleGridExchange exchange(device, folder, c.grid, slots, .125f, .02f);
        FluidParticleGridExchange::View exchangeView{};
        exchangeView.particles = particles.resource.Get();
        exchangeView.previousPositions = previous.resource.Get();
        exchangeView.offsets = bins.resource.Get();
        exchangeView.indices = ids.resource.Get();
        exchangeView.capacity = capacities.resource.Get();
        exchangeView.reusableParticleLimit = live;
        exchange.begin(gpu.cmd.Get(), exchangeView, true);
        exchange.seed(gpu.cmd.Get(), exchangeView, 0, live);
        FluidFlowingBand::Desc desc;
        desc.fine = c.grid;
        desc.maxParticles = slots;
        FluidFlowingBand band(device, folder, desc);
        FluidFlowingBand::View view{frame.resource.Get(),
                                    particles.resource.Get(),
                                    exchange.particleQuantities(),
                                    bins.resource.Get(),
                                    ids.resource.Get(),
                                    exchange.gridRead(),
                                    volumes.resource.Get(),
                                    capacities.resource.Get(),
                                    solid.resource.Get(),
                                    complexity.resource.Get(),
                                    {5, 5, 5, 125},
                                    collider.resource->GetGPUVirtualAddress(),
                                    mesh.resource.Get()};
        band.record(gpu.cmd.Get(), view, true);
        auto early = gpu.capture(band.state(), coarseCells * sizeof(FluidFlowingBand::State));
        band.record(gpu.cmd.Get(), view, false);
        band.record(gpu.cmd.Get(), view, false);
        auto decisions = gpu.capture(band.state(), coarseCells * sizeof(FluidFlowingBand::State));
        auto sites = gpu.capture(band.sites(), coarseCells * 64 * sizeof(XMFLOAT4));
        auto siteCounts = gpu.capture(band.siteCounts(), coarseCells * 4);
        exchangeView.requests = band.requests();
        exchangeView.sites = band.sites();
        exchangeView.siteCounts = band.siteCounts();
        exchangeView.siteStride = 64;
        exchange.deposit(gpu.cmd.Get(), exchangeView);
        auto retired = gpu.capture(exchange.counters(), 256),
             deposited = gpu.capture(exchange.gridRead(), coarseCells * 32);
        // Reuse real conservative implicit transport with a manufactured closed
        // circulation. It tests owner movement, not a claim of live MAC coupling.
        const uint32_t stride = (coarseN + 1) * (coarseN + 1) * (coarseN + 1);
        std::vector<double> rates(stride * 3);
        rates[index(4, 3, 3, coarseN + 1)] = 1;
        rates[stride + index(4, 4, 3, coarseN + 1)] = 1;
        rates[index(4, 4, 3, coarseN + 1)] = -1;
        rates[stride + index(3, 4, 3, coarseN + 1)] = -1;
        auto flow = gpu.upload(rates), flux = gpu.upload(std::vector<Q>(stride * 3)),
             limiter = gpu.upload(std::vector<XMFLOAT2>(coarseCells));
        FluidImplicitTransport transport(device, folder, {coarseN, coarseN, coarseN, coarseCells}, true);
        transport.beginFrame(gpu.cmd.Get(), true, true);
        transport.record(gpu.cmd.Get(), exchange.gridRead(), capacities.resource.Get(), flow.resource.Get(),
                         exchange.gridWrite(), flux.resource.Get(), limiter.resource.Get(), .25f, false);
        exchange.commitGridTransport();
        transport.finishFrame(gpu.cmd.Get());
        auto transported = gpu.capture(exchange.gridRead(), coarseCells * 32),
             transferred = gpu.capture(flux.resource.Get(), stride * 3 * 32);
        // Real post-transaction GPU bins, then measure joint ownership: an
        // interior without particles must remain wet without adding a replica.
        FluidTestBinning rebin(device, folder, cells, slots);
        rebin.record(gpu.cmd.Get(), frame.resource.Get(), particles.resource.Get(), bins.resource.Get(),
                     ids.resource.Get());
        view.gridQuantity = exchange.gridRead();
        band.record(gpu.cmd.Get(), view, false);
        auto mixedDecision = gpu.capture(band.state(), coarseCells * sizeof(FluidFlowingBand::State));
        // Forced restoration needs no new measurement, only current sites.
        gpu::Buffer blockedCollider, blockedFrame;
        if (test == 10) {
            auto closed = colliders;
            XMStoreFloat4x4(&closed[0].worldToLocal, XMMatrixTranslation(-8, -8, -8));
            closed[0].extentType = {16, 16, 16, 1};
            blockedCollider = gpu.upload(closed);
            gpu::transition(gpu.cmd.Get(), blockedCollider.resource.Get(),
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            auto updated = c;
            updated.collision.x = 1;
            blockedFrame = gpu::buffer(device, 512, D3D12_HEAP_TYPE_UPLOAD);
            memcpy(blockedFrame.mapped, &updated, sizeof(updated));
            view.frame = blockedFrame.resource.Get();
            view.colliders = blockedCollider.resource->GetGPUVirtualAddress();
        }
        band.record(gpu.cmd.Get(), view, false, true);
        exchange.restore(gpu.cmd.Get(), exchangeView);
        auto finalQuantities = gpu.capture(exchange.particleQuantities(), slots * 32),
             finalGrid = gpu.capture(exchange.gridRead(), coarseCells * 32),
             finalParticles = gpu.capture(particles.resource.Get(), slots * 80),
             finalPrevious = gpu.capture(previous.resource.Get(), slots * 16),
             finalCounters = gpu.capture(exchange.counters(), 256);
        gpu.execute();
        transport.collect(gpu.frequency);
        auto first = read<FluidFlowingBand::State>(early, coarseCells),
             state = read<FluidFlowingBand::State>(decisions, coarseCells);
        const bool admitted = test == 0 || test == 1 || test == 9 || test == 10;
        check(first[focus].decision.x == 0, "Band promoted before its dwell period");
        if (admitted) {
            if (state[focus].decision.x != 1)
                std::cerr << names[test] << " reason " << state[focus].decision.y << " errors "
                          << state[focus].error.x << ',' << state[focus].error.y << ','
                          << state[focus].error.z << '\n';
            check(state[focus].decision.x == 1, "Smooth flowing interior was rejected");
        } else
            check(state[focus].decision.x == 0, "Protected fluid detail was demoted");
        std::vector<uint32_t> selected;
        for (auto &s : state)
            selected.push_back(s.decision.x);
        if (test == 0)
            referenceDecisions = selected;
        if (test == 1)
            check(selected == referenceDecisions, "A Galilean velocity boost changed error-based admission");
        const uint32_t reasons[]{0,
                                 0,
                                 FluidFlowingBand::VelocityDetail,
                                 FluidFlowingBand::VelocityDetail,
                                 FluidFlowingBand::SurfaceBand,
                                 FluidFlowingBand::SolidBand,
                                 FluidFlowingBand::SpatialDetail,
                                 FluidFlowingBand::PhysicsImportance,
                                 FluidFlowingBand::SolidBand,
                                 0};
        if (!admitted)
            check((state[focus].decision.y & reasons[test]) != 0,
                  "Band rejection did not identify the expected error");
        double maxMomentError = 0;
        for (uint32_t i = 0; i < coarseCells; i++) {
            if (state[i].decision.y & FluidFlowingBand::Invalid)
                continue;
            const auto expected = measure(samples, i);
            for (uint32_t a = 0; a < 4; a++) {
                const double error = std::abs(double((&state[i].error.x)[a]) - (&expected.x)[a]);
                maxMomentError = std::max(maxMomentError, error);
                check(std::isfinite((&state[i].error.x)[a]) && error < 2e-6,
                      "GPU band errors disagree with independent moments");
            }
        }
        auto points = read<XMFLOAT4>(sites, coarseCells * 64);
        auto counts = read<uint32_t>(siteCounts, coarseCells);
        check(counts[focus] == (test == 5 ? 32u : 64u),
              "Restoration sites ignored current collider geometry");
        if (test == 5)
            for (uint32_t i = 0; i < counts[focus]; i++)
                check(points[focus * 64 + i].x > 7, "Generated a restoration point inside the solid");
        auto before = read<Q>(deposited, coarseCells), after = read<Q>(transported, coarseCells),
             transfers = read<Q>(transferred, stride * 3);
        auto end = read<Q>(finalQuantities, slots), grid = read<Q>(finalGrid, coarseCells);
        auto ctr = read<uint32_t>(finalCounters, 64);
        auto cache = read<Sample>(finalParticles, slots);
        auto history = read<XMFLOAT4>(finalPrevious, slots);
        check(ctr[7] == 0 && ctr[16] == 0 && (test == 10 ? ctr[6] == 8 : ctr[6] == 0),
              "Flowing transactions failed or silently deferred");
        const auto retiredCounts = read<uint32_t>(retired, 64);
        const auto mixed = read<FluidFlowingBand::State>(mixedDecision, coarseCells);
        if (admitted) {
            check(mixed[focus].decision.x == 1 && mixed[focus].decision.w == 0,
                  "Grid-owned flowing region disappeared from joint band coverage");
            check(retiredCounts[2] > 0 && (test == 10 ? ctr[3] == 0 : retiredCounts[2] == ctr[3]),
                  "Policy did not retire and restore real particles");
            check(before[focus][3] == 8 && after[focus][3] > 0, "Flowing grid owner missing");
            check(transfers[index(4, 3, 3, coarseN + 1)][3] > .24,
                  "No physical shared face transfer occurred");
            check(std::abs(after[focus][2] - before[focus][2]) > 1e-8,
                  "Grid-owned momentum never moved between cells");
        }
        Q expected{}, actual{};
        double energyBefore = 0, energyAfter = 0, maxError = 0;
        for (uint32_t i = 0; i < live; i++) {
            expected[3] += .125;
            for (uint32_t a = 0; a < 3; a++) {
                const double v = (&samples[i].velocityFlags.x)[a];
                expected[a] += .125 * v;
                energyBefore += .0625 * v * v;
            }
        }
        for (uint32_t i = 0; i < slots; i++) {
            for (uint32_t a = 0; a < 4; a++)
                actual[a] += end[i][a];
            check((cache[i].velocityFlags.w != 0) == (end[i][3] > 0),
                  "Restored cache/authority liveness mismatch");
            if (end[i][3] > 0) {
                check(memcmp(&cache[i].positionRadius, &history[i], 16) == 0,
                      "Restoration inherited old particle motion");
                for (uint32_t a = 0; a < 3; a++)
                    energyAfter += .5 * end[i][a] * end[i][a] / end[i][3];
            }
        }
        for (uint32_t i = 0; i < coarseCells; i++) {
            const auto &q = grid[i];
            check(test == 10 ? q == after[i] : q == Q{}, "Restoration lost or left an unexpected grid owner");
            for (uint32_t a = 0; a < 4; a++)
                actual[a] += q[a];
            if (q[3] > 0)
                for (uint32_t a = 0; a < 3; a++)
                    energyAfter += .5 * q[a] * q[a] / q[3];
        }
        for (uint32_t a = 0; a < 4; a++) {
            maxError = std::max(maxError, std::abs(actual[a] - expected[a]));
            check(std::abs(actual[a] - expected[a]) < 1e-9, "Flowing policy changed joint mass/momentum");
        }
        check(energyAfter <= energyBefore + 1e-8, "Flowing handoff injected translational energy");
        std::cout << "PASS band " << names[test] << " | retired " << retiredCounts[2] << " | joint error "
                  << maxError << " | moment error " << maxMomentError << '\n';
    }
}
