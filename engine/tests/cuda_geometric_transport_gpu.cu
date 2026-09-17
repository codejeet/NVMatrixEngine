#include "../src/fluid/cuda/fluid_cuda_geometric_transport.h"
#include "../src/fluid/cuda/fluid_cuda_geometry.cuh"
#include "../src/fluid/cuda/fluid_cuda_device.cuh"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace lab::cuda_fluid;
namespace {
using Q = std::array<double, 4>;
void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}
void check(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(e));
}
struct Buffer {
    void *p = nullptr;
    size_t bytes;
    explicit Buffer(size_t n) : bytes(n) {
        check(cudaMalloc(&p, n + 64));
        check(cudaMemset(p, 0, n));
        check(cudaMemset(static_cast<char *>(p) + n, 0xcd, 64));
    }
    ~Buffer() {
        cudaFree(p);
    }
    template <class T> void upload(const std::vector<T> &v) {
        require(v.size() * sizeof(T) == bytes, "Invalid upload size");
        check(cudaMemcpy(p, v.data(), bytes, cudaMemcpyHostToDevice));
    }
    template <class T> std::vector<T> read() const {
        std::vector<T> result(bytes / sizeof(T));
        check(cudaMemcpy(result.data(), p, bytes, cudaMemcpyDeviceToHost));
        return result;
    }
    void guard() const {
        std::array<unsigned char, 64> b;
        check(cudaMemcpy(b.data(), static_cast<const char *>(p) + bytes, 64, cudaMemcpyDeviceToHost));
        require(std::all_of(b.begin(), b.end(), [](auto v) { return v == 0xcd; }),
                "Geometric buffer guard changed");
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
// Independent polygon clipping + piecewise Simpson integration over z. No
// cube-difference formula or GPU inverse polynomial is used by this oracle.
double clippedArea(double x, double y, double alpha) {
    using P = std::array<double, 2>;
    std::vector<P> polygon{{0, 0}, {1, 0}, {1, 1}, {0, 1}}, out;
    for (uint32_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        const double da = x * a[0] + y * a[1] - alpha, db = x * b[0] + y * b[1] - alpha;
        if (da <= 0)
            out.push_back(a);
        if ((da <= 0) != (db <= 0)) {
            const double t = da / (da - db);
            out.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])});
        }
    }
    double area = 0;
    for (uint32_t i = 0; i < out.size(); ++i) {
        const auto a = out[i], b = out[(i + 1) % out.size()];
        area += a[0] * b[1] - a[1] * b[0];
    }
    return std::abs(area) * .5;
}
double oracle(double3 n, double alpha) {
    // Integrate along the weakest axis. Rounding a strong-axis knot by an ulp
    // perturbs its area by O(ulp/minorNormal), then Simpson gives the endpoint
    // spurious weight across a long interval. Permuting axes preserves volume
    // and conditions this independent polygon-clipping oracle.
    if (std::abs(n.z) > std::abs(n.x))
        std::swap(n.z, n.x);
    if (std::abs(n.z) > std::abs(n.y))
        std::swap(n.z, n.y);
    if (n.x == 0 && n.y == 0) {
        if (!n.z)
            return std::clamp(alpha, 0., 1.);
        const double cut = std::clamp(alpha / n.z, 0., 1.);
        return n.z > 0 ? cut : 1 - cut;
    }
    if (!n.z)
        return clippedArea(n.x, n.y, alpha);
    std::vector<double> knots{0, 1};
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y) {
            double z = (alpha - n.x * x - n.y * y) / n.z;
            if (z > 0 && z < 1)
                knots.push_back(z);
        }
    std::sort(knots.begin(), knots.end());
    double volume = 0;
    for (uint32_t i = 1; i < knots.size(); ++i) {
        double a = knots[i - 1], b = knots[i], m = (a + b) * .5;
        volume += (b - a) *
                  (clippedArea(n.x, n.y, alpha - n.z * a) + 4 * clippedArea(n.x, n.y, alpha - n.z * m) +
                   clippedArea(n.x, n.y, alpha - n.z * b)) /
                  6;
    }
    return volume;
}
struct PlaneRequest {
    double3 n;
    double fraction;
};
__global__ void solvePlanes(const PlaneRequest *request, double2 *output, uint32_t count) {
    const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= count)
        return;
    const auto r = request[i];
    const double a = geometry::intercept(r.n, r.fraction);
    output[i] = make_double2(a, geometry::fraction(r.n, a));
}
void planeOracle() {
    std::vector<PlaneRequest> requests;
    for (const auto n :
         {make_double3(1, 0, 0), make_double3(0, -1, 0), make_double3(0, 0, 1), make_double3(.2, .3, .5),
          make_double3(-.3, .6, -.1), make_double3(1e-12, .4, .6), make_double3(.5, .5, 0),
          make_double3(1e-8, 1e-7, 1), make_double3(1e-18, 1e-9, 1), make_double3(1e-16, 3e-16, 1),
          make_double3(4.8349030041717544e-104, 1, 1.8839514661144003e-105), make_double3(0, 1e-200, 1),
          make_double3(1e-200, 1e-100, 1), make_double3(1e-201, 1e-200, 1)})
        for (double f : {0., 1e-300, 1e-200, 1e-110, 1e-30, 1e-20, 1e-14, 1e-12, 1e-8, .001, .03, .13, .3, .5,
                         .71, .95, 1 - 1e-12, 1.})
            requests.push_back({n, f});
    Buffer input(requests.size() * sizeof(PlaneRequest)), output(requests.size() * sizeof(double2));
    input.upload(requests);
    Stream s;
    solvePlanes<<<(uint32_t(requests.size()) + 127) / 128, 128, 0, s.p>>>(
        static_cast<const PlaneRequest *>(input.p), static_cast<double2 *>(output.p),
        uint32_t(requests.size()));
    check(cudaStreamSynchronize(s.p));
    const auto actual = output.read<double2>();
    double error = 0;
    for (uint32_t i = 0; i < requests.size(); ++i) {
        const auto r = requests[i];
        require(std::isfinite(actual[i].x) && std::isfinite(actual[i].y), "Nonfinite PLIC result");
        const auto reflected = make_double3(std::abs(r.n.x), std::abs(r.n.y), std::abs(r.n.z));
        const double expected = oracle(reflected, actual[i].x);
        const double difference = std::abs(expected - r.fraction);
        if (difference >= 2e-12)
            std::cerr << std::setprecision(17) << "Plane oracle mismatch: n=" << r.n.x << ',' << r.n.y << ','
                      << r.n.z << " target=" << r.fraction << " alpha=" << actual[i].x
                      << " actual=" << actual[i].y << " oracle=" << expected << '\n';
        error = std::max(error, difference);
        require(std::abs(actual[i].y - r.fraction) < 3e-14, "PLIC inverse does not reproduce volume");
        if (r.fraction > 0 && r.fraction < 1e-5)
            require(std::abs(actual[i].y / r.fraction - 1) < 2e-12, "PLIC erased a tiny geometric interval");
    }
    require(error < 2e-12, "PLIC disagrees with independent clipped-volume integration");
    input.guard();
    output.guard();
    std::cout << "{\"case\":\"geometric-plane-oracle\",\"planes\":" << requests.size()
              << ",\"maximumError\":" << error << ",\"pass\":true}\n";
}
struct Fixture {
    uint32_t nx, ny, nz, stride;
    detail::Frame frame{};
    std::vector<Q> quantities;
    std::vector<std::array<double, 2>> phases;
    std::vector<double> velocities;
    Buffer source, phase, rates, output, flux, failure;
    std::unique_ptr<GeometricTransport, decltype(&destroyGeometricTransport)> transport;
    Stream stream;
    Fixture(uint32_t x, uint32_t y = 1, uint32_t z = 1, bool odd = false, uint32_t maximum = 16,
            uint32_t capacityIterations = 128)
        : nx(x), ny(y), nz(z), stride((x + 1) * (y + 1) * (z + 1)), quantities(x * y * z),
          phases(x * y * z, {0, 1}), velocities(3 * stride), source(quantities.size() * 32),
          phase(phases.size() * 16), rates(velocities.size() * 8), output(source.bytes),
          flux(velocities.size() * 32), failure(4),
          transport(
              createGeometricTransport({2 * x - uint32_t(odd), 2 * y, 2 * z, maximum, capacityIterations}),
              destroyGeometricTransport) {
        frame.grid = make_uint4(2 * x - uint32_t(odd), 2 * y, 2 * z, (2 * x - uint32_t(odd)) * 2 * y * 2 * z);
        frame.minimumCell = make_float4(0, 0, 0, .5f);
        frame.gravityDt.w = .25f;
        if (odd)
            for (uint32_t k = 0; k < z; ++k)
                for (uint32_t j = 0; j < y; ++j)
                    phases[(k * y + j) * x + x - 1][1] = .5;
    }
    uint32_t face(uint32_t x, uint32_t y, uint32_t z, uint32_t a) const {
        return a * stride + (z * (ny + 1) + y) * (nx + 1) + x;
    }
    void upload() {
        source.upload(quantities);
        phase.upload(phases);
        rates.upload(velocities);
    }
    GeometricTransportInputs inputs() {
        return {source.p, phase.p, nullptr, rates.p, output.p, flux.p, static_cast<uint32_t *>(failure.p)};
    }
    void run() {
        enqueueGeometricTransport(transport.get(), stream.p, &frame, inputs());
        check(cudaStreamSynchronize(stream.p));
    }
    GeometricTransportMetrics metrics() const {
        GeometricTransportMetrics m;
        check(cudaMemcpy(&m, geometricTransportMetrics(transport.get()), sizeof(m), cudaMemcpyDeviceToHost));
        return m;
    }
    double audit() {
        for (auto p : {&source, &phase, &rates, &output, &flux, &failure})
            p->guard();
        require(!failure.read<uint32_t>()[0], "Geometric transport rejected fixture");
        const auto m = metrics();
        require(m.completed == m.substeps && !m.invalid && !m.capped, "Invalid geometric completion");
        require(source.read<Q>() == quantities, "Geometric source owner was modified");
        const auto result = output.read<Q>(), transfers = flux.read<Q>();
        auto balance = quantities;
        std::vector<bool> internal(transfers.size());
        for (uint32_t a = 0; a < 3; ++a)
            for (uint32_t z = 0; z < nz; ++z)
                for (uint32_t y = 0; y < ny; ++y)
                    for (uint32_t x = 0; x < nx; ++x) {
                        uint32_t p[]{x, y, z}, dims[]{nx, ny, nz}, delta[]{1, nx, nx * ny};
                        if (p[a] + 1 >= dims[a])
                            continue;
                        const uint32_t l = (z * ny + y) * nx + x, r = l + delta[a];
                        p[a]++;
                        const uint32_t fi = face(p[0], p[1], p[2], a);
                        internal[fi] = true;
                        for (uint32_t k = 0; k < 4; ++k) {
                            balance[l][k] -= transfers[fi][k];
                            balance[r][k] += transfers[fi][k];
                        }
                    }
        double error = 0, initialEnergy = 0, finalEnergy = 0;
        Q initial{}, final{};
        for (uint32_t i = 0; i < result.size(); ++i) {
            require(result[i][3] >= 0 && result[i][3] <= phases[i][1] * (1 + 2e-13),
                    "Geometric owner left capacity bounds");
            for (uint32_t k = 0; k < 4; ++k) {
                require(std::isfinite(result[i][k]), "Nonfinite geometric quantity");
                initial[k] += quantities[i][k];
                final[k] += result[i][k];
                error = std::max(error, std::abs(result[i][k] - balance[i][k]));
                if (k < 3) {
                    if (quantities[i][3] > 0)
                        initialEnergy += quantities[i][k] * quantities[i][k] / quantities[i][3];
                    if (result[i][3] > 0)
                        finalEnergy += result[i][k] * result[i][k] / result[i][3];
                }
            }
        }
        for (uint32_t i = 0; i < internal.size(); ++i)
            if (!internal[i])
                require(transfers[i] == Q{}, "Boundary/padded flux is nonzero");
        for (uint32_t k = 0; k < 4; ++k)
            require(std::abs(initial[k] - final[k]) < 3e-12 * std::max(initial[3], 1e-90),
                    "Geometric transport lost global quantity");
        require(error < 2e-12 * std::max(initial[3], 1e-90),
                "Geometric face ledger fails independent balance");
        require(finalEnergy <= initialEnergy * (1 + 2e-12) + 1e-90,
                "Geometric transport adds translational energy");
        return error;
    }
};
void slab(bool graph, bool reverse) {
    Fixture f(32);
    const double speed = reverse ? -4 : 4;
    double left = reverse ? 15.2 : 5.2, right = left + 7.5;
    auto fill = [&] {
        for (uint32_t i = 0; i < f.nx; ++i) {
            double v = std::max(0., std::min(double(i + 1), right) - std::max(double(i), left));
            f.phases[i][0] = v;
            f.quantities[i] = {.3 * v, -.2 * v, .1 * v, v};
        }
    };
    fill();
    for (uint32_t i = 1; i < f.nx; ++i)
        f.velocities[f.face(i, 0, 0, 0)] = speed;
    f.upload();
    cudaGraph_t g = nullptr;
    cudaGraphExec_t exec = nullptr;
    if (graph) {
        check(cudaStreamBeginCapture(f.stream.p, cudaStreamCaptureModeThreadLocal));
        enqueueGeometricTransport(f.transport.get(), f.stream.p, &f.frame, f.inputs());
        check(cudaStreamEndCapture(f.stream.p, &g));
        check(cudaGraphInstantiate(&exec, g, 0));
    }
    double error = 0;
    for (uint32_t step = 0; step < 8; ++step) {
        if (graph) {
            check(cudaGraphLaunch(exec, f.stream.p));
            check(cudaStreamSynchronize(f.stream.p));
        } else
            f.run();
        f.audit();
        const auto result = f.output.read<Q>();
        left += speed * f.frame.gravityDt.w;
        right += speed * f.frame.gravityDt.w;
        for (uint32_t i = 0; i < f.nx; ++i) {
            double exact = std::max(0., std::min(double(i + 1), right) - std::max(double(i), left));
            error = std::max(error, std::abs(result[i][3] - exact));
            if (exact == 0)
                require(result[i][3] == 0, "Geometric slab leaked a dilute tail into dry space");
            f.phases[i][0] = result[i][3];
        }
        f.quantities = result;
        f.upload();
    }
    if (exec)
        cudaGraphExecDestroy(exec);
    if (g)
        cudaGraphDestroy(g);
    require(error < 2e-12, "Geometric slab does not translate at prescribed velocity");
    require(f.metrics().substeps == 3, "GPU CFL schedule did not subdivide high-Courant advection");
    require(!f.metrics().capacityIterations,
            "Admissible translation executed unnecessary capacity iterations");
    std::cout << "{\"case\":\"geometric-slab-"
              << (graph     ? "graph"
                  : reverse ? "reverse"
                            : "direct")
              << "\",\"maximumError\":" << error << ",\"pass\":true}\n";
}
void circulation(bool full, bool odd) {
    Fixture f(7, 5, 3, odd);
    for (uint32_t i = 0; i < f.quantities.size(); ++i) {
        double volume = f.phases[i][1] * (full ? 1. : .3 + .04 * (i % 7));
        f.phases[i][0] = volume;
        f.quantities[i] = {volume * (.03 * (i % 5) - .04), volume * (-.2 + .03 * (i % 9)), volume * .1,
                           volume};
    }
    // Exact divergence-free face circulation from a discrete stream function.
    auto psi = [&](uint32_t x, uint32_t y) { return double(x * (f.nx - x) * y * (f.ny - y)) * .003; };
    for (uint32_t z = 0; z < f.nz; ++z) {
        for (uint32_t y = 0; y < f.ny; ++y)
            for (uint32_t x = 1; x < f.nx; ++x)
                f.velocities[f.face(x, y, z, 0)] = psi(x, y + 1) - psi(x, y);
        for (uint32_t y = 1; y < f.ny; ++y)
            for (uint32_t x = 0; x < f.nx; ++x)
                f.velocities[f.face(x, y, z, 1)] = -(psi(x + 1, y) - psi(x, y));
    }
    // Add curls in the other two planes: six-face flux correction must work
    // simultaneously, not merely on independent 2D layers of a 3D allocation.
    auto yz = [&](uint32_t y, uint32_t z) { return double(y * (f.ny - y) * z * (f.nz - z)) * .006; };
    for (uint32_t x = 0; x < f.nx; ++x) {
        for (uint32_t z = 0; z < f.nz; ++z)
            for (uint32_t y = 1; y < f.ny; ++y)
                f.velocities[f.face(x, y, z, 1)] += yz(y, z + 1) - yz(y, z);
        for (uint32_t z = 1; z < f.nz; ++z)
            for (uint32_t y = 0; y < f.ny; ++y)
                f.velocities[f.face(x, y, z, 2)] -= yz(y + 1, z) - yz(y, z);
    }
    auto zx = [&](uint32_t z, uint32_t x) { return double(z * (f.nz - z) * x * (f.nx - x)) * .004; };
    for (uint32_t y = 0; y < f.ny; ++y) {
        for (uint32_t x = 0; x < f.nx; ++x)
            for (uint32_t z = 1; z < f.nz; ++z)
                f.velocities[f.face(x, y, z, 2)] += zx(z, x + 1) - zx(z, x);
        for (uint32_t x = 1; x < f.nx; ++x)
            for (uint32_t z = 0; z < f.nz; ++z)
                f.velocities[f.face(x, y, z, 0)] -= zx(z + 1, x) - zx(z, x);
    }
    double error = 0;
    for (uint32_t step = 0; step < 8; ++step) {
        f.upload();
        f.run();
        error = std::max(error, f.audit());
        f.quantities = f.output.read<Q>();
        for (uint32_t i = 0; i < f.phases.size(); ++i) {
            f.phases[i][0] = f.quantities[i][3];
            if (full)
                require(std::abs(f.phases[i][0] - f.phases[i][1]) < 2e-13,
                        "Full phase lost incompressible circulation");
        }
    }
    std::cout << "{\"case\":\"geometric-"
              << (full  ? "full-circulation"
                  : odd ? "odd-circulation"
                        : "mixed-circulation")
              << "\",\"balanceError\":" << error << ",\"pass\":true}\n";
}
void rejection() {
    for (uint32_t variant = 0; variant < 9; ++variant) {
        Fixture f(3);
        Buffer planes(3 * sizeof(double4));
        f.phases[0][0] = .4;
        f.quantities[0] = {.1, 0, 0, .4};
        if (variant == 0)
            f.phases[0][0] = .1;
        if (variant == 1)
            f.phases[0][1] = .5;
        if (variant == 2)
            f.velocities[0] = 1;
        if (variant == 3)
            f.velocities[f.face(1, 0, 0, 0)] = NAN;
        if (variant == 4)
            f.velocities[f.face(1, 0, 0, 0)] = 1000;
        if (variant == 5)
            f.quantities[0][0] = NAN;
        if (variant >= 6) {
            auto plane = make_double4(1, 0, 0, .4);
            if (variant == 6)
                plane.x = NAN;
            if (variant == 7)
                plane.x = 2;
            if (variant == 8)
                plane.w = .3;
            planes.upload(std::vector<double4>{plane, {}, {}});
        }
        f.upload();
        const auto before = f.output.read<unsigned char>(), flux = f.flux.read<unsigned char>();
        auto input = f.inputs();
        if (variant >= 6)
            input.planes = planes.p;
        enqueueGeometricTransport(f.transport.get(), f.stream.p, &f.frame, input);
        check(cudaStreamSynchronize(f.stream.p));
        require(f.failure.read<uint32_t>()[0] && f.output.read<unsigned char>() == before &&
                    f.flux.read<unsigned char>() == flux,
                "Invalid geometric input partially published");
        if (variant == 4)
            require(f.metrics().capped, "Oversized geometric CFL did not report its cap");
        const auto first = f.metrics();
        f.run();
        const auto replay = f.metrics();
        require(!std::memcmp(&first, &replay, sizeof(first)) && f.output.read<unsigned char>() == before &&
                    f.flux.read<unsigned char>() == flux,
                "Later work erased geometric failure evidence or published a prefix");
        for (auto p : {&f.source, &f.phase, &f.rates, &f.output, &f.flux, &f.failure, &planes})
            p->guard();
    }
    std::cout << "{\"case\":\"geometric-invalid-publication\",\"variants\":9,\"pass\":true}\n";
}
void invalidApi() {
    Fixture f(3);
    f.upload();
    uint32_t rejected = 0;
    for (uint32_t variant = 0; variant < 12; ++variant) {
        auto input = f.inputs();
        auto frame = f.frame;
        if (variant == 0)
            input.quantity = nullptr;
        if (variant == 1)
            input.output = f.phase.p;
        if (variant == 2)
            input.planes = input.rates;
        if (variant == 3)
            frame.grid.x++;
        if (variant == 4)
            frame.gravityDt.w = 0;
        if (variant == 5)
            frame.minimumCell.w = NAN;
        try {
            if (variant < 9)
                enqueueGeometricTransport(variant == 6 ? nullptr : f.transport.get(),
                                          variant == 7 ? nullptr : f.stream.p,
                                          variant == 8 ? nullptr : &frame, input);
            else {
                auto *p = createGeometricTransport({variant == 9 ? 0u : 6u, 2, 2, 16,
                                                    variant == 10   ? 0u
                                                    : variant == 11 ? 513u
                                                                    : 128u});
                destroyGeometricTransport(p);
            }
        } catch (const std::exception &) {
            ++rejected;
        }
    }
    require(rejected == 12, "Invalid geometric API binding/configuration was accepted");
    std::cout << "{\"case\":\"geometric-invalid-api\",\"variants\":12,\"pass\":true}\n";
}
void tiny() {
    Fixture f(3);
    for (uint32_t i = 0; i < 3; ++i) {
        f.phases[i][0] = 1e-30;
        f.quantities[i] = {1e-61, -2e-61, 0, 1e-30};
    }
    f.velocities[f.face(1, 0, 0, 0)] = .1;
    f.velocities[f.face(2, 0, 0, 0)] = .1;
    f.upload();
    f.run();
    f.audit();
    for (const auto &q : f.output.read<Q>())
        require(q[3] > 0, "Tiny valid owner was erased");
    std::cout << "{\"case\":\"geometric-tiny-ownership\",\"pass\":true}\n";
}
void fullPhaseOwnershipTag() {
    Fixture f(2, 2);
    const uint32_t faces[]{f.face(1, 0, 0, 0), f.face(1, 1, 0, 1), f.face(1, 1, 0, 0), f.face(0, 1, 0, 1)};
    const uint32_t donors[]{0, 1, 3, 2}, receivers[]{1, 3, 2, 0};
    for (uint32_t i = 0; i < 4; ++i) {
        const double v = .1 + .2 * i;
        f.phases[i][0] = 1; // Other owners fill the rest: no air interface.
        f.quantities[i] = {.03 * v, -.1 * v, .2 * v, v};
        f.velocities[faces[i]] = i < 2 ? .4 : -.4;
    }
    auto exact = f.quantities;
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 4; ++k) {
            const double flux = .4 * f.frame.gravityDt.w * f.quantities[donors[i]][k];
            exact[donors[i]][k] -= flux;
            exact[receivers[i]][k] += flux;
        }
    f.upload();
    f.run();
    f.audit();
    const auto result = f.output.read<Q>();
    for (uint32_t i = 0; i < 4; ++i)
        for (uint32_t k = 0; k < 4; ++k)
            require(std::abs(result[i][k] - exact[i][k]) < 2e-15,
                    "Ownership fraction was mistaken for a liquid-air interface");
    std::cout << "{\"case\":\"geometric-full-phase-ownership-tag\",\"pass\":true}\n";
}
void tinySweptSlab() {
    Fixture f(3);
    Buffer planes(3 * sizeof(double4));
    planes.upload(std::vector<double4>{make_double4(-1, 0, 0, 1e-30), {}, {}});
    f.phases[0][0] = 1e-30;
    f.quantities[0] = {1e-61, -2e-61, 0, 1e-30};
    f.velocities[f.face(1, 0, 0, 0)] = 1e-20;
    f.upload();
    auto input = f.inputs();
    input.planes = planes.p;
    enqueueGeometricTransport(f.transport.get(), f.stream.p, &f.frame, input);
    check(cudaStreamSynchronize(f.stream.p));
    f.audit();
    planes.guard();
    const auto result = f.output.read<Q>();
    require(result[0] == Q{} && result[1] == f.quantities[0] && result[2] == Q{},
            "Sub-ulp far-corner sweep did not transfer the complete tiny owner exactly");
    std::cout << "{\"case\":\"geometric-tiny-swept-slab\",\"pass\":true}\n";
}
void capacityCycle(bool graph, bool capped) {
    Fixture f(2, 2, 1, false, 16, capped ? 1 : 128);
    const uint32_t faces[]{f.face(1, 0, 0, 0), f.face(1, 1, 0, 1), f.face(1, 1, 0, 0), f.face(0, 1, 0, 1)};
    const uint32_t donors[]{0, 1, 3, 2}, receivers[]{1, 3, 2, 0};
    const double speeds[]{.4, .3, .5, .2};
    for (uint32_t i = 0; i < 4; ++i) {
        f.phases[i][0] = 1;
        f.quantities[i] = {.02 * i, -.1, .03, 1};
        f.velocities[faces[i]] = (i < 2 ? 1 : -1) * speeds[i];
    }
    f.upload();
    const auto before = f.output.read<Q>(), beforeFlux = f.flux.read<Q>();
    cudaGraph_t g = nullptr;
    cudaGraphExec_t exec = nullptr;
    if (graph) {
        check(cudaStreamBeginCapture(f.stream.p, cudaStreamCaptureModeThreadLocal));
        enqueueGeometricTransport(f.transport.get(), f.stream.p, &f.frame, f.inputs());
        check(cudaStreamEndCapture(f.stream.p, &g));
        check(cudaGraphInstantiate(&exec, g, 0));
        check(cudaGraphLaunch(exec, f.stream.p));
        check(cudaStreamSynchronize(f.stream.p));
    } else
        f.run();
    const auto m = f.metrics();
    require(m.capacityIterations > 0 && bool(m.capacityCapped) == capped,
            "Capacity cycle did not exercise bounded admission");
    if (capped) {
        require(f.failure.read<uint32_t>()[0] && f.output.read<Q>() == before &&
                    f.flux.read<Q>() == beforeFlux,
                "Capped capacity admission published partial quantities");
        check(cudaGraphLaunch(exec, f.stream.p));
        check(cudaStreamSynchronize(f.stream.p));
        const auto replay = f.metrics();
        require(!std::memcmp(&m, &replay, sizeof(m)) && f.output.read<Q>() == before &&
                    f.flux.read<Q>() == beforeFlux,
                "Rejected capacity graph replay lost its sticky failure or changed publication");
    } else {
        f.audit();
        auto exact = f.quantities;
        const double circulation = *std::min_element(speeds, speeds + 4) * f.frame.gravityDt.w;
        for (uint32_t i = 0; i < 4; ++i)
            for (uint32_t k = 0; k < 4; ++k) {
                const double quantity = circulation * f.quantities[donors[i]][k];
                exact[donors[i]][k] -= quantity;
                exact[receivers[i]][k] += quantity;
            }
        const auto actual = f.output.read<Q>(), flux = f.flux.read<Q>();
        for (uint32_t i = 0; i < 4; ++i) {
            require(std::abs(std::abs(flux[faces[i]][3]) - circulation) < 2e-14,
                    "Capacity cycle failed the independent bottleneck-flux oracle");
            for (uint32_t k = 0; k < 4; ++k)
                require(std::abs(actual[i][k] - exact[i][k]) < 2e-14,
                        "Capacity cycle changed mass or donor-consistent momentum");
        }
        if (graph) {
            const auto activeRates = f.velocities;
            std::fill(f.velocities.begin(), f.velocities.end(), 0.);
            f.rates.upload(f.velocities);
            check(cudaGraphLaunch(exec, f.stream.p));
            check(cudaStreamSynchronize(f.stream.p));
            f.audit();
            require(!f.metrics().capacityIterations && f.output.read<Q>() == f.quantities,
                    "Inactive capacity replay retained a prior loop condition or changed water");
            f.velocities = activeRates;
            f.rates.upload(f.velocities);
            check(cudaGraphLaunch(exec, f.stream.p));
            check(cudaStreamSynchronize(f.stream.p));
            f.audit();
            require(f.metrics().capacityIterations == m.capacityIterations && f.output.read<Q>() == actual,
                    "Reactivated capacity graph changed its numerical result");
        }
    }
    require(bool(geometricTransportCapturedBodyNodes(f.transport.get())) == graph,
            "Capacity graph did not use its device-controlled iteration body");
    for (auto p : {&f.source, &f.phase, &f.rates, &f.output, &f.flux, &f.failure})
        p->guard();
    if (exec)
        cudaGraphExecDestroy(exec);
    if (g)
        cudaGraphDestroy(g);
    std::cout << "{\"case\":\"geometric-capacity-"
              << (capped  ? "capped"
                  : graph ? "graph"
                          : "direct")
              << "\",\"iterations\":" << m.capacityIterations << ",\"pass\":true}\n";
}
} // namespace
int main() try {
    planeOracle();
    slab(false, false);
    slab(true, false);
    slab(false, true);
    circulation(false, false);
    circulation(true, false);
    circulation(false, true);
    rejection();
    invalidApi();
    tiny();
    fullPhaseOwnershipTag();
    tinySweptSlab();
    capacityCycle(false, false);
    capacityCycle(true, false);
    capacityCycle(true, true);
    std::cout
        << "PASS CUDA geometric transport: 15 cases; static geometric phase/flux validation, not moving "
           "joint-interface or rendering acceptance\n";
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA geometric transport: " << e.what() << '\n';
    return 1;
}
