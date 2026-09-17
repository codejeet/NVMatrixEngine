#include "cuda_fluid_fixture.h"
#include "cuda_surface_rays.h"
#include "cuda_surface_planes.h"
#include "cuda_surface_curved.h"
#include "../src/fluid/fluid_cuda.h"
#include "../src/fluid/fluid_surface.h"
#include <cmath>
#include <cstring>
#include <sstream>
#include <map>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
namespace {
using namespace lab;
using namespace lab::cuda_fluid;
using lab::cuda_test::require;
using Microsoft::WRL::ComPtr;
void clipping(cuda_test::Fixture &d, const std::filesystem::path &folder) {
    using D4 = std::array<double, 4>;
    using D3 = std::array<double, 3>;
    std::vector<std::array<double, 2>> phases;
    std::vector<D4> planes, boxes;
    std::vector<double> expected;
    // Independent inclusion/exclusion integral (not the sorted piecewise
    // polynomial used by either production GPU implementation).
    auto oracle = [](D4 p, D3 lo, D3 hi) {
        long double alpha = p[3], volume = 1;
        std::array<long double, 3> n{};
        uint32_t dimensions = 0;
        for (uint32_t a = 0; a < 3; ++a) {
            alpha -= std::abs(p[a]) * (p[a] < 0 ? 1 - hi[a] : lo[a]);
            if (p[a] != 0) {
                n[dimensions++] = std::abs(p[a]) * (hi[a] - lo[a]);
                volume *= n[dimensions - 1];
            }
        }
        if (alpha <= 0)
            return 0.;
        long double support = 0;
        for (uint32_t a = 0; a < dimensions; ++a)
            support += n[a];
        if (alpha >= support)
            return 1.;
        long double integral = 0;
        for (uint32_t k = 0; k < (1u << dimensions); ++k) {
            long double t = alpha;
            int sign = 1;
            for (uint32_t a = 0; a < dimensions; ++a)
                if (k & (1u << a)) {
                    t -= n[a];
                    sign = -sign;
                }
            if (t > 0) {
                long double power = 1;
                for (uint32_t a = 0; a < dimensions; ++a)
                    power *= t;
                integral += sign * power;
            }
        }
        for (uint32_t a = 2; a <= dimensions; ++a)
            volume *= a;
        return std::clamp(double(integral / volume), 0., 1.);
    };
    auto add = [&](D4 plane, D3 lo, D3 hi) {
        const double global = oracle(plane, {0, 0, 0}, {1, 1, 1});
        phases.push_back({global, 1});
        planes.push_back(plane);
        boxes.push_back({lo[0], lo[1], lo[2], 0});
        boxes.push_back({hi[0], hi[1], hi[2], 0});
        expected.push_back(oracle(plane, lo, hi));
    };
    for (uint32_t signs = 0; signs < 8; ++signs)
        for (uint32_t sample = 1; sample < 20; ++sample) {
            const D4 p{signs & 1 ? -.2 : .2, signs & 2 ? -.3 : .3, signs & 4 ? -.5 : .5, sample / 20.};
            add(p, {0, 0, 0}, {1, 1, 1});
            add(p, {.13, .07, .21}, {.87, .93, .79});
        }
    for (double thickness : {1e-30, 1e-20, 1e-10, .001, .125})
        for (double direction : {-1., 1.}) {
            add({direction, 0, 0, thickness}, {0, 0, 0}, {1, 1, 1});
            add({direction, 0, 0, thickness}, {direction < 0 ? .5 : 0, 0, 0}, {direction < 0 ? 1 : .5, 1, 1});
        }
    std::vector<gpu::Buffer> gpuInputs, uploads;
    d.begin();
    auto upload = [&](const void *data, uint64_t size) {
        gpuInputs.push_back(d.make(size));
        uploads.push_back(gpu::buffer(d.device.Get(), size, D3D12_HEAP_TYPE_UPLOAD));
        std::memcpy(uploads.back().mapped, data, size_t(size));
        auto *r = gpuInputs.back().resource.Get();
        gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
        d.cmd->CopyBufferRegion(r, 0, uploads.back().resource.Get(), 0, size);
        gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    upload(phases.data(), phases.size() * sizeof(phases[0]));
    upload(planes.data(), planes.size() * sizeof(planes[0]));
    upload(boxes.data(), boxes.size() * sizeof(boxes[0]));
    auto result = d.make(expected.size() * 16);
    auto readback = gpu::buffer(d.device.Get(), expected.size() * 16, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_ROOT_PARAMETER parameters[5]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    for (uint32_t i = 1; i < 5; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{5, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    ComPtr<ID3D12RootSignature> root;
    gpu::check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Box root serialize");
    gpu::check(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root)),
               "Box root");
    auto code = gpu::bytes(folder / "shaders/CudaSurfaceBoxProbe.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.CS = {code.data(), code.size()};
    ComPtr<ID3D12PipelineState> pipeline;
    gpu::check(d.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pipeline)), "Box pipeline");
    const uint32_t constants[]{uint32_t(expected.size()), 0, 0, 0};
    d.cmd->SetComputeRootSignature(root.Get());
    d.cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);
    ID3D12Resource *resources[]{gpuInputs[0].resource.Get(), gpuInputs[1].resource.Get(),
                                result.resource.Get(), gpuInputs[2].resource.Get()};
    for (uint32_t i = 0; i < 4; ++i)
        d.cmd->SetComputeRootUnorderedAccessView(i + 1, resources[i]->GetGPUVirtualAddress());
    d.cmd->SetPipelineState(pipeline.Get());
    d.cmd->Dispatch((uint32_t(expected.size()) + 63) / 64, 1, 1);
    gpu::uav(d.cmd.Get());
    d.copyOut(result.resource.Get(), readback.resource.Get(), expected.size() * 16, 0);
    d.submit();
    void *data = nullptr;
    D3D12_RANGE range{0, expected.size() * 16}, written{0, 0};
    gpu::check(readback.resource->Map(0, &range, &data), "Box integral readback");
    const auto *values = static_cast<const DirectX::XMFLOAT4 *>(data);
    double maxError = 0;
    for (uint32_t i = 0; i < expected.size(); ++i) {
        double errorValue = std::abs(values[i].x - expected[i]);
        maxError = std::max(maxError, errorValue);
        const double tolerance = expected[i] > 0 && expected[i] < 1e-6 ? expected[i] * 2e-6 : 2e-7;
        require(std::isfinite(values[i].x) && errorValue <= tolerance,
                "Oblique/reflected/thin phase box integral mismatch");
    }
    readback.resource->Unmap(0, &written);
    std::cout << "{\"case\":\"surface-box-integrals\",\"samples\":" << expected.size()
              << ",\"maxError\":" << maxError << ",\"pass\":true}\n";
}
void run(cuda_test::Fixture &d, const std::filesystem::path &folder, bool graph) {
    Config c{};
    c.nx = 5;
    c.ny = 7;
    c.nz = 3;
    c.capacity = 1;
    c.pressureIterations = 32;
    c.viscositySubsteps = 1;
    c.ownedParticles = true;
    c.deterministic = true;
    c.graphs = graph;
    constexpr uint32_t coarse = 3 * 4 * 2;
    std::vector<gpu::Buffer> buffers, uploads;
    std::array<ID3D12Resource *, BufferCount> resources{};
    std::array<ID3D12Resource *, OwnershipBufferCount> ownership{};
    std::array<ID3D12Resource *, GridInventoryBufferCount> grid{};
    std::array<ID3D12Resource *, SurfaceGeometryBufferCount> surface{};
    d.begin();
    auto add = [&](uint64_t size, bool output = false) {
        buffers.push_back(d.make(size + 64, true));
        uploads.push_back(gpu::buffer(d.device.Get(), size + 64, D3D12_HEAP_TYPE_UPLOAD));
        std::memset(uploads.back().mapped, output ? 0xcd : 0, size_t(size));
        std::memset(static_cast<char *>(uploads.back().mapped) + size, 0xcd, 64);
        auto *r = buffers.back().resource.Get();
        gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
        d.cmd->CopyBufferRegion(r, 0, uploads.back().resource.Get(), 0, size + 64);
        gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return r;
    };
    for (uint32_t i = 0; i < resources.size(); ++i)
        resources[i] = add(bufferBytes(c, Buffer(i)));
    for (uint32_t i = 0; i < ownership.size(); ++i)
        ownership[i] = add(ownershipBytes(c, OwnershipBuffer(i)));
    for (uint32_t i = 0; i < grid.size(); ++i)
        grid[i] = add(gridInventoryBytes(c, GridInventoryBuffer(i)));
    for (uint32_t i = 0; i < surface.size(); ++i)
        surface[i] = add(surfaceGeometryBytes(c, SurfaceGeometryBuffer(i)), true);
    d.submit();
    FluidCuda solver(d.device.Get(), c, resources, {}, d.queue.Get(), false, ownership, grid, surface);
    auto result = d.make(coarse * 16);
    auto readback = gpu::buffer(d.device.Get(), coarse * 16 + coarse * 48 + 128, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_ROOT_PARAMETER p[6]{};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 4;
    for (uint32_t i = 1; i < 6; ++i) {
        p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{6, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    ComPtr<ID3D12RootSignature> root;
    gpu::check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Surface probe root");
    gpu::check(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root)),
               "Surface probe signature");
    ComPtr<ID3D12PipelineState> seed, probe;
    auto pipeline = [&](const char *name, ComPtr<ID3D12PipelineState> &out) {
        auto code = gpu::bytes(folder / "shaders" / (std::string(name) + ".dxil"));
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
        ps.pRootSignature = root.Get();
        ps.CS = {code.data(), code.size()};
        gpu::check(d.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&out)), name);
    };
    pipeline("CudaSurfaceSeed", seed);
    pipeline("CudaSurfaceProbe", probe);
    FluidSimulationConstants f{};
    f.grid = {c.nx, c.ny, c.nz, c.nx * c.ny * c.nz};
    f.counts.x = c.capacity;
    f.minimumCell = {0, 0, 0, .5f};
    f.maximumRadius = {2.5f, 3.5f, 1.5f, .125f};
    f.gravityDt = {0, 0, 0, 1.f / 120};
    f.solver = {1000, .95f, 0, .125f};
    f.material.z = 1;
    f.initialMinimum.w = .015625f;
    f.display.w = 2;
    FluidColliderTimeline colliders{};
    FluidSystemDesc desc{};
    desc.minimum = {0, 0, 0};
    desc.maximum = {2.5f, 3.5f, 1.5f};
    desc.gridCellSize = .5f;
    desc.maxParticles = 1;
    ComPtr<ID3D12Device5> device5;
    ComPtr<ID3D12GraphicsCommandList4> cmd4;
    gpu::check(d.device.As(&device5), "Canonical surface DXR device");
    gpu::check(d.cmd.As(&cmd4), "Canonical surface DXR command list");
    FluidSurface canonical(device5.Get(), folder, desc);
    cuda_test::SurfaceRays rayProbe(d, device5.Get(), folder);
    FluidGpuView view{};
    view.particles = resources[Particles];
    view.offsets = resources[Offsets];
    view.indices = resources[Indices];
    view.previousPositions = resources[Solid];
    view.grid = f.grid;
    view.faces = resources[Faces];
    // Unused in uncarved phase mode, but root descriptors remain valid.
    view.meshPhi = view.interior = view.interiorTotals = resources[Solid];
    view.colliderAddress = resources[Solid]->GetGPUVirtualAddress();
    const uint64_t fieldBytes = canonical.fieldResource()->GetDesc().Width;
    const uint64_t mapBytes = canonical.mapResource()->GetDesc().Width;
    auto fieldReadback = gpu::buffer(d.device.Get(), fieldBytes + mapBytes, D3D12_HEAP_TYPE_READBACK,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    uint64_t frequency = 0;
    gpu::check(d.queue->GetTimestampFrequency(&frequency), "Canonical surface timestamp frequency");
    double maxFieldError = 0, maxVolumeError = 0;
    uint32_t sharedNodes = 0;
    std::vector<char> accepted;
    auto bind = [&](uint32_t mode) {
        const uint32_t constants[]{c.nx, c.ny, c.nz, mode};
        d.cmd->SetComputeRootSignature(root.Get());
        d.cmd->SetComputeRoot32BitConstants(0, 4, constants, 0);
        ID3D12Resource *views[]{surface[0], surface[1], result.resource.Get(), grid[0], grid[1]};
        for (uint32_t i = 0; i < 5; ++i)
            d.cmd->SetComputeRootUnorderedAccessView(i + 1, views[i]->GetGPUVirtualAddress());
    };
    for (uint32_t mode = 0; mode < 5; ++mode) {
        d.begin();
        bind(mode);
        d.cmd->SetPipelineState(seed.Get());
        d.cmd->Dispatch(2, 1, 1);
        gpu::uav(d.cmd.Get());
        // Real engine adapter: DX12 seed -> CUDA Solver -> DX12 shader consumer.
        // No simulation data is read back or uploaded between these producers.
        solver.run(d.cmd.Get(), d.queue.Get(), d.allocator.Get(), f, colliders, mode == 1 ? 2u : 0u, false,
                   mode == 2);
        bind(mode);
        d.cmd->SetPipelineState(probe.Get());
        d.cmd->Dispatch(1, 1, 1);
        gpu::uav(d.cmd.Get());
        canonical.record(cmd4.Get(),
                         {view, desc, mode == 1 ? 2u : 0u, true, mode == 2, mode == 1 ? 2.f / 120 : 0.f,
                          surface[0], surface[1]},
                         Camera{}, 1.f / 60);
        canonical.recordReadback(d.cmd.Get());
        rayProbe.record(cmd4.Get(), canonical);
        uint64_t fieldOffset = 0;
        for (auto *r : {canonical.fieldResource(), canonical.mapResource()}) {
            gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            d.cmd->CopyBufferRegion(fieldReadback.resource.Get(), fieldOffset, r, 0, r->GetDesc().Width);
            fieldOffset += r->GetDesc().Width;
            gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        d.copyOut(result.resource.Get(), readback.resource.Get(), coarse * 16, 0);
        uint64_t offset = coarse * 16;
        for (uint32_t i = 0; i < surface.size(); ++i) {
            const auto bytes = surfaceGeometryBytes(c, SurfaceGeometryBuffer(i));
            d.copyOut(surface[i], readback.resource.Get(), bytes + 64, offset);
            offset += bytes + 64;
        }
        d.submit();
        bool rejected = false;
        try {
            solver.collect();
        } catch (const std::exception &) {
            rejected = true;
        }
        require(rejected == (mode == 4), "Surface handoff did not reject exactly the invalid capacity frame");
        rayProbe.collect(mode);
        canonical.collect(frequency);
        void *fieldData = nullptr;
        D3D12_RANGE fieldRange{0, size_t(fieldBytes + mapBytes)}, fieldWritten{0, 0};
        gpu::check(fieldReadback.resource->Map(0, &fieldRange, &fieldData), "Canonical surface readback");
        const auto *nodes = static_cast<const DirectX::XMFLOAT4 *>(fieldData);
        const auto *pages =
            reinterpret_cast<const uint32_t *>(static_cast<const char *>(fieldData) + fieldBytes);
        std::map<std::array<uint32_t, 3>, DirectX::XMFLOAT4> seen;
        const auto bricks = canonical.brickGrid;
        uint32_t active = 0;
        for (uint32_t brick = 0; brick < bricks.w; ++brick) {
            uint32_t slot = pages[brick];
            if (slot == 0xffffffff)
                continue;
            require(slot < bricks.w, "Canonical surface page overflow");
            ++active;
            for (uint32_t n = 0; n < 729; ++n) {
                const std::array<uint32_t, 3> key{(brick % bricks.x) * 8 + n % 9,
                                                  (brick / bricks.x % bricks.y) * 8 + n / 9 % 9,
                                                  (brick / (bricks.x * bricks.y)) * 8 + n / 81};
                const auto value = nodes[slot * 729 + n];
                auto [entry, inserted] = seen.emplace(key, value);
                if (!inserted) {
                    ++sharedNodes;
                    require(!std::memcmp(&entry->second, &value, sizeof(value)),
                            "Canonical brick seam differs");
                }
                // Independent signed-distance oracle in physical metres. The
                // phase conversion must preserve planes, not merely reproduce
                // the implementation's nonlinear occupancy field.
                const double lo[]{0, mode == 1 ? 1.75 : 0, 0};
                const double hi[]{2.5, mode == 1 ? 3.5 : 1.25, 1.5};
                const double domain[]{2.5, 3.5, 1.5};
                double domainPhi = -1e30;
                for (uint32_t axis = 0; axis < 3; ++axis) {
                    const double position = double(key[axis]) * .25 - 1;
                    domainPhi = std::max(domainPhi, std::max(-position, position - domain[axis]));
                }
                const double y = double(key[1]) * .25 - 1;
                const double liquidPhi =
                    mode == 2 ? .5 : std::clamp(mode == 1 ? lo[1] - y : y - hi[1], -.5, .5);
                // Raw free-surface field; exact domain clipping is exercised by
                // the shared sampler and DXR oracle, not baked into these nodes.
                const double expected = domainPhi >= .5 ? .5 : liquidPhi;
                maxFieldError = std::max(maxFieldError, std::abs(value.x - expected));
                require(std::isfinite(value.x) && std::abs(value.x - expected) < 2e-7 && value.y == 0 &&
                            value.z == 0 && value.w == 0,
                        "Canonical phase field disagrees with physical-volume oracle or stale motion");
            }
        }
        require(active == canonical.activeBricks && (active == 0) == (mode == 2),
                "Canonical phase activation/reset mismatch");
        require((canonical.surfaceBricks == 0) == (mode == 2) && canonical.accelerationStructure() != 0,
                "Canonical phase BLAS did not rebuild/reset");
        const double exactVolume = mode == 2 ? 0 : 2.5 * 1.5 * (mode == 1 ? 1.75 : 1.25);
        maxVolumeError = std::max(maxVolumeError, std::abs(canonical.renderedVolume - exactVolume));
        if (std::abs(canonical.renderedVolume - exactVolume) >= 1e-6)
            std::cerr << "Planar volume mode " << mode << ": " << canonical.renderedVolume << " expected "
                      << exactVolume << '\n';
        require(std::abs(canonical.renderedVolume - exactVolume) < 1e-6,
                "Planar pool contour lost volume at domain walls");
        fieldReadback.resource->Unmap(0, &fieldWritten);
        void *mapped = nullptr;
        D3D12_RANGE range{0, size_t(offset)};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Surface probe readback");
        const auto *v = static_cast<const DirectX::XMFLOAT4 *>(mapped);
        for (uint32_t i = 0; i < coarse; ++i) {
            const uint32_t x = i % 3, y = i / 3 % 4, z = i / 12;
            const double capacity =
                (2 * x + 1 < c.nx ? 2 : 1) * (2 * y + 1 < c.ny ? 2 : 1) * (2 * z + 1 < c.nz ? 2 : 1) * .125;
            const double fraction = mode == 2   ? 0
                                    : y == 1    ? .25
                                    : mode == 1 ? (y >= 2 ? 1. : 0.)
                                                : (y == 0 ? 1. : 0.);
            const float mask = fraction == 0 ? 0 : fraction == 1 ? 7 : mode == 1 ? 4.f : 1.f;
            require(std::abs(v[i].x - capacity * fraction) < 1e-7 && std::abs(v[i].y - capacity) < 1e-7 &&
                        std::abs(v[i].z - fraction) < 1e-7 && v[i].w == mask,
                    "DX12 shader misread published phase, reflected plane or odd-cell capacity");
        }
        const auto *bytes = static_cast<const char *>(mapped);
        size_t cursor = coarse * 16;
        for (uint32_t i = 0; i < surface.size(); ++i) {
            cursor += surfaceGeometryBytes(c, SurfaceGeometryBuffer(i));
            for (uint32_t k = 0; k < 64; ++k)
                require(uint8_t(bytes[cursor + k]) == 0xcd, "Surface output guard changed");
            cursor += 64;
        }
        if (mode == 3)
            accepted.assign(bytes, bytes + cursor);
        if (mode == 4)
            require(accepted.size() == cursor && !std::memcmp(accepted.data(), bytes, cursor),
                    "Rejected frame changed renderer geometry");
        D3D12_RANGE written{0, 0};
        readback.resource->Unmap(0, &written);
        // Same field and BLAS, with the production full-brick traversal
        // reference instead of tight software bounds. Separate submission keeps
        // each immutable constant-buffer version alive through its GPU use.
        d.begin();
        rayProbe.record(cmd4.Get(), canonical, true);
        d.submit();
        rayProbe.collect(mode);
    }
    // Isolate material-guide interpolation from transport: publish a constant
    // projected MAC field, render once, pause, then retain the stationary cache.
    const auto faceBytes = bufferBytes(c, Faces);
    auto faceUpload = gpu::buffer(d.device.Get(), faceBytes, D3D12_HEAP_TYPE_UPLOAD);
    auto *faceValues = static_cast<DirectX::XMFLOAT4 *>(faceUpload.mapped);
    const uint32_t faceStride = (c.nx + 1) * (c.ny + 1) * (c.nz + 1);
    for (uint32_t i = 0; i < faceStride * 3; ++i)
        faceValues[i] = {float(i / faceStride + 1), 0, 0, 1};
    for (uint32_t motionFrame = 0; motionFrame < 3; ++motionFrame) {
        d.begin();
        if (!motionFrame) {
            gpu::transition(d.cmd.Get(), view.faces, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
            d.cmd->CopyBufferRegion(view.faces, 0, faceUpload.resource.Get(), 0, faceBytes);
            gpu::transition(d.cmd.Get(), view.faces, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        canonical.record(
            cmd4.Get(),
            {view, desc, 3, motionFrame == 0, false, motionFrame == 0 ? .01f : 0.f, surface[0], surface[1]},
            Camera{}, 1.f / 60);
        require(canonical.changedThisFrame == (motionFrame < 2),
                "Phase surface must clear stopped motion once, then retain its stationary cache");
        canonical.recordReadback(d.cmd.Get());
        uint64_t offset = 0;
        for (auto *r : {canonical.fieldResource(), canonical.mapResource()}) {
            gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            d.cmd->CopyBufferRegion(fieldReadback.resource.Get(), offset, r, 0, r->GetDesc().Width);
            offset += r->GetDesc().Width;
            gpu::transition(d.cmd.Get(), r, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        d.submit();
        canonical.collect(frequency);
        void *data = nullptr;
        D3D12_RANGE range{0, size_t(fieldBytes + mapBytes)}, written{0, 0};
        gpu::check(fieldReadback.resource->Map(0, &range, &data), "Phase material guide readback");
        const auto *nodes = static_cast<const DirectX::XMFLOAT4 *>(data);
        const auto *pages = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) + fieldBytes);
        uint32_t wetNodes = 0;
        for (uint32_t brick = 0; brick < canonical.brickGrid.w; ++brick) {
            if (pages[brick] == 0xffffffff)
                continue;
            for (uint32_t n = 0; n < 729; ++n) {
                const auto v = nodes[pages[brick] * 729 + n];
                if (v.x < 0)
                    ++wetNodes;
                if (motionFrame || v.x < 0) {
                    const float seconds = motionFrame ? 0 : .01f;
                    require(std::abs(v.y + seconds) < 2e-8 && std::abs(v.z + 2 * seconds) < 2e-8 &&
                                std::abs(v.w + 3 * seconds) < 2e-8,
                            "Phase MAC material guide is wrong or stale after pause");
                }
            }
        }
        require(wetNodes > 0, "Material-guide fixture did not exercise liquid");
        fieldReadback.resource->Unmap(0, &written);
    }
    std::ostringstream report;
    rayProbe.report(graph);
    solver.report(report);
    require(report.str().find("\"sharedBuffers\":22") != std::string::npos,
            "Engine adapter omitted geometry resources");
    std::cout << "{\"case\":\"surface-dx12-" << (graph ? "graph" : "direct")
              << "\",\"handoffs\":5,\"sharedBuffers\":22,\"canonicalNodes\":true,\"blasBuild\":true"
              << ",\"sharedNodesChecked\":" << sharedNodes << ",\"maxFieldError\":" << maxFieldError
              << ",\"maxContourVolumeError\":" << maxVolumeError << ",\"motionFrames\":3,\"pass\":true}\n";
}
} // namespace
int main(int argc, char **argv) try {
    const auto folder = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::current_path();
    cuda_test::Fixture d(folder);
    run(d, folder, false);
    run(d, folder, true);
    clipping(d, folder);
    cuda_test::movingPlanes(d, folder);
    const auto coarse=cuda_test::curvedSurfaces(d,folder,1),fine=cuda_test::curvedSurfaces(d,folder,2);
    require(fine[0]<coarse[0]*.4&&fine[1]<coarse[1]*.5&&fine[2]<coarse[2]&&fine[3]<coarse[3]*.6,
            "Curved phase geometry/optics did not converge with spatial refinement");
    std::cout<<"{\"case\":\"surface-curved-convergence\",\"hitRatio\":"<<fine[0]/coarse[0]
             <<",\"normalRatio\":"<<fine[1]/coarse[1]<<",\"receiverRatio\":"<<fine[2]/coarse[2]
             <<",\"volumeRatio\":"<<fine[3]/coarse[3]<<",\"pass\":true}\n";
    std::cout << "PASS CUDA surface geometry: engine handoff, canonical field, shared nodes and BLAS; "
                 "not full optical acceptance\n";
} catch (const std::exception &e) {
    std::cerr << "FAIL CUDA surface geometry: " << e.what() << '\n';
    return 1;
}
