#include "../src/fluid/cuda/fluid_cuda_mac.h"
#include "../src/fluid/cuda/fluid_cuda_grid.h"
#include "../src/fluid/cuda/fluid_cuda_device.cuh"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
using namespace lab::cuda_fluid;
using lab::cuda_fluid::detail::Frame;
namespace {
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
void require(bool b, const char *why) {
    if (!b)
        throw std::runtime_error(why);
}
template <class T> std::vector<T> read(const void *p, size_t count) {
    std::vector<T> out(count);
    check(cudaMemcpy(out.data(), p, count * sizeof(T), cudaMemcpyDeviceToHost));
    return out;
}
// Test-only ABI mirror: the oracle below never consumes GPU row coefficients
// while constructing its own face-energy matrix and integrated flux RHS.
struct Row {
    uint32_t count;
    float diagonal, rhs, step;
    uint32_t neighbor[24];
    float coefficient[24], boundary, volume;
};
struct Stored {
    Row row;
    float pressure[2];
    double preciseRhs, precisePressure;
};
static_assert(sizeof(Stored) == 240);
__global__ void initialize(Frame f, lab::cuda_fluid::detail::Fields d, bool closed, bool moving, bool empty,
                           bool barrier, bool manufactured, uint32_t wetHeight) {
    using namespace lab::cuda_fluid::detail;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < f.grid.w) {
        int3 p = cellFromIndex(id, f);
        const bool wet = !empty && (closed || p.y < int(wetHeight ? wetHeight : f.grid.y - 2));
        const bool wall = barrier && p.x == int(f.grid.x / 2);
        d.cells[id] = make_float4(0, 0, wall ? 2.f : wet ? 1.f : 0.f, 0);
        d.counts[id] = wet && !wall ? 8u : 0u;
        d.solid[id] = wall     ? make_float4(-.1f, .002f, 0, 0)
                      : moving ? make_float4(.02f, 1, 0, 0)
                               : make_float4(100, 0, 0, 0);
        d.material[id] = make_float4(wet ? .8f : .2f, .7f, 0, 0);
        d.pressure[id] = d.output[id] = 0;
    }
    if (id < 3 * faceStride(f)) {
        int a;
        int3 p;
        float vel = 0;
        if (faceCoordinate(id, f, a, p)) {
            vel = .003f * sinf(.35f * (p.x + p.y + 2 * p.z + a));
            const uint32_t axisCoord = a == 0 ? p.x : a == 1 ? p.y : p.z;
            const uint32_t extent = a == 0 ? f.grid.x : a == 1 ? f.grid.y : f.grid.z;
            if (manufactured)
                vel = float(double(f.gravityDt.w) / (double(f.solver.x) * f.minimumCell.w) *
                            (a == 0   ? 10000.
                             : a == 1 ? 20000.
                                      : -30000.));
            if (a == 1)
                vel += f.gravityDt.y * f.gravityDt.w;
            if (barrier && a == 0 && (p.x == int(f.grid.x / 2) || p.x == int(f.grid.x / 2 + 1)))
                vel = .002f;
            if (!axisCoord || axisCoord == extent)
                vel = 0;
        }
        d.faces[id] = make_float4(vel, .125f, 1, 1);
        d.scratch[id] = make_float4(0, 0, 0, 0);
    }
}
struct Fixture {
    cudaStream_t stream = nullptr;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t executable = nullptr;
    Mac *mac = nullptr;
    MacConfig config;
    Frame frame{};
    void *buffers[BufferCount]{};
    uint32_t faces = 0, pi = 0;
    bool closed = false;
    bool empty = false, barrier = false, manufactured = false;
    uint32_t wetHeight = 0;
    Fixture(MacConfig cfg, bool sealed = false) : config(cfg), closed(sealed) {
        frame.grid = make_uint4(cfg.nx, cfg.ny, cfg.nz, cfg.nx * cfg.ny * cfg.nz);
        frame.minimumCell = make_float4(0, 0, 0, .13f);
        frame.gravityDt = make_float4(0, 0, 0, 1.f / 120);
        frame.solver = make_float4(1000, 0, 0, .125f);
        frame.counts.x = 1;
        faces = 3 * (cfg.nx + 1) * (cfg.ny + 1) * (cfg.nz + 1);
        check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        try {
            for (auto i : {Faces, Scratch, Cells, Solid, Material, Pressure0, Pressure1})
                check(cudaMalloc(buffers + i, size_t(i == Faces || i == Scratch ? faces : frame.grid.w) *
                                                  (i == Pressure0 || i == Pressure1 ? 4 : 16)));
            check(cudaMalloc(buffers + Counts, size_t(frame.grid.w) * 4));
            // GridStage shares the complete solver binding ABI. These fields
            // are not accessed by this projection-only fixture.
            for (auto &p : buffers)
                if (!p)
                    check(cudaMalloc(&p, 256));
            mac = createMac(cfg);
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
        if (executable)
            cudaGraphExecDestroy(executable);
        if (graph)
            cudaGraphDestroy(graph);
        destroyMac(mac);
        for (auto p : buffers)
            if (p)
                cudaFree(p);
        if (stream)
            cudaStreamDestroy(stream);
    }
    void init(bool moving = false) {
        initialize<<<(faces + 127) / 128, 128, 0, stream>>>(frame, lab::cuda_fluid::detail::fields(buffers),
                                                            closed, moving, empty, barrier, manufactured,
                                                            wetHeight);
        enqueueGrid(stream, buffers, &frame, GridStage::Divergence);
    }
    void advance(bool moving = false) {
        beginMacFrame(mac, stream);
        init(moving);
        pi = enqueueMac(mac, stream, buffers, &frame);
        check(cudaStreamSynchronize(stream));
    }
    std::vector<uint32_t> pool() {
        return read<uint32_t>(macView(mac).pool.control, BrickCounterCount);
    }
    std::vector<uint32_t> metrics() {
        return read<uint32_t>(macView(mac).counters, MacCounterCount);
    }
    std::vector<float4> field(Buffer i) {
        return read<float4>(buffers[i], i == Faces || i == Scratch ? faces : frame.grid.w);
    }
};
__global__ void addLargeVortex(Frame f, lab::cuda_fluid::detail::Fields d) {
    using namespace lab::cuda_fluid::detail;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    int axis;
    int3 p;
    if (id >= 3 * faceStride(f) || !faceCoordinate(id, f, axis, p))
        return;
    // Exactly solenoidal integer streamfunction plus the small existing
    // perturbation. Deliberately stresses FP32 face quantization, not a game speed.
    auto psi = [&](int x, int y) { return x * y * (int(f.grid.x) - x) * (int(f.grid.y) - y); };
    int curl = axis == 0   ? psi(p.x, p.y + 1) - psi(p.x, p.y)
               : axis == 1 ? psi(p.x, p.y) - psi(p.x + 1, p.y)
                           : 0;
    d.faces[id].x += float(curl) * 4;
}
MacConfig config(bool fine = false) {
    return {12, 12, 12, 121, 27, 64, fine, .15f};
}
void referenceFine(Fixture &f, const std::vector<float4> &result) {
    f.init();
    uint32_t pi = 0;
    for (uint32_t i = 0; i < f.config.iterations; ++i) {
        enqueueGrid(f.stream, f.buffers, &f.frame, GridStage::Jacobi, pi);
        pi = 1 - pi;
    }
    enqueueGrid(f.stream, f.buffers, &f.frame, GridStage::Project, pi);
    check(cudaStreamSynchronize(f.stream));
    const auto expected = f.field(Faces);
    require(expected.size() == result.size() &&
                !std::memcmp(expected.data(), result.data(), expected.size() * sizeof(float4)),
            "Fine projection changed the baseline face result or cache metadata");
}
void fineCase(bool overflow) {
    auto c = config(true);
    if (overflow)
        c.brickCapacity = 1;
    Fixture f(c);
    f.advance();
    auto metrics = f.metrics(), pool = f.pool();
    require(!pool[BrickInvalid] && !metrics[MacCoarse], "Unexpected fine topology");
    require(overflow ? pool[BrickOverflows] && metrics[MacFallbacks] == 1 && !pool[BrickVersion]
                     : pool[BrickVersion] == 1,
            "Capacity deferral or fine publication not reported");
    referenceFine(f, f.field(Faces));
    std::cout << "{\"case\":\"mac-" << (overflow ? "capacity-fallback" : "forced-fine")
              << "\",\"pass\":true}\n";
}
void budgetCase() {
    auto c = config(true);
    c.changesPerFrame = 2;
    Fixture f(c);
    for (uint32_t i = 0; i < 14; ++i) {
        f.advance();
        const auto p = f.pool();
        require(p[BrickFrameChanges] <= 2 && !p[BrickInvalid], "Exceeded bounded page changes");
        referenceFine(f, f.field(Faces));
    }
    auto p = f.pool();
    require(p[BrickVersion] > 0 && f.metrics()[MacFallbacks] > 0, "Budgeted allocation did not converge");
    std::cout << "{\"case\":\"mac-budget-deferral\",\"pass\":true}\n";
}
// Host-only coordinate math, deliberately independent of CUDA row construction.
using P = std::array<int, 3>;
struct FacePatch {
    P origin;
    int axis, width;
    double distance, velocity, known;
    bool wall;
    std::map<uint32_t, double> weights;
};
void auditMixed(Fixture &f, bool expectMixed = true) {
    const auto v = macView(f.mac);
    const auto status = f.pool(), metrics = f.metrics();
    const bool fallback = !status[BrickReady];
    require(!status[BrickInvalid] &&
                (fallback ? f.config.multigrid && metrics[MacFallbacks] : status[BrickVersion]),
            "Fixture did not produce a valid resident/fallback projection");
    if (expectMixed)
        require(!fallback && metrics[MacCoarse] && metrics[MacJunctions],
                "Fixture failed to exercise a published mixed operator with T junctions");
    uint32_t front = status[BrickFront];
    auto ownership = read<uint32_t>(v.map[front], f.frame.grid.w);
    auto states = read<uint32_t>(v.state[front], v.coarseCount);
    if (fallback) {
        std::fill(states.begin(), states.end(), 0);
        for (uint32_t id = 0; id < f.frame.grid.w; ++id)
            ownership[id] = id;
    }
    const auto pages = read<uint32_t>(v.pool.pages[front], v.pool.bricks);
    const auto keys = read<uint32_t>(v.pool.keys[front], v.pool.capacity);
    const auto active = read<uint32_t>(v.pool.active[front], v.pool.bricks);
    const auto stored = read<Stored>(v.pool.fields[front], v.pool.capacity * 64);
    const auto fine =
        fallback ? read<double>(macPressureView(f.mac).fallback, f.frame.grid.w * 2) : std::vector<double>{};
    const auto result = f.field(Faces), resultCells = f.field(Cells);
    f.init();
    check(cudaStreamSynchronize(f.stream));
    const auto before = f.field(Faces), cells = f.field(Cells), solids = f.field(Solid);
    const P dims{int(f.config.nx), int(f.config.ny), int(f.config.nz)};
    auto inside = [&](P p) {
        return p[0] >= 0 && p[1] >= 0 && p[2] >= 0 && p[0] < dims[0] && p[1] < dims[1] && p[2] < dims[2];
    };
    auto index = [&](P p) { return uint32_t((p[2] * dims[1] + p[1]) * dims[0] + p[0]); };
    auto coord = [&](uint32_t id) {
        return P{int(id % dims[0]), int(id / dims[0] % dims[1]), int(id / (dims[0] * dims[1]))};
    };
    auto face = [&](P p, int a) {
        return a * (f.faces / 3) + (p[2] * (dims[1] + 1) + p[1]) * (dims[0] + 1) + p[0];
    };
    auto width = [&](P p) {
        return inside(p) && (states[(p[2] / 2 * v.cy + p[1] / 2) * v.cx + p[0] / 2] & 1) ? 2 : 1;
    };
    auto wet = [&](P p) { return inside(p) && cells[index(p)].z == 1; };
    std::vector<std::map<uint32_t, double>> matrix(f.frame.grid.w);
    std::vector<double> rhs(f.frame.grid.w), pressure(f.frame.grid.w);
    std::vector<FacePatch> patches;
    const double scale = double(f.frame.solver.x) * f.frame.minimumCell.w / f.frame.gravityDt.w;
    for (int a = 0; a < 3; ++a) {
        P extent = dims;
        ++extent[a];
        for (int z = 0; z < extent[2]; ++z)
            for (int y = 0; y < extent[1]; ++y)
                for (int x = 0; x < extent[0]; ++x) {
                    P right{x, y, z}, left = right;
                    --left[a];
                    if (!wet(left) && !wet(right))
                        continue;
                    if (inside(left) && inside(right) && ownership[index(left)] == ownership[index(right)])
                        continue;
                    int w = std::max(width(left), width(right));
                    if (right[(a + 1) % 3] % w || right[(a + 2) % 3] % w)
                        continue;
                    const double distance = .5 * (width(left) + width(right));
                    std::map<uint32_t, double> b;
                    double velocity = 0, known = 0;
                    bool wall = !inside(left) || !inside(right) || solids[index(left)].x < 0 ||
                                solids[index(right)].x < 0;
                    for (int j = 0; j < w; ++j)
                        for (int i = 0; i < w; ++i) {
                            P q = right;
                            q[(a + 1) % 3] += i;
                            q[(a + 2) % 3] += j;
                            velocity += before[face(q, a)].x / double(w * w);
                            for (int side = 0; side < 2; ++side) {
                                P n = q;
                                n[a] -= 1 - side;
                                double weight = (side ? -1. : 1.) / (w * w);
                                if (wet(n))
                                    b[ownership[index(n)]] += weight;
                                else if (!wall && f.frame.material.y) {
                                    // Constant curvature fixture, same pressure on every air-side boundary.
                                    known += weight * double(f.frame.material.y) * .7f;
                                }
                            }
                        }
                    const double conductance = double(w * w) / distance;
                    patches.push_back({right, a, w, distance, velocity, known, wall, b});
                    for (auto [i, bi] : b) {
                        rhs[i] -= double(w * w) * bi * velocity * scale;
                        if (!wall) {
                            rhs[i] -= conductance * bi * known;
                            for (auto [j, bj] : b)
                                matrix[i][j] += conductance * bi * bj;
                        }
                    }
                }
    }
    double matrixError = 0, rhsError = 0, residualFluxError = 0, before2 = 0, after2 = 0;
    double rhsPressureError = 0, maxDivergence = 0;
    uint32_t leaves = 0, positiveSiblings = 0;
    for (uint32_t id = 0; id < f.frame.grid.w; ++id) {
        P p = coord(id);
        const int w = width(p);
        const P root{p[0] / w * w, p[1] / w * w, p[2] / w * w};
        require(ownership[id] == index(root), "Noncanonical coarse/fine ownership");
        if (!wet(p) || ownership[id] != id)
            continue;
        ++leaves;
        double actualRhs;
        if (fallback) {
            pressure[id] = fine[id * 2 + 1];
            actualRhs = fine[id * 2];
        } else {
            const uint32_t key = (p[2] / 4 * v.pool.by + p[1] / 4) * v.pool.bx + p[0] / 4;
            require(active[key] && pages[key] < v.pool.capacity && keys[pages[key]] == key,
                    "Missing pressure leaf storage");
            const auto &s = stored[pages[key] * 64 + (p[2] & 3) * 16 + (p[1] & 3) * 4 + (p[0] & 3)];
            pressure[id] = f.config.multigrid ? s.precisePressure : s.pressure[f.pi];
            require(s.row.count <= 24 && s.row.volume == w * w * w, "Malformed pressure row");
            auto expected = matrix[id];
            matrixError = std::max(matrixError, std::abs(expected[id] - s.row.diagonal));
            expected.erase(id);
            for (uint32_t j = 0; j < s.row.count; ++j) {
                matrixError =
                    std::max(matrixError, std::abs(expected[s.row.neighbor[j]] - s.row.coefficient[j]));
                expected.erase(s.row.neighbor[j]);
                positiveSiblings += s.row.coefficient[j] > 0;
            }
            for (auto [unused, value] : expected)
                matrixError = std::max(matrixError, std::abs(value));
            actualRhs = f.config.multigrid ? s.preciseRhs : s.row.rhs;
        }
        rhsPressureError = std::max(rhsPressureError, std::abs(rhs[id] - actualRhs));
        // Pressure RHS units scale with rho*h/dt and summed face area. As in
        // the existing DX12 oracle, gate its induced physical divergence, not
        // an arbitrary absolute pressure threshold or cancelling relative RHS.
        rhsError =
            std::max(rhsError, std::abs(rhs[id] - actualRhs) / (scale * w * w * w * f.frame.minimumCell.w));
    }
    require((fallback || leaves == metrics[MacLeaves]) && (!expectMixed || positiveSiblings),
            "Wrong leaf count or missing T-junction sibling terms");
    if (f.manufactured) {
        // Affine pressure is exact at both fine/coarse centroids. Closed Neumann
        // pressure is unique only up to its volume-weighted additive gauge.
        const double vx = before[face({3, 3, 3}, 0)].x, vy = before[face({3, 3, 3}, 1)].x,
                     vz = before[face({3, 3, 3}, 2)].x;
        std::vector<double> expected(f.frame.grid.w);
        double offset = 0, volume = 0;
        for (uint32_t id = 0; id < f.frame.grid.w; ++id)
            if (!matrix[id].empty()) {
                P p = coord(id);
                double w = width(p), mass = w * w * w;
                expected[id] = scale * (vx * (p[0] + .5 * w) + vy * (p[1] + .5 * w) + vz * (p[2] + .5 * w));
                offset += (pressure[id] - expected[id]) * mass;
                volume += mass;
            }
        offset /= volume;
        double error = 0, maxVelocity = 0;
        for (uint32_t id = 0; id < f.frame.grid.w; ++id)
            if (!matrix[id].empty())
                error = std::max(error, std::abs(pressure[id] - expected[id] - offset));
        for (auto v : result)
            maxVelocity = std::max(maxVelocity, std::abs(double(v.x)));
        require(error < .5 && maxVelocity < 1e-5,
                "Manufactured affine pressure not recovered across T junctions");
        std::cout << "\"gaugePressureError\":" << error << ",\"manufacturedVelocityError\":" << maxVelocity
                  << ",";
    }
    double faceGradientError = 0;
    for (const auto &patch : patches) {
        double difference = patch.known;
        for (auto [id, weight] : patch.weights)
            difference += weight * pressure[id];
        const double expected =
            patch.wall ? patch.velocity : patch.velocity + difference / (scale * patch.distance);
        for (int j = 0; j < patch.width; ++j)
            for (int i = 0; i < patch.width; ++i) {
                P q = patch.origin;
                q[(patch.axis + 1) % 3] += i;
                q[(patch.axis + 2) % 3] += j;
                faceGradientError =
                    std::max(faceGradientError, std::abs(result[face(q, patch.axis)].x - expected));
            }
    }
    require(faceGradientError < 2e-8, "Canonical face gradient differs from independent pressure projection");
    for (uint32_t id = 0; id < f.frame.grid.w; ++id) {
        if (matrix[id].empty())
            continue;
        double ap = 0;
        for (auto [j, value] : matrix[id])
            ap += value * pressure[j];
        P p = coord(id);
        const int w = width(p);
        double flux = 0;
        for (int a = 0; a < 3; ++a)
            for (int j = 0; j < w; ++j)
                for (int i = 0; i < w; ++i) {
                    P q = p;
                    q[(a + 1) % 3] += i;
                    q[(a + 2) % 3] += j;
                    flux -= result[face(q, a)].x;
                    q[a] += w;
                    flux += result[face(q, a)].x;
                }
        const double residual = rhs[id] - ap;
        residualFluxError = std::max(residualFluxError,
                                     std::abs(flux + residual / scale) / (w * w * w * f.frame.minimumCell.w));
        maxDivergence = std::max(maxDivergence, std::abs(flux) / (w * w * w * f.frame.minimumCell.w));
        before2 += rhs[id] * rhs[id];
        after2 += residual * residual;
        // Prolongation must give all eight child cells the same divergence,
        // without changing the pre-force velocity cache used by FLIP.
        for (int z = 0; z < w; ++z)
            for (int y = 0; y < w; ++y)
                for (int x = 0; x < w; ++x) {
                    P child{p[0] + x, p[1] + y, p[2] + z};
                    double div = 0;
                    for (int a = 0; a < 3; ++a) {
                        P q = child;
                        div -= result[face(q, a)].x;
                        ++q[a];
                        div += result[face(q, a)].x;
                    }
                    require(std::abs(div - flux / (w * w * w)) < 2e-8,
                            "Incompatible child-face prolongation");
                    require(std::abs(div / f.frame.minimumCell.w - resultCells[index(child)].y) < 2e-7,
                            "Displayed divergence differs from actual face flux");
                }
    }
    for (uint32_t i = 0; i < f.faces; ++i)
        require(result[i].y == before[i].y && result[i].z == before[i].z,
                "Projection damaged FLIP cache/weights");
    require(matrixError < 2e-6 && rhsError < 1e-6 && residualFluxError < 1e-6,
            "Independent face-energy operator/flux audit failed");
    require(after2 < before2 * .05, "Bounded relaxation failed to reduce residual");
    if (f.config.multigrid) {
        const auto pv = macPressureView(f.mac);
        const auto counts = read<uint32_t>(pv.counts, PressureCounterCount);
        const auto scalars = read<double>(pv.scalars, 8);
        require(counts[PressureConverged] && !counts[PressureCapped] && !counts[PressureInvalid] &&
                    !counts[PressureTotalCapped] && counts[PressureIterations] <= 32,
                "MGPCG did not converge in every substep");
        require(maxDivergence < .000101 && scalars[2] <= .0001 && scalars[1] <= scalars[0] * 1.0001e-10,
                "True divergence/relative-residual gate not met");
        require(std::abs(scalars[2] - maxDivergence) < 1e-6,
                "GPU convergence disagrees with independent actual face divergence");
        struct HRow {
            float weight[6], diagonal, boundary;
        };
        const auto hrows = read<HRow>(pv.rows, pv.capacity);
        const auto factor = read<float>(pv.factor, 4096);
        auto previous = matrix;
        P previousDims = dims;
        double hierarchyError = 0, factorError = 0;
        for (uint32_t l = 0; l < pv.levelCount; ++l) {
            auto g = pv.levels[l];
            const uint32_t n = g.x * g.y * g.z;
            const int ratio = l ? 2 : 4;
            std::vector<std::map<uint32_t, double>> next(n);
            auto parent = [&](uint32_t i) {
                const uint32_t x = i % previousDims[0], y = i / previousDims[0] % previousDims[1],
                               z = i / (previousDims[0] * previousDims[1]);
                return (z / ratio * g.y + y / ratio) * g.x + x / ratio;
            };
            for (uint32_t i = 0; i < previous.size(); ++i)
                for (auto [j, value] : previous[i])
                    next[parent(i)][parent(j)] += value;
            for (uint32_t i = 0; i < n; ++i) {
                const auto &r = hrows[g.offset + i];
                std::map<uint32_t, double> actual{{i, r.diagonal}};
                const P p{int(i % g.x), int(i / g.x % g.y), int(i / (g.x * g.y))};
                const P extent{int(g.x), int(g.y), int(g.z)};
                for (int axis = 0; axis < 3; ++axis)
                    for (int s = 0; s < 2; ++s) {
                        P q = p;
                        q[axis] += s ? 1 : -1;
                        if (q[axis] >= 0 && q[axis] < extent[axis]) {
                            uint32_t j = (q[2] * g.y + q[1]) * g.x + q[0];
                            actual[j] -= r.weight[axis * 2 + s];
                            require(r.weight[axis * 2 + s] == hrows[g.offset + j].weight[axis * 2 + 1 - s],
                                    "Hierarchy faces lost exact symmetry");
                        } else
                            require(r.weight[axis * 2 + s] == 0, "Hierarchy leaks across domain boundary");
                    }
                double boundary = 0;
                for (auto [j, value] : next[i]) {
                    boundary += value;
                    hierarchyError =
                        std::max(hierarchyError, std::abs(actual[j] - value) / (1 + std::abs(value)));
                    actual.erase(j);
                }
                for (auto [j, value] : actual)
                    hierarchyError = std::max(hierarchyError, std::abs(value));
                hierarchyError =
                    std::max(hierarchyError, std::abs(boundary - r.boundary) / (1 + std::abs(boundary)));
            }
            previous = std::move(next);
            previousDims = {int(g.x), int(g.y), int(g.z)};
        }
        auto bottom = pv.levels[pv.levelCount - 1];
        const uint32_t bn = bottom.x * bottom.y * bottom.z;
        float scaleDiagonal = 1;
        for (uint32_t i = 0; i < bn; ++i)
            scaleDiagonal = std::max(scaleDiagonal, hrows[bottom.offset + i].diagonal);
        const double shift = scaleDiagonal * 1e-4f;
        for (uint32_t i = 0; i < bn; ++i)
            for (uint32_t j = 0; j < bn; ++j) {
                double a = 0;
                for (uint32_t k = 0; k <= std::min(i, j); ++k)
                    a += double(factor[i * 64 + k]) * factor[j * 64 + k];
                const double expected = previous[i][j] + (i == j ? shift : 0);
                factorError = std::max(factorError, std::abs(a - expected) / (1 + std::abs(expected)));
            }
        require(hierarchyError < 2e-5 && factorError < 2e-5,
                "Galerkin hierarchy or Cholesky factor differs from independent aggregation");
        std::cout << "\"hierarchyError\":" << hierarchyError << ",\"factorError\":" << factorError << ",";
        std::cout << "\"cgIterations\":" << counts[PressureIterations] << ",";
    }
    std::cout << "\"matrixError\":" << matrixError << ",\"rhsDivergenceError\":" << rhsError
              << ",\"rhsPressureError\":" << rhsPressureError
              << ",\"residualDivergenceError\":" << residualFluxError
              << ",\"faceGradientError\":" << faceGradientError << ",\"maxDivergence\":" << maxDivergence;
}
void pressureHistory(bool graph) {
    auto c = config(true);
    c.multigrid = true;
    Fixture f(c);
    const auto pv = macPressureView(f.mac);
    if (graph) {
        check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
        beginMacFrame(f.mac, f.stream);
        f.init();
        f.pi = enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
        check(cudaStreamEndCapture(f.stream, &f.graph));
        check(cudaGraphInstantiate(&f.executable, f.graph, 0));
        for (int i = 0; i < 3; ++i)
            check(cudaGraphLaunch(f.executable, f.stream));
        check(cudaStreamSynchronize(f.stream));
    } else
        for (int i = 0; i < 3; ++i)
            f.advance();
    auto counters = read<uint32_t>(pv.counts, PressureCounterCount);
    require(counters[PressureHierarchyBuilds] == 1 && counters[PressureHierarchyReuses] == 2 &&
                counters[PressureWarmStarts] == 2 && !counters[PressureIterations] && !f.pool()[BrickInvalid],
            "Unchanged pressure topology/RHS did not reuse its hierarchy and converged pressure");
    const auto reference = f.field(Faces);
    // A bad but finite historical guess must be rejected, not force extra
    // iterations or loosen the original ||b|| / physical divergence threshold.
    std::vector<double> bad(f.frame.grid.w);
    for (size_t i = 0; i < bad.size(); ++i)
        bad[i] = (i & 1) ? 1e12 : -1e12;
    check(cudaMemcpyAsync(pv.previousPressure, bad.data(), bad.size() * sizeof(double),
                          cudaMemcpyHostToDevice, f.stream));
    f.advance();
    counters = read<uint32_t>(pv.counts, PressureCounterCount);
    require(counters[PressureWarmStarts] == 2 && counters[PressureIterations] > 0 && !f.pool()[BrickInvalid],
            "Worse warm guess was not discarded");
    auto output = f.field(Faces);
    for (size_t i = 0; i < output.size(); ++i)
        require(std::abs(reference[i].x - output[i].x) < 2e-7f, "Warm fallback changed projected velocity");
    // Same topology, new velocity RHS: cache only the operator, not forcing.
    f.frame.gravityDt.y = -1;
    f.advance();
    require(read<uint32_t>(pv.counts, PressureCounterCount)[PressureHierarchyBuilds] == 1 &&
                !f.pool()[BrickInvalid],
            "RHS-only change rebuilt the pressure hierarchy or failed");
    // A wet/air change, then a solid change, must each invalidate the cache.
    f.wetHeight = f.frame.grid.y - 3;
    f.advance();
    require(read<uint32_t>(pv.counts, PressureCounterCount)[PressureHierarchyBuilds] == 2 &&
                !f.pool()[BrickInvalid],
            "Free-surface change retained a stale matrix");
    f.barrier = true;
    f.advance();
    require(read<uint32_t>(pv.counts, PressureCounterCount)[PressureHierarchyBuilds] == 3 &&
                !f.pool()[BrickInvalid],
            "Solid change retained a stale matrix");
    // Reset must invalidate both row/hierarchy history and pressure history.
    beginMacFrame(f.mac, f.stream, true);
    f.advance();
    counters = read<uint32_t>(pv.counts, PressureCounterCount);
    require(counters[PressureHierarchyBuilds] == 4 && !counters[PressureUseWarmStart] &&
                !counters[PressureTotalCapped] && !f.pool()[BrickInvalid],
            "Reset reused pressure history or lost convergence");
    std::cout << "{\"case\":\"mgpcg-history-" << (graph ? "graph" : "direct") << "\",\"pass\":true}\n";
}
void refinementGuardEquivalence() {
    auto c = config();
    c.nx = 13;
    c.ny = 11;
    c.nz = 15;
    c.brickCapacity = 48;
    Fixture f(c);
    for (int moving = 0; moving < 2; ++moving) {
        f.advance(moving != 0);
        const auto v = macView(f.mac);
        const auto state = read<RefinementState>(v.nextRefinement, v.coarseCount);
        const auto cells = f.field(Cells), solids = f.field(Solid);
        for (uint32_t id = 0; id < v.coarseCount; ++id) {
            uint32_t expected = RefinementValid;
            const int bx = 2 * (id % v.cx), by = 2 * (id / v.cx % v.cy), bz = 2 * (id / (v.cx * v.cy));
            for (int z = -2; z < 4; ++z)
                for (int y = -2; y < 4; ++y)
                    for (int x = -2; x < 4; ++x) {
                        const int px = bx + x, py = by + y, pz = bz + z;
                        if (px < 0 || py < 0 || pz < 0 || px >= int(c.nx) || py >= int(c.ny) ||
                            pz >= int(c.nz)) {
                            expected |= RefinementSurfaceGuard;
                            continue;
                        }
                        const uint32_t i = (pz * c.ny + py) * c.nx + px;
                        if (cells[i].z != 1 || solids[i].x < 0)
                            expected |= RefinementSurfaceGuard;
                        const auto s = solids[i];
                        const float speed = std::sqrt(s.y * s.y + s.z * s.z + s.w * s.w);
                        if (speed > 0 && s.x < c.predictionSeconds * speed)
                            expected |= RefinementMovingSolid;
                    }
            require(state[id].flags == expected, "Aggregated refinement guard differs from 6^3 oracle");
        }
    }
    std::cout << "{\"case\":\"mac-aggregated-guard-oracle\",\"pass\":true}\n";
}
void mixedCase(bool closed, bool graph, bool multigrid = false) {
    auto c = config();
    c.multigrid = multigrid;
    Fixture f(c, closed);
    if (!closed)
        f.frame.material.y = .072f;
    if (graph) {
        check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
        beginMacFrame(f.mac, f.stream);
        f.init();
        f.pi = enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
        check(cudaStreamEndCapture(f.stream, &f.graph));
        check(cudaGraphInstantiate(&f.executable, f.graph, 0));
        for (int i = 0; i < 9; ++i)
            check(cudaGraphLaunch(f.executable, f.stream));
        check(cudaStreamSynchronize(f.stream));
    } else
        for (int i = 0; i < 9; ++i)
            f.advance();
    std::cout << "{\"case\":\"" << (multigrid ? "mgpcg-" : "mac-")
              << (graph    ? "graph-mixed"
                  : closed ? "closed-mixed"
                           : "surface-mixed")
              << "\",";
    auditMixed(f);
    std::cout << ",\"pass\":true}\n";
}
void movingCase() {
    Fixture f(config());
    for (int i = 0; i < 9; ++i)
        f.advance();
    require(f.metrics()[MacCoarse] > 0, "No initial coarse region");
    f.advance(true);
    require(!f.metrics()[MacCoarse] && f.metrics()[MacPromotions] > 0 && !f.pool()[BrickInvalid],
            "Predictive solid margin failed to promote");
    std::cout << "{\"case\":\"mac-moving-margin\",\"pass\":true}\n";
}
void oddGridCase() {
    auto c = config();
    c.nx = 13;
    c.ny = 11;
    c.nz = 9;
    c.brickCapacity = 36;
    c.iterations = 120;
    Fixture f(c);
    for (int i = 0; i < 9; ++i)
        f.advance();
    std::cout << "{\"case\":\"mac-partial-bricks-even-parity\",";
    auditMixed(f);
    std::cout << ",\"pass\":true}\n";
}
void invalidCase() {
    uint32_t rejected = 0;
    auto a = config(), b = a, c = a;
    a.iterations = 0;
    b.predictionSeconds = NAN;
    c.nx = 0;
    for (auto cfg : {a, b, c}) {
        try {
            auto m = createMac(cfg);
            destroyMac(m);
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    Fixture f(config());
    auto frame = f.frame;
    frame.minimumCell.w = 0;
    try {
        enqueueMac(f.mac, f.stream, f.buffers, &frame);
    } catch (const std::exception &) {
        ++rejected;
    }
    auto previous = f.buffers[Pressure1];
    f.buffers[Pressure1] = f.buffers[Pressure0];
    try {
        enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    } catch (const std::exception &) {
        ++rejected;
    }
    f.buffers[Pressure1] = previous;
    require(rejected == 5, "Invalid mixed configuration or aliased pressure accepted");
    std::cout << "{\"case\":\"mac-invalid-config\",\"pass\":true}\n";
}
__global__ void policyInput(Frame f, lab::cuda_fluid::detail::Fields d, float3 velocity,
                            uint32_t population) {
    using namespace lab::cuda_fluid::detail;
    const uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id < f.grid.w)
        d.counts[id] = d.cells[id].z == 1 ? population : 0;
    int axis;
    int3 p;
    if (id < 3 * faceStride(f) && faceCoordinate(id, f, axis, p))
        d.faces[id].x = component(velocity, axis);
}
std::vector<RefinementState> seedPolicy(Fixture &f, float3 velocity) {
    auto v = macView(f.mac);
    std::vector<RefinementState> records(v.coarseCount);
    std::vector<uint32_t> history(v.coarseCount, 0x801);
    for (auto &r : records) {
        r.vx = velocity.x;
        r.vy = velocity.y;
        r.vz = velocity.z;
        r.liquidFraction = 1;
        r.quietSeconds = 1;
        r.flags = RefinementValid;
        r.history = 0x801;
    }
    check(cudaMemcpy(v.history, history.data(), history.size() * 4, cudaMemcpyHostToDevice));
    check(cudaMemcpy(v.previousRefinement, records.data(), records.size() * sizeof(RefinementState),
                     cudaMemcpyHostToDevice));
    return records;
}
std::vector<RefinementState> policyStep(Fixture &f, float3 velocity, uint32_t population = 8,
                                        bool graph = false) {
    // Manufactured policy inputs, not a time-integrated fluid experiment. The
    // existing numerical cases separately audit actual mixed projection fluxes.
    beginMacFrame(f.mac, f.stream);
    f.init();
    policyInput<<<(f.faces + 127) / 128, 128, 0, f.stream>>>(
        f.frame, lab::cuda_fluid::detail::fields(f.buffers), velocity, population);
    enqueueGrid(f.stream, f.buffers, &f.frame, GridStage::Divergence);
    if (graph) {
        if (!f.executable) {
            check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
            enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
            check(cudaStreamEndCapture(f.stream, &f.graph));
            check(cudaGraphInstantiate(&f.executable, f.graph, 0));
        }
        check(cudaGraphLaunch(f.executable, f.stream));
    } else
        enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    check(cudaStreamSynchronize(f.stream));
    require(!f.pool()[BrickInvalid], "Policy fixture rejected its projection");
    auto v = macView(f.mac);
    return read<RefinementState>(v.previousRefinement, v.coarseCount);
}
uint32_t policyId(uint32_t x, uint32_t y = 3, uint32_t z = 3) {
    return (z * 6 + y) * 6 + x;
}
void refinementSignals() {
    {
        Fixture f(config());
        f.empty = true;
        f.frame.gravityDt.y = -9.81f;
        auto records = seedPolicy(f, make_float3(0, 0, 0));
        for (auto &r : records)
            r.liquidFraction = 0;
        check(cudaMemcpy(macView(f.mac).previousRefinement, records.data(),
                         records.size() * sizeof(RefinementState), cudaMemcpyHostToDevice));
        const auto result = policyStep(f, make_float3(0, 0, 0), 0);
        for (const auto &r : result)
            require(r.velocityError == 0 && r.fractionError == 0 && r.importance == 0 && r.wakeSeconds == 0 &&
                        !(r.history & 1),
                    "Empty air generated liquid refinement error or became a coarse liquid cell");
        const auto m = f.metrics();
        require(!m[MacWakeRefinements] && !m[MacTemporalRefinements] && !m[MacPaddedRefinements],
                "Empty air polluted refinement telemetry");
        std::cout << "{\"case\":\"refinement-empty-air\",\"pass\":true}\n";
    }
    {
        auto c = config();
        c.multigrid = true;
        Fixture f(c, true);
        f.frame.gravityDt.y = -9.81f;
        for (uint32_t i = 0; i < 24; ++i) {
            f.advance();
            require(!f.pool()[BrickInvalid], "Hydrostatic refinement rejected its converged pressure field");
        }
        require(f.metrics()[MacCoarse] > 0, "Gravity was counted twice and permanently disabled coarsening");
        std::cout << "{\"case\":\"refinement-hydrostatic-coarsening\",\"pass\":true}\n";
    }
    {
        Fixture f(config(), true);
        seedPolicy(f, make_float3(1, 0, 0));
        f.frame.gravityDt.y = -.2f;
        const auto r =
            policyStep(f, make_float3(1, f.frame.gravityDt.y * f.frame.gravityDt.w, 0))[policyId(3)];
        require(r.velocityError < 1e-5 && r.importance < .01f && (r.history & 1),
                "Uniform translation / known gravity was mistaken for unresolved motion");
        std::cout << "{\"case\":\"refinement-comoving-gravity\",\"pass\":true}\n";
    }
    {
        Fixture f(config(), true);
        seedPolicy(f, make_float3(0, 0, 0));
        const auto r = policyStep(f, make_float3(.04f, .03f, 0))[policyId(3)];
        const double expected = std::hypot(.04, .03) / f.config.refinement.velocityScale;
        require(std::abs(r.velocityError - expected) < 1e-6 && !(r.history & 1) &&
                    f.metrics()[MacTemporalRefinements] && r.wakeSeconds == f.config.refinement.wakeSeconds,
                "Normalized temporal velocity error did not refine a locally uniform disturbance");
        std::cout << "{\"case\":\"refinement-velocity-error\",\"pass\":true}\n";
    }
    {
        Fixture f(config(), true);
        seedPolicy(f, make_float3(0, 0, 0));
        const auto r = policyStep(f, make_float3(0, 0, 0), 4)[policyId(3)];
        if (r.liquidFraction != .5f || r.fractionError != .5f || (r.history & 1) ||
            !f.metrics()[MacTemporalRefinements])
            std::cerr << "occupancy diagnostics: fraction=" << r.liquidFraction
                      << " error=" << r.fractionError << " history=" << r.history
                      << " refinements=" << f.metrics()[MacTemporalRefinements] << '\n';
        require(r.liquidFraction == .5f && r.fractionError == .5f && !(r.history & 1) &&
                    f.metrics()[MacTemporalRefinements],
                "Occupancy change was hidden by unchanged wet-cell labels");
        std::cout << "{\"case\":\"refinement-volume-error\",\"pass\":true}\n";
    }
    {
        Fixture f(config(), true);
        auto records = seedPolicy(f, make_float3(0, 0, 0));
        records[policyId(2)].vx = -1;
        check(cudaMemcpy(macView(f.mac).previousRefinement, records.data(),
                         records.size() * sizeof(RefinementState), cudaMemcpyHostToDevice));
        const auto r = policyStep(f, make_float3(0, 0, 0));
        require(!(r[policyId(2)].history & 1) && !(r[policyId(3)].history & 1) &&
                    (r[policyId(4)].history & 1) && r[policyId(3)].importance == 0 &&
                    f.metrics()[MacPaddedRefinements],
                "Refinement padding missing or recursively flooded the domain");
        std::cout << "{\"case\":\"refinement-single-layer-padding\",\"pass\":true}\n";
    }
}
void refinementAdvection(bool graph) {
    Fixture f(config(), true);
    f.frame.minimumCell.w = .01f;
    const float velocity = 2 * f.frame.minimumCell.w / f.frame.gravityDt.w;
    auto records = seedPolicy(f, make_float3(velocity, 0, 0));
    records[policyId(2)].wakeSeconds = f.config.refinement.wakeSeconds;
    check(cudaMemcpy(macView(f.mac).previousRefinement, records.data(),
                     records.size() * sizeof(RefinementState), cudaMemcpyHostToDevice));
    const auto r = policyStep(f, make_float3(velocity, 0, 0), 8, graph);
    require(std::abs(r[policyId(3)].wakeSeconds - (f.config.refinement.wakeSeconds - f.frame.gravityDt.w)) <
                    1e-5 &&
                r[policyId(2)].wakeSeconds < 1e-5 && r[policyId(4)].wakeSeconds < 1e-5 &&
                !(r[policyId(3)].history & 1) && f.metrics()[MacWakeRefinements],
            "Refinement wake did not advect one cell with the manufactured uniform velocity");
    if (graph) {
        const auto second = policyStep(f, make_float3(velocity, 0, 0), 8, true);
        require(std::abs(second[policyId(4)].wakeSeconds -
                         (f.config.refinement.wakeSeconds - 2 * f.frame.gravityDt.w)) < 1e-5,
                "Captured replay read a frozen history generation");
    }
    std::cout << "{\"case\":\"refinement-advected-wake" << (graph ? "-graph" : "") << "\",\"pass\":true}\n";
}
void refinementDwell() {
    const std::vector<std::vector<float>> partitions = {
        std::vector<float>(4, .01f), std::vector<float>(8, .005f), {.01f, .005f, .005f, .01f, .01f}};
    for (const auto &timesteps : partitions) {
        auto c = config();
        c.refinement.quietSeconds = .04f;
        Fixture f(c, true);
        double elapsed = 0;
        for (size_t i = 0; i < timesteps.size(); ++i) {
            f.frame.gravityDt.w = timesteps[i];
            elapsed += double(timesteps[i]);
            const auto r = policyStep(f, make_float3(0, 0, 0))[policyId(3)];
            require(r.quietSeconds == elapsed && bool(r.history & 1) == (i + 1 == timesteps.size()),
                    "Quiet-time hysteresis depends on substep count instead of physical time");
        }
    }
    std::cout << "{\"case\":\"refinement-physical-dwell-time\",\"pass\":true}\n";
}
void refinementInvalid() {
    uint32_t rejected = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        auto c = config();
        if (i == 0)
            c.refinement.velocityScale = 0;
        if (i == 1)
            c.refinement.fractionError = NAN;
        if (i == 2)
            c.refinement.demoteThreshold = c.refinement.promoteThreshold;
        if (i == 3)
            c.refinement.wakeSeconds = -1;
        if (i == 4)
            c.refinement.quietSeconds = 11;
        try {
            auto *m = createMac(c);
            destroyMac(m);
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    require(rejected == 5, "Invalid refinement policy accepted");
    std::cout << "{\"case\":\"refinement-invalid-policy\",\"pass\":true}\n";
}
void pressureStress(bool highPressure) {
    auto c = config();
    c.multigrid = true;
    if (highPressure) {
        // This manufactured 100-g RHS audits the mixed pressure operator, not
        // refinement policy. Keep coarse/fine junctions despite its deliberate
        // velocity discontinuities; physical pressure/flux tolerances stay exact.
        c.refinement.velocityScale = c.refinement.velocityDifference = 1e5f;
    }
    if (!highPressure) {
        c.nx = 24;
        c.ny = 20;
        c.nz = 18;
        c.brickCapacity = 150;
        c.changesPerFrame = 300;
    }
    Fixture f(c);
    f.barrier = !highPressure;
    if (highPressure)
        f.frame.gravityDt.y = -981;
    for (int i = 0; i < 9; ++i)
        f.advance();
    if (!highPressure)
        require(macPressureView(f.mac).levelCount >= 2, "No multilevel hierarchy exercised");
    std::cout << "{\"case\":\"mgpcg-" << (highPressure ? "high-pressure" : "multilevel-moving-wall") << "\",";
    auditMixed(f);
    std::cout << ",\"pass\":true}\n";
}
void manufacturedPressure() {
    auto c = config();
    c.multigrid = true;
    // Reinstalling an affine pressure gradient each step is not a trajectory.
    // Hold the test's mixed topology; dedicated policy fixtures exercise error
    // promotion with production thresholds instead.
    c.refinement.velocityScale = c.refinement.velocityDifference = 1e5f;
    Fixture f(c, true);
    f.manufactured = true;
    for (int i = 0; i < 9; ++i)
        f.advance();
    std::cout << "{\"case\":\"mgpcg-manufactured-affine\",";
    auditMixed(f);
    std::cout << ",\"pass\":true}\n";
}
void quantizedFluxRejection() {
    auto c = config(true);
    c.multigrid = true;
    Fixture f(c, true);
    beginMacFrame(f.mac, f.stream);
    f.init();
    addLargeVortex<<<(f.faces + 127) / 128, 128, 0, f.stream>>>(f.frame,
                                                                lab::cuda_fluid::detail::fields(f.buffers));
    enqueueGrid(f.stream, f.buffers, &f.frame, GridStage::Divergence);
    f.pi = enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    check(cudaStreamSynchronize(f.stream));
    const auto pv = macPressureView(f.mac);
    const auto status = read<uint32_t>(pv.counts, PressureCounterCount);
    const auto scalar = read<double>(pv.scalars, 8);
    require(status[PressureConverged] && !status[PressureCapped] && scalar[2] <= .00005,
            "Quantized-flux fixture did not converge before FP32 publication");
    require(scalar[7] > .0001 && f.pool()[BrickInvalid] && !f.pool()[BrickVersion],
            "Invalid FP32 flux was not rejected before topology publication");
    std::cout << "{\"case\":\"mgpcg-quantized-flux-rejected\",\"solverDivergence\":" << scalar[2]
              << ",\"faceDivergence\":" << scalar[7] << ",\"pass\":true}\n";
}
void finePressure(uint32_t mode) {
    auto c = config(mode == 0);
    c.multigrid = true;
    if (mode)
        c.brickCapacity = 1;
    if (mode == 1) {
        c.nx = 13;
        c.ny = 11;
        c.nz = 9;
    }
    Fixture f(c, mode == 2);
    if (mode == 1) {
        f.barrier = true;
        f.frame.material.y = .072f;
    }
    if (mode == 2)
        f.frame.gravityDt.y = -981;
    for (int i = 0; i < 9; ++i) {
        f.advance();
        auto p = f.pool();
        auto counts = read<uint32_t>(macPressureView(f.mac).counts, PressureCounterCount);
        require(!p[BrickInvalid] && counts[PressureConverged] && !counts[PressureTotalCapped],
                "Fine/residency fallback failed convergence");
        if (mode)
            require(!p[BrickVersion] && p[BrickOverflows] && f.metrics()[MacFallbacks] == uint32_t(i + 1),
                    "Fallback published incomplete page residency");
    }
    if (mode) {
        auto residentConfig = c;
        residentConfig.forcedFine = true;
        residentConfig.brickCapacity = ((c.nx + 3) / 4) * ((c.ny + 3) / 4) * ((c.nz + 3) / 4);
        Fixture resident(residentConfig, f.closed);
        resident.frame = f.frame;
        resident.barrier = f.barrier;
        resident.advance();
        const auto expected = resident.field(Faces), actual = f.field(Faces);
        for (uint32_t i = 0; i < f.faces; ++i)
            require(std::abs(actual[i].x - expected[i].x) < 2e-8f && actual[i].y == expected[i].y &&
                        actual[i].z == expected[i].z && actual[i].w == expected[i].w,
                    "Converged fallback differs from the full resident fine operator");
    }
    std::cout << "{\"case\":\"mgpcg-"
              << (mode == 0   ? "forced-fine"
                  : mode == 1 ? "capacity-free-surface"
                              : "capacity-closed-high-pressure")
              << "\",";
    auditMixed(f, false);
    std::cout << ",\"pass\":true}\n";
}
void budgetPressure() {
    auto c = config();
    c.multigrid = true;
    c.changesPerFrame = 2;
    Fixture f(c);
    f.frame.material.y = .072f;
    check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
    beginMacFrame(f.mac, f.stream);
    f.init();
    f.pi = enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    check(cudaStreamEndCapture(f.stream, &f.graph));
    check(cudaGraphInstantiate(&f.executable, f.graph, 0));
    for (int i = 0; i < 14; ++i) {
        check(cudaGraphLaunch(f.executable, f.stream));
        check(cudaStreamSynchronize(f.stream));
        const auto p = f.pool();
        const auto counts = read<uint32_t>(macPressureView(f.mac).counts, PressureCounterCount);
        require(p[BrickFrameChanges] <= 2 && !p[BrickInvalid] && counts[PressureConverged] &&
                    !counts[PressureTotalCapped],
                "Graph budget transition exceeded page budget or lost convergence");
        for (auto cell : f.field(Cells))
            require(cell.z != 1 || std::abs(cell.y) < .000101f,
                    "Graph fallback published divergent velocity");
    }
    require(f.metrics()[MacFallbacks] > 0 && f.pool()[BrickVersion],
            "No fallback-to-resident transition exercised");
    std::cout << "{\"case\":\"mgpcg-budget-graph-recovery\",";
    auditMixed(f);
    std::cout << ",\"pass\":true}\n";
}
void publishedFallbackPressure() {
    auto c = config();
    c.multigrid = true;
    c.brickCapacity = 9;
    Fixture f(c);
    f.wetHeight = 4;
    f.frame.material.y = .072f;
    f.advance();
    const auto mv = macView(f.mac);
    const auto initial = f.pool();
    require(initial[BrickVersion] && !initial[BrickInvalid], "No initial resident field for fallback test");
    const uint32_t front = initial[BrickFront];
    const auto saved =
        read<unsigned char>(mv.pool.fields[front], mv.pool.capacity * 64 * mv.pool.bytesPerCell);
    const auto pages = read<uint32_t>(mv.pool.pages[front], mv.pool.bricks);
    const auto active = read<uint32_t>(mv.pool.active[front], mv.pool.bricks);
    const auto map = read<uint32_t>(mv.map[front], f.frame.grid.w);
    f.wetHeight = 10;
    for (int i = 0; i < 9; ++i) {
        f.advance();
        const auto p = f.pool();
        const auto counts = read<uint32_t>(macPressureView(f.mac).counts, PressureCounterCount);
        require(!p[BrickInvalid] && !p[BrickReady] && p[BrickFront] == front &&
                    p[BrickVersion] == initial[BrickVersion] && counts[PressureConverged] &&
                    !counts[PressureTotalCapped],
                "Missing pages invalidated the converged fine fallback or replaced published topology");
        require(saved == read<unsigned char>(mv.pool.fields[front], saved.size()) &&
                    pages == read<uint32_t>(mv.pool.pages[front], mv.pool.bricks) &&
                    active == read<uint32_t>(mv.pool.active[front], mv.pool.bricks) &&
                    map == read<uint32_t>(mv.map[front], f.frame.grid.w),
                "Fallback changed the complete previous published field/ownership");
    }
    std::cout << "{\"case\":\"mgpcg-published-front-capacity-recovery\",";
    auditMixed(f, false);
    f.wetHeight = 4;
    f.advance();
    const auto recovered = f.pool();
    const auto counts = read<uint32_t>(macPressureView(f.mac).counts, PressureCounterCount);
    require(!recovered[BrickInvalid] && recovered[BrickReady] &&
                recovered[BrickVersion] == initial[BrickVersion] + 1 && counts[PressureConverged],
            "Pool did not recover from a valid fine fallback");
    for (auto cell : f.field(Cells))
        require(cell.z != 1 || std::abs(cell.y) < .000101f, "Recovered field is divergent");
    std::cout << ",\"pass\":true}\n";
}
void cappedPressure(bool fallback = false) {
    auto c = config();
    c.multigrid = true;
    c.cgIterations = 1;
    if (fallback)
        c.brickCapacity = 1;
    Fixture f(c);
    beginMacFrame(f.mac, f.stream);
    f.init();
    check(cudaStreamSynchronize(f.stream));
    const auto input = f.field(Faces);
    const auto pressureBefore = read<float>(f.buffers[Pressure0], f.frame.grid.w);
    f.pi = enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    check(cudaStreamSynchronize(f.stream));
    auto p = f.pool();
    const auto v = macPressureView(f.mac);
    auto counts = read<uint32_t>(v.counts, PressureCounterCount);
    require(p[BrickInvalid] && !p[BrickVersion] && counts[PressureCapped] &&
                counts[PressureTotalCapped] == 1 && !counts[PressureConverged] &&
                counts[PressureIterations] == 1,
            "Capped solve was silently accepted");
    const auto output = f.field(Faces);
    require(!std::memcmp(input.data(), output.data(), input.size() * sizeof(float4)) &&
                pressureBefore == read<float>(f.buffers[Pressure0], f.frame.grid.w) &&
                pressureBefore == read<float>(f.buffers[Pressure1], f.frame.grid.w),
            "Rejected resident/fallback solve changed velocities or published pressure");
    const auto mv = macView(f.mac);
    const auto history = read<uint32_t>(mv.history, mv.coarseCount);
    const auto metrics = f.metrics();
    // Later already-queued substeps, including a history-reset request, cannot
    // erase the first failure, retry publication or keep allocating topology.
    check(cudaStreamBeginCapture(f.stream, cudaStreamCaptureModeThreadLocal));
    beginMacFrame(f.mac, f.stream, true);
    enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    check(cudaStreamEndCapture(f.stream, &f.graph));
    check(cudaGraphInstantiate(&f.executable, f.graph, 0));
    for (int i = 0; i < 4; ++i)
        check(cudaGraphLaunch(f.executable, f.stream));
    check(cudaStreamSynchronize(f.stream));
    const auto after = f.field(Faces);
    require(f.pool() == p && f.metrics() == metrics &&
                counts == read<uint32_t>(v.counts, PressureCounterCount) &&
                history == read<uint32_t>(mv.history, mv.coarseCount) &&
                !std::memcmp(input.data(), after.data(), input.size() * sizeof(float4)),
            "Queued graph replay erased a failed solve or continued its topology/velocity updates");
    std::cout << "{\"case\":\"mgpcg-" << (fallback ? "fallback-capped-rejected" : "capped-rejected")
              << "\",\"pass\":true}\n";
}
void emptyPressure() {
    auto c = config();
    c.multigrid = true;
    Fixture f(c);
    f.empty = true;
    f.advance();
    auto p = f.pool();
    auto counts = read<uint32_t>(macPressureView(f.mac).counts, PressureCounterCount);
    require(!p[BrickInvalid] && p[BrickVersion] && !f.metrics()[MacLeaves] && counts[PressureConverged] &&
                !counts[PressureIterations],
            "Empty system failed zero-work convergence");
    std::cout << "{\"case\":\"mgpcg-empty\",\"pass\":true}\n";
}
void rejectionCase() {
    Fixture f(config());
    for (int i = 0; i < 9; ++i)
        f.advance();
    const auto v = macView(f.mac);
    const auto p = v.pool;
    const auto previous = f.pool();
    const auto previousPolicy =
        read<unsigned char>(v.previousRefinement, v.coarseCount * sizeof(RefinementState));
    const auto previousHistory = read<uint32_t>(v.history, v.coarseCount);
    const auto frontFields =
        read<unsigned char>(p.fields[previous[BrickFront]], p.capacity * 64 * p.bytesPerCell);
    beginMacFrame(f.mac, f.stream);
    f.init();
    check(cudaStreamSynchronize(f.stream));
    const auto inputFaces = f.field(Faces);
    // Nonfinite capillary data must reject the mixed candidate after restriction,
    // not run the fine stencil from overwritten scratch or publish NaN pressure.
    auto material = f.field(Material);
    for (auto &x : material)
        x.y = NAN;
    f.frame.material.y = .072f;
    check(cudaMemcpyAsync(f.buffers[Material], material.data(), material.size() * sizeof(float4),
                          cudaMemcpyHostToDevice, f.stream));
    enqueueMac(f.mac, f.stream, f.buffers, &f.frame);
    check(cudaStreamSynchronize(f.stream));
    const auto current = f.pool();
    const auto output = f.field(Faces);
    require(current[BrickInvalid] && current[BrickVersion] == previous[BrickVersion] &&
                current[BrickFront] == previous[BrickFront],
            "Malformed candidate published");
    require(!std::memcmp(inputFaces.data(), output.data(), output.size() * sizeof(float4)),
            "Rejected mixed solve still modified velocities");
    require(frontFields == read<unsigned char>(p.fields[current[BrickFront]], frontFields.size()),
            "Rejected candidate overwrote published rows");
    require(previousPolicy == read<unsigned char>(v.previousRefinement, previousPolicy.size()) &&
                previousHistory == read<uint32_t>(v.history, v.coarseCount),
            "Rejected projection advanced published refinement history");
    std::cout << "{\"case\":\"mac-invalid-operator\",\"pass\":true}\n";
}
} // namespace
int main() {
    try {
        int devices = 0;
        check(cudaGetDeviceCount(&devices));
        require(devices > 0, "No CUDA device");
        invalidCase();
        fineCase(false);
        fineCase(true);
        budgetCase();
        mixedCase(true, false);
        mixedCase(false, false);
        mixedCase(false, true);
        movingCase();
        oddGridCase();
        rejectionCase();
        mixedCase(true, false, true);
        mixedCase(false, false, true);
        mixedCase(false, true, true);
        pressureStress(false);
        pressureStress(true);
        cappedPressure();
        emptyPressure();
        manufacturedPressure();
        quantizedFluxRejection();
        finePressure(0);
        finePressure(1);
        finePressure(2);
        budgetPressure();
        publishedFallbackPressure();
        cappedPressure(true);
        refinementSignals();
        refinementAdvection(false);
        refinementAdvection(true);
        refinementDwell();
        refinementInvalid();
        pressureHistory(false);
        pressureHistory(true);
        refinementGuardEquivalence();
        std::cout << "PASS CUDA mixed MAC: 38 cases; numerical fixtures, not realtime or game integration "
                     "acceptance\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL CUDA mixed MAC: " << e.what() << '\n';
        return 1;
    }
}
