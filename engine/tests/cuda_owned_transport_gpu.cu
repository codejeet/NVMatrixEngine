#include "../src/fluid/cuda/fluid_cuda_owned_transport.h"
#include "../src/fluid/cuda/fluid_cuda_device.cuh"
#include "../src/fluid/cuda/fluid_cuda_grid.h"
#include "../src/fluid/cuda/fluid_cuda_mac.h"
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
namespace {
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
void require(bool value, const char *why) {
    if (!value)
        throw std::runtime_error(why);
}
struct BufferStorage {
    void *p = nullptr;
    size_t bytes;
    explicit BufferStorage(size_t size) : bytes(size) {
        check(cudaMalloc(&p, bytes + 64));
        std::vector<unsigned char> initial(bytes + 64);
        std::fill(initial.begin() + bytes, initial.end(), 0xcd);
        check(cudaMemcpy(p, initial.data(), initial.size(), cudaMemcpyHostToDevice));
    }
    ~BufferStorage() {
        cudaFree(p);
    }
    template <class T> void upload(const std::vector<T> &v) {
        require(v.size() * sizeof(T) == bytes, "Wrong fixture upload size");
        check(cudaMemcpy(p, v.data(), bytes, cudaMemcpyHostToDevice));
    }
    template <class T> std::vector<T> read() const {
        std::vector<T> v(bytes / sizeof(T));
        check(cudaMemcpy(v.data(), p, bytes, cudaMemcpyDeviceToHost));
        return v;
    }
    void guard() const {
        std::array<unsigned char, 64> tail;
        check(cudaMemcpy(tail.data(), static_cast<const char *>(p) + bytes, 64, cudaMemcpyDeviceToHost));
        require(std::all_of(tail.begin(), tail.end(), [](auto x) { return x == 0xcd; }),
                "Transport buffer tail changed");
    }
};
struct Stream {
    cudaStream_t p = nullptr;
    Stream() {
        check(cudaStreamCreateWithFlags(&p, cudaStreamNonBlocking));
    }
    ~Stream() {
        cudaStreamSynchronize(p);
        cudaStreamDestroy(p);
    }
};
using Q = std::array<double, 4>;
struct Edge {
    uint32_t id, left, right;
    double rate;
};
struct Model {
    OwnedTransportConfig cfg;
    std::vector<Q> source;
    std::vector<std::array<double, 2>> capacity;
    std::vector<double> rates;
    float dt = .25f;
    Model(uint32_t x, uint32_t y = 1, uint32_t z = 1)
        : cfg{x, y, z}, source(x * y * z), capacity(x * y * z, {1, 1}),
          rates(3 * (x + 1) * (y + 1) * (z + 1)) {
        for (uint32_t i = 0; i < source.size(); ++i) {
            double v = .173 + double(i % 7) * .051234567891;
            source[i] = {v * (.2 + .03 * (i % 3)), v * (-.4 + .02 * (i % 5)), v * .123456789123, v};
        }
    }
    uint32_t face(uint32_t x, uint32_t y, uint32_t z, uint32_t a) const {
        return a * (cfg.nx + 1) * (cfg.ny + 1) * (cfg.nz + 1) + (z * (cfg.ny + 1) + y) * (cfg.nx + 1) + x;
    }
    std::vector<Edge> edges() const {
        std::vector<Edge> e;
        for (uint32_t a = 0; a < 3; ++a)
            for (uint32_t z = 0; z < cfg.nz; ++z)
                for (uint32_t y = 0; y < cfg.ny; ++y)
                    for (uint32_t x = 0; x < cfg.nx; ++x) {
                        uint32_t p[]{x, y, z}, extent[]{cfg.nx, cfg.ny, cfg.nz},
                            step[]{1, cfg.nx, cfg.nx * cfg.ny};
                        if (p[a] + 1 >= extent[a])
                            continue;
                        uint32_t left = (z * cfg.ny + y) * cfg.nx + x;
                        ++p[a];
                        uint32_t id = face(p[0], p[1], p[2], a);
                        e.push_back({id, left, left + step[a], rates[id]});
                    }
        return e;
    }
    std::vector<Q> oracle() const {
        const uint32_t n = uint32_t(source.size()), stride = n + 4;
        std::vector<double> a(size_t(n) * stride);
        for (uint32_t i = 0; i < n; ++i) {
            a[i * stride + i] = capacity[i][0];
            for (uint32_t k = 0; k < 4; ++k)
                a[i * stride + n + k] = source[i][k];
        }
        // Independent face-column assembly, followed by pivoted dense elimination;
        // neither reproduces the GPU cell gather nor its Jacobi iteration.
        for (auto e : edges()) {
            uint32_t donor = e.rate >= 0 ? e.left : e.right, receiver = e.rate >= 0 ? e.right : e.left;
            double q = std::abs(double(dt) * e.rate);
            a[donor * stride + donor] += q;
            a[receiver * stride + donor] -= q;
        }
        for (uint32_t i = 0; i < n; ++i) {
            bool zero = true;
            for (uint32_t j = 0; j < stride; ++j)
                zero &= a[i * stride + j] == 0;
            if (zero)
                a[i * stride + i] = 1;
        }
        for (uint32_t k = 0; k < n; ++k) {
            uint32_t pivot = k;
            for (uint32_t i = k + 1; i < n; ++i)
                if (std::abs(a[i * stride + k]) > std::abs(a[pivot * stride + k]))
                    pivot = i;
            require(std::abs(a[pivot * stride + k]) > 1e-18, "Singular independent transport oracle");
            for (uint32_t j = k; j < stride; ++j)
                std::swap(a[k * stride + j], a[pivot * stride + j]);
            const double diagonal = a[k * stride + k];
            for (uint32_t j = k; j < stride; ++j)
                a[k * stride + j] /= diagonal;
            for (uint32_t i = 0; i < n; ++i)
                if (i != k) {
                    double scale = a[i * stride + k];
                    for (uint32_t j = k; j < stride; ++j)
                        a[i * stride + j] -= scale * a[k * stride + j];
                }
        }
        std::vector<Q> result(n);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t k = 0; k < 4; ++k)
                result[i][k] = a[i * stride + n + k];
        return result;
    }
};
struct Fixture {
    Model m;
    BufferStorage source, capacity, rates, output, transfers, failure;
    std::unique_ptr<OwnedTransport, decltype(&destroyOwnedTransport)> solver;
    Stream stream;
    explicit Fixture(Model model)
        : m(std::move(model)), source(m.source.size() * 32), capacity(m.capacity.size() * 16),
          rates(m.rates.size() * 8), output(source.bytes), transfers(m.rates.size() * 32), failure(4),
          solver(createOwnedTransport(m.cfg), destroyOwnedTransport) {
        upload();
    }
    void upload() {
        source.upload(m.source);
        capacity.upload(m.capacity);
        rates.upload(m.rates);
    }
    OwnedTransportInputs inputs() {
        return {source.p, capacity.p, rates.p, output.p, transfers.p, static_cast<uint32_t *>(failure.p)};
    }
    void run() {
        enqueueOwnedTransport(solver.get(), stream.p, inputs(), m.dt);
        check(cudaStreamSynchronize(stream.p));
    }
    OwnedTransportMetrics metrics() {
        OwnedTransportMetrics r;
        check(cudaMemcpy(&r, ownedTransportMetrics(solver.get()), sizeof(r), cudaMemcpyDeviceToHost));
        return r;
    }
    void guards() {
        for (auto b : {&source, &capacity, &rates, &output, &transfers, &failure})
            b->guard();
    }
    double audit() {
        guards();
        require(!failure.read<uint32_t>()[0], "Conservative transport rejected valid data");
        const auto metric = metrics();
        require(metric.converged && !metric.invalid && !metric.capped &&
                    metric.residual <= m.cfg.residualTolerance,
                "Conservative transport convergence diagnostic failed");
        require(source.read<Q>() == m.source, "Transport changed its source owner");
        const auto a = output.read<Q>(), flux = transfers.read<Q>(), expected = m.oracle();
        auto balance = m.source;
        std::vector<bool> internal(flux.size());
        double error = 0, initialEnergy = 0, finalEnergy = 0;
        Q initial{}, final{};
        for (auto e : m.edges()) {
            internal[e.id] = true;
            const uint32_t donor = e.rate >= 0 ? e.left : e.right;
            for (uint32_t k = 0; k < 4; ++k) {
                double wanted = double(m.dt) * e.rate * expected[donor][k];
                error = std::max(error, std::abs(flux[e.id][k] - wanted));
                balance[e.left][k] -= flux[e.id][k];
                balance[e.right][k] += flux[e.id][k];
            }
        }
        for (uint32_t i = 0; i < flux.size(); ++i)
            if (!internal[i])
                require(flux[i] == Q{}, "Boundary/padding exported flux");
        for (uint32_t i = 0; i < a.size(); ++i) {
            require(a[i][3] >= 0 && a[i][3] <= m.capacity[i][0] + 1e-12, "Unbounded published volume");
            for (uint32_t k = 0; k < 4; ++k) {
                require(std::isfinite(a[i][k]), "Nonfinite published quantity");
                error = std::max({error, std::abs(a[i][k] - m.capacity[i][0] * expected[i][k]),
                                  std::abs(a[i][k] - balance[i][k])});
                initial[k] += m.source[i][k];
                final[k] += a[i][k];
                if (k < 3) {
                    if (m.source[i][3] > 0)
                        initialEnergy += m.source[i][k] * m.source[i][k] / m.source[i][3];
                    if (a[i][3] > 0)
                        finalEnergy += a[i][k] * a[i][k] / a[i][3];
                }
            }
        }
        for (uint32_t k = 0; k < 4; ++k)
            require(std::abs(initial[k] - final[k]) < 2e-11, "Joint quantity not conserved");
        require(finalEnergy <= initialEnergy + 2e-11, "Transport injected kinetic energy");
        require(error < 2e-11, "CUDA transport disagrees with independent matrix/face balance");
        return error;
    }
    void rejected() {
        std::vector<Q> sentinel(output.bytes / 32, {.11, -.27, .39, .41});
        output.upload(sentinel);
        std::vector<Q> faces(transfers.bytes / 32, {.13, -.19, .23, .31});
        transfers.upload(faces);
        const auto original = source.read<unsigned char>();
        run();
        guards();
        require(failure.read<uint32_t>()[0] != 0, "Invalid transport was accepted");
        require(source.read<unsigned char>() == original && output.read<Q>() == sentinel &&
                    transfers.read<Q>() == faces,
                "Rejected transport published a partial owner/face flux");
    }
};
Model cycle(double rate) {
    Model m(3, 3);
    m.rates[m.face(1, 0, 0, 0)] = rate;
    m.rates[m.face(1, 1, 0, 1)] = rate;
    m.rates[m.face(1, 1, 0, 0)] = -rate;
    m.rates[m.face(0, 1, 0, 1)] = -rate;
    return m;
}
void standard(const char *name, Model m, bool graph = false) {
    Fixture f(std::move(m));
    double error = 0;
    if (graph) {
        cudaGraph_t g = nullptr;
        cudaGraphExec_t exec = nullptr;
        check(cudaStreamBeginCapture(f.stream.p, cudaStreamCaptureModeThreadLocal));
        enqueueOwnedTransport(f.solver.get(), f.stream.p, f.inputs(), f.m.dt);
        check(cudaStreamEndCapture(f.stream.p, &g));
        check(cudaGraphInstantiate(&exec, g, 0));
        cudaGraphDestroy(g);
        try {
            for (uint32_t i = 0; i < 4; ++i) {
                f.upload();
                check(cudaGraphLaunch(exec, f.stream.p));
                check(cudaStreamSynchronize(f.stream.p));
                error = std::max(error, f.audit());
                f.m.source = f.output.read<Q>();
            }
        } catch (...) {
            cudaGraphExecDestroy(exec);
            throw;
        }
        cudaGraphExecDestroy(exec);
    } else {
        f.run();
        error = f.audit();
    }
    auto s = f.metrics();
    std::cout << "{\"case\":\"" << name << "\",\"maxEquationError\":" << error
              << ",\"iterations\":" << s.iterations << ",\"pass\":true}\n";
}
void invalidInput() {
    for (uint32_t mode = 0; mode < 8; ++mode) {
        Model m(2);
        if (mode == 0)
            m.source[0][3] = -1;
        if (mode == 1) {
            m.source[0][3] = 0;
            m.source[0][0] = 1;
        }
        if (mode == 2)
            m.rates[m.face(1, 0, 0, 0)] = std::numeric_limits<double>::quiet_NaN();
        if (mode == 3)
            m.rates[m.face(0, 0, 0, 0)] = 1;
        if (mode == 4)
            m.capacity[0][0] = -1;
        if (mode == 5)
            m.source[0][3] = 2;
        if (mode == 6)
            m.capacity[0][0] = 0;
        if (mode == 7) {
            m.source[0] = {1e-51, -2e-51, 0, 1e-50};
            m.capacity[0][1] = 0;
        }
        Fixture f(m);
        f.rejected();
    }
    std::cout << "{\"case\":\"owned-transport-invalid-input\",\"pass\":true}\n";
}
void rejectCapacity() {
    Model m(2);
    m.source = {Q{.2, 0, 0, 1}, Q{.2, 0, 0, 1}};
    m.rates[m.face(1, 0, 0, 0)] = 1;
    Fixture f(m);
    f.rejected();
    require(f.metrics().maximumExcess > .1, "Overflow rejection did not measure receiver excess");
    std::cout << "{\"case\":\"owned-transport-receiver-overfill\",\"pass\":true}\n";
}
void rejectCap() {
    auto m = cycle(256);
    Fixture f(m);
    f.rejected();
    require(f.metrics().capped && f.metrics().iterations == 256, "Iteration cap was not enforced");
    f.rejected();
    require(f.metrics().iterations == 0, "Sticky failure started another transport solve");
    std::cout << "{\"case\":\"owned-transport-cap-and-sticky-failure\",\"pass\":true}\n";
}
void tinyOwnership() {
    Model m(2);
    m.source = {Q{.2e-30, -.4e-30, .1e-60, 1e-30}, Q{}};
    m.rates[m.face(1, 0, 0, 0)] = .5;
    Fixture f(m);
    f.run();
    f.audit();
    const auto output = f.output.read<Q>(), expected = m.oracle();
    double relative = 0;
    for (uint32_t i = 0; i < output.size(); ++i)
        for (uint32_t a = 0; a < 4; ++a) {
            const double wanted = m.capacity[i][0] * expected[i][a];
            require(wanted != 0 && output[i][a] != 0,
                    "Nonzero grid-owned tail was erased by the stopping scale");
            relative = std::max(relative, std::abs((output[i][a] - wanted) / wanted));
        }
    require(relative < 3e-12, "Tiny grid-owned quantity lost relative precision");
    std::cout << "{\"case\":\"owned-transport-tiny-ownership\",\"relativeError\":" << relative
              << ",\"pass\":true}\n";
}
void invalidApi() {
    uint32_t count = 0;
    for (uint32_t i = 0; i < 5; ++i) {
        OwnedTransportConfig c{2, 2, 2};
        if (i == 0)
            c.nx = 0;
        if (i == 1)
            c.nz = 1048576;
        if (i == 2)
            c.maxIterations = 3;
        if (i == 3)
            c.residualTolerance = 1e-8;
        if (i == 4)
            c.residualTolerance = std::numeric_limits<double>::quiet_NaN();
        try {
            auto p = createOwnedTransport(c);
            destroyOwnedTransport(p);
        } catch (const std::exception &) {
            ++count;
        }
    }
    Fixture f(Model(2));
    for (uint32_t i = 0; i < 3; ++i) {
        auto v = f.inputs();
        if (i == 0)
            v.output = const_cast<void *>(v.quantity);
        if (i == 1)
            v.failure = nullptr;
        try {
            enqueueOwnedTransport(f.solver.get(), f.stream.p, v, i == 2 ? 0 : f.m.dt);
        } catch (const std::exception &) {
            ++count;
        }
    }
    require(count == 8, "Malformed transport API accepted");
    std::cout << "{\"case\":\"owned-transport-invalid-api\",\"pass\":true}\n";
}
void pressureRates() {
    using namespace lab::cuda_fluid::detail;
    Config c{};
    c.nx = 9;
    c.ny = 7;
    c.nz = 5;
    c.capacity = 1;
    Frame frame{};
    frame.grid = make_uint4(c.nx, c.ny, c.nz, c.nx * c.ny * c.nz);
    frame.counts.x = 1;
    frame.minimumCell.w = .25f;
    frame.maximumRadius = make_float4(2.25f, 1.75f, 1.25f, .02f);
    frame.gravityDt.w = .01f;
    frame.solver = make_float4(1000, .95f, 0, .125f);
    std::array<std::unique_ptr<BufferStorage>, BufferCount> memory;
    void *buffers[BufferCount]{};
    for (uint32_t i = 0; i < BufferCount; ++i) {
        memory[i] = std::make_unique<BufferStorage>(bufferBytes(c, Buffer(i)));
        buffers[i] = memory[i]->p;
    }
    memory[Counts]->upload(std::vector<uint32_t>(frame.grid.w, 1));
    memory[Solid]->upload(std::vector<float4>(frame.grid.w, make_float4(100, 0, 0, 0)));
    std::vector<float4> velocities(bufferBytes(c, Faces) / 16, make_float4(0, 0, 1, 0));
    const uint32_t stride = (c.nx + 1) * (c.ny + 1) * (c.nz + 1);
    for (uint32_t i = 0; i < velocities.size(); ++i) {
        uint32_t a = i / stride, k = i % stride;
        uint32_t p[]{k % (c.nx + 1), (k / (c.nx + 1)) % (c.ny + 1), k / ((c.nx + 1) * (c.ny + 1))};
        uint32_t dims[]{c.nx, c.ny, c.nz};
        bool live = true;
        for (uint32_t j = 0; j < 3; ++j)
            live &= p[j] < dims[j] + (a == j);
        if (live && p[a] > 0 && p[a] < dims[a])
            velocities[i].x = .013f * float(std::sin(double(i) * .37));
    }
    memory[Faces]->upload(velocities);
    Model m((c.nx + 1) / 2, (c.ny + 1) / 2, (c.nz + 1) / 2);
    m.dt = frame.gravityDt.w;
    for (uint32_t i = 0; i < m.source.size(); ++i) {
        uint32_t x = i % m.cfg.nx, y = (i / m.cfg.nx) % m.cfg.ny, z = i / (m.cfg.nx * m.cfg.ny);
        double volume = std::min(2u, c.nx - 2 * x) * std::min(2u, c.ny - 2 * y) * std::min(2u, c.nz - 2 * z) *
                        std::pow(double(frame.minimumCell.w), 3);
        m.capacity[i] = {volume, volume};
        for (auto &q : m.source[i])
            q *= volume;
    }
    Fixture f(m);
    const uint32_t bricks = ((c.nx + 3) / 4) * ((c.ny + 3) / 4) * ((c.nz + 3) / 4);
    MacConfig mc{c.nx, c.ny, c.nz, 120, bricks, bricks, false, .15f, true, 32, false};
    std::unique_ptr<Mac, decltype(&destroyMac)> mac(createMac(mc), destroyMac);
    beginMacFrame(mac.get(), f.stream.p, true);
    enqueueGrid(f.stream.p, buffers, &frame, GridStage::Classify);
    enqueueGrid(f.stream.p, buffers, &frame, GridStage::Forces);
    enqueueGrid(f.stream.p, buffers, &frame, GridStage::Divergence);
    enqueueMac(mac.get(), f.stream.p, buffers, &frame);
    check(cudaStreamSynchronize(f.stream.p));
    uint32_t invalid;
    check(cudaMemcpy(&invalid, macView(mac.get()).pool.control + BrickInvalid, 4, cudaMemcpyDeviceToHost));
    require(!invalid, "Projected-rate pressure solve rejected");
    velocities = memory[Faces]->read<float4>();
    enqueueOwnedTransportRates(f.solver.get(), f.stream.p, &frame, buffers[Faces], f.rates.p,
                               static_cast<uint32_t *>(f.failure.p));
    check(cudaStreamSynchronize(f.stream.p));
    f.m.rates = f.rates.read<double>();
    std::vector<double> expected(f.m.rates.size());
    // Assemble coarse faces by walking fine faces, independent of the GPU's
    // per-coarse-face four-child gather, including odd-grid edge areas.
    for (uint32_t i = 0; i < velocities.size(); ++i) {
        uint32_t a = i / stride, k = i % stride;
        uint32_t p[]{k % (c.nx + 1), (k / (c.nx + 1)) % (c.ny + 1), k / ((c.nx + 1) * (c.ny + 1))},
            dims[]{c.nx, c.ny, c.nz};
        bool live = true;
        for (uint32_t j = 0; j < 3; ++j)
            live &= p[j] < dims[j] + (a == j);
        if (!live || p[a] == 0 || p[a] >= dims[a] || (p[a] & 1))
            continue;
        expected[f.m.face(p[0] / 2, p[1] / 2, p[2] / 2, a)] +=
            double(velocities[i].x) * double(frame.minimumCell.w) * double(frame.minimumCell.w);
    }
    double magnitude = 0;
    for (uint32_t i = 0; i < expected.size(); ++i) {
        require(std::abs(expected[i] - f.m.rates[i]) < 1e-18,
                "Pressure flux restriction disagrees with face oracle");
        magnitude += std::abs(expected[i]);
    }
    require(magnitude > .0001, "Pressure-rate test has no flow");
    f.run();
    double error = f.audit();
    for (uint32_t invalidRate = 0; invalidRate < 8; ++invalidRate) {
        auto broken = velocities;
        if (invalidRate < 6) {
            const uint32_t axis = invalidRate / 2;
            uint32_t p[]{0, 0, 0}, dims[]{c.nx, c.ny, c.nz};
            p[axis] = (invalidRate & 1) ? dims[axis] : 0;
            broken[axis * stride + (p[2] * (c.ny + 1) + p[1]) * (c.nx + 1) + p[0]].x = .1f;
        } else {
            // Both retained and eliminated interior fine faces must be checked.
            broken[invalidRate == 6 ? 1 : 2].x = std::numeric_limits<float>::quiet_NaN();
        }
        memory[Faces]->upload(broken);
        f.failure.upload(std::vector<uint32_t>{0});
        enqueueOwnedTransportRates(f.solver.get(), f.stream.p, &frame, buffers[Faces], f.rates.p,
                                   static_cast<uint32_t *>(f.failure.p));
        check(cudaStreamSynchronize(f.stream.p));
        require(f.failure.read<uint32_t>()[0] != 0, "Invalid canonical face flux was silently discarded");
        f.rejected();
    }
    for (auto &p : memory)
        p->guard();
    std::cout << "{\"case\":\"owned-transport-projected-MAC-odd-grid\",\"maxEquationError\":" << error
              << ",\"rateMagnitude\":" << magnitude << ",\"pass\":true}\n";
}
} // namespace
int main() try {
    Model empty(2);
    for (auto &q : empty.source)
        q = {};
    standard("owned-transport-empty", empty);
    standard("owned-transport-zero-flux", Model(3));
    standard("owned-transport-circulation", cycle(2));
    standard("owned-transport-high-CFL-graph", cycle(16), true);
    Model closing(3);
    closing.capacity = {{{0, 1}}, {{0, 1}}, {{3, 1}}};
    closing.rates[closing.face(1, 0, 0, 0)] = 4;
    closing.rates[closing.face(2, 0, 0, 0)] = 8;
    standard("owned-transport-closing-chain", closing);
    std::reverse(closing.source.begin(), closing.source.end());
    std::reverse(closing.capacity.begin(), closing.capacity.end());
    closing.rates[closing.face(1, 0, 0, 0)] = -8;
    closing.rates[closing.face(2, 0, 0, 0)] = -4;
    standard("owned-transport-reverse-closing-chain", closing);
    invalidInput();
    rejectCapacity();
    rejectCap();
    tinyOwnership();
    invalidApi();
    pressureRates();
    std::cout << "PASS CUDA owned transport: 12 cases; quantity/flux correctness, not complete flowing FLIP "
                 "or realtime acceptance\n";
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA owned transport: " << e.what() << '\n';
    return 1;
}
