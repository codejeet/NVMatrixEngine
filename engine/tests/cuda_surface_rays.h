#pragma once
#include "cuda_fluid_fixture.h"
#include "../src/fluid/fluid_surface.h"
#include "../src/scene.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace lab::cuda_test {
// Bounded hardware oracle over the production surface BLAS. No compute ray
// traversal, alternate field, CPU particle upload or application framework.
class SurfaceRays {
    struct Ray {
        DirectX::XMFLOAT4 originMin, directionMax;
        DirectX::XMUINT4 control;
    };
    struct Result {
        DirectX::XMFLOAT4 hit, media, optics, attenuation, continuation, receiver;
    };
    static_assert(sizeof(Ray) == 48 && sizeof(Result) == 96);
    using V = std::array<double, 3>;
    struct BoxHit {
        bool hit = false;
        double t = 0;
        V normal{};
    };
    Fixture &d;
    std::vector<Ray> rays;
    gpu::Buffer frame, input, output, readback, stats, zeros, cie, instances, tlas, scratch, table;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12StateObject> state;
    uint64_t tested = 0, hits = 0, transmittedPaths = 0, tir = 0, normalChecks = 0;
    double maxHitError = 0, maxNormalError = 0, maxOpticsError = 0, maxContinuationError = 0;
    double maxRawHitError = 0, maxReceiverError = 0, maxReceiverFluxError = 0;
    uint64_t receiverPaths = 0;
    bool curvedChecks = false;
    static V point(const DirectX::XMFLOAT4 &p) {
        return {p.x, p.y, p.z};
    }
    static double dot(V a, V b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    static V at(V o, V v, double t) {
        return {o[0] + v[0] * t, o[1] + v[1] * t, o[2] + v[2] * t};
    }
    static V refractReference(V v, V n, double ni, double nt, double &F) {
        if (dot(v, n) > 0)
            for (double &a : n)
                a = -a;
        const double c = -dot(v, n), eta = ni / nt, sin2 = eta * eta * (1 - c * c);
        F = 1;
        if (sin2 >= 1)
            return {};
        const double ct = std::sqrt(1 - sin2), rs = (ni * c - nt * ct) / (ni * c + nt * ct),
                     rp = (nt * c - ni * ct) / (nt * c + ni * ct);
        F = (rs * rs + rp * rp) * .5;
        V result{};
        for (uint32_t a = 0; a < 3; ++a)
            result[a] = eta * v[a] + (eta * c - ct) * n[a];
        return result;
    }
    static BoxHit box(V o, V v, V lo, V hi, double minimum, double maximum,
                      const std::array<double, 4> &plane = {}) {
        double nearT = -std::numeric_limits<double>::infinity(),
               farT = std::numeric_limits<double>::infinity();
        V enter{}, leave{};
        for (uint32_t a = 0; a < 3; ++a) {
            if (v[a] == 0) {
                if (o[a] < lo[a] || o[a] > hi[a])
                    return {};
                continue;
            }
            double first = (lo[a] - o[a]) / v[a], last = (hi[a] - o[a]) / v[a];
            V n0{}, n1{};
            n0[a] = -1;
            n1[a] = 1;
            if (first > last) {
                std::swap(first, last);
                std::swap(n0, n1);
            }
            if (first > nearT) {
                nearT = first;
                enter = n0;
            }
            if (last < farT) {
                farT = last;
                leave = n1;
            }
        }
        V normal{plane[0], plane[1], plane[2]};
        const double norm = std::sqrt(dot(normal, normal));
        if (norm > 0) {
            const double denominator = dot(normal, v), distance = plane[3] - dot(normal, o);
            if (denominator == 0) {
                if (distance < 0)
                    return {};
            } else {
                const double t = distance / denominator;
                for (double &component : normal)
                    component /= norm;
                if (denominator < 0 && t > nearT) {
                    nearT = t;
                    enter = normal;
                }
                if (denominator > 0 && t < farT) {
                    farT = t;
                    leave = normal;
                }
            }
        }
        if (nearT > farT)
            return {};
        if (nearT >= minimum && nearT <= maximum)
            return {true, nearT, enter};
        if (farT >= minimum && farT <= maximum)
            return {true, farT, leave};
        return {};
    }
    static bool inside(V p, V lo, V hi) {
        return p[0] > lo[0] && p[0] < hi[0] && p[1] > lo[1] && p[1] < hi[1] && p[2] > lo[2] && p[2] < hi[2];
    }
    static double curvedPhi(V p, const DirectX::XMFLOAT4 &shape) {
        return p[1] - shape.y -
               shape.w * ((p[0] - shape.x) * (p[0] - shape.x) + (p[2] - shape.z) * (p[2] - shape.z));
    }
    static V curvedNormal(V p, const DirectX::XMFLOAT4 &shape) {
        V n{-2 * shape.w * (p[0] - shape.x), 1, -2 * shape.w * (p[2] - shape.z)};
        const double length = std::sqrt(dot(n, n));
        for (double &v : n)
            v /= length;
        return n;
    }
    static BoxHit curved(V o, V v, const DirectX::XMFLOAT4 &shape, double minimum, double maximum) {
        BoxHit best{};
        best.t = maximum;
        const V hi{2.5, 3.5, 1.5};
        auto accept = [&](double t, V n, bool wall) {
            if (t < minimum || t > best.t)
                return;
            const V p = at(o, v, t);
            for (uint32_t a = 0; a < 3; ++a)
                if (p[a] < -1e-9 || p[a] > hi[a] + 1e-9)
                    return;
            if (wall && curvedPhi(p, shape) > 1e-10)
                return;
            best = {true, t, wall ? n : curvedNormal(p, shape)};
        };
        for (uint32_t a = 0; a < 3; ++a)
            if (v[a] != 0) {
                V n{};
                n[a] = -1;
                accept(-o[a] / v[a], n, true);
                n[a] = 1;
                accept((hi[a] - o[a]) / v[a], n, true);
            }
        const double A = -shape.w * (v[0] * v[0] + v[2] * v[2]),
                     B = v[1] - 2 * shape.w * ((o[0] - shape.x) * v[0] + (o[2] - shape.z) * v[2]),
                     C = curvedPhi(o, shape);
        if (A == 0) {
            if (B != 0)
                accept(-C / B, {}, false);
        } else {
            const double disc = B * B - 4 * A * C;
            if (disc >= 0) {
                const double q = -.5 * (B + std::copysign(std::sqrt(disc), B));
                if (q == 0)
                    accept(-B / (2 * A), {}, false);
                else {
                    accept(q / A, {}, false);
                    accept(C / q, {}, false);
                }
            }
        }
        return best;
    }
    void add(V o, V direction, uint32_t mask = 8) {
        double length = std::sqrt(dot(direction, direction));
        Ray r{{float(o[0]), float(o[1]), float(o[2]), .0002f},
              {float(direction[0] / length), float(direction[1] / length), float(direction[2] / length), 100},
              {mask, 0, 0, 0}};
        rays.push_back(r);
    }

  public:
    SurfaceRays(Fixture &fixture, ID3D12Device5 *device, const std::filesystem::path &folder) : d(fixture) {
        // World-space seams occur at x/z=1 for this padded 0.25m node lattice.
        for (double x : {.375, .75, .9999, 1., 1.0001, 1.5, 2.125})
            for (double z : {.375, .75, .9999, 1., 1.0001, 1.125}) {
                add({x, 4.5, z}, {0, -1, 0});
                add({x, -1.25, z}, {0, 1, 0});
                add({x, 4.5, z}, {.03, -1, -.02});
            }
        for (double y : {.375, .625, 1., 1.25, 1.5, 1.75, 2., 2.625, 3.25, 3.5, 3.75}) {
            add({-.75, y, .75}, {1, 0, 0});
            add({3.25, y, .75}, {-1, 0, 0});
            add({1.25, y, -.75}, {0, 0, 1});
            add({1.25, y, 2.25}, {0, 0, -1});
        }
        for (double y : {.625, 2.625})
            for (double x : {.5, 1.25}) {
                add({x, y, .75}, {0, 1, 0});
                add({x, y, .75}, {0, -1, 0});
                add({x, y, .75}, {.5, std::sqrt(.75), 0});
                add({x, y, .75}, {std::sqrt(.75), .5, 0});
                add({x, y, .75}, {1, .002, .01});
            }
        add({.4, 1.251, .75}, {1, -.001, 0});
        add({.4, 3.501, .75}, {1, -.001, 0});
        add({1.25, 4.5, .75}, {0, -1, 0}, 0xff);
        add({1.25, 4.5, .75}, {0, -1, 0}, 2);
        add({3.5, 4.5, 2.5}, {0, -1, 0});
        auto make = [&](uint64_t bytes, const wchar_t *name) {
            return gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name);
        };
        frame = gpu::buffer(device, 768, D3D12_HEAP_TYPE_UPLOAD);
        input = gpu::buffer(device, rays.size() * sizeof(Ray), D3D12_HEAP_TYPE_UPLOAD);
        std::memcpy(input.mapped, rays.data(), rays.size() * sizeof(Ray));
        output = make(rays.size() * sizeof(Result), L"Phase surface / DXR hit and medium results");
        stats = make(256, L"Phase surface / production intersection diagnostics");
        zeros = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
        std::memset(zeros.mapped, 0, 256);
        readback = gpu::buffer(device, rays.size() * sizeof(Result) + 256, D3D12_HEAP_TYPE_READBACK,
                               D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        cie = gpu::buffer(device, 802 * 16, D3D12_HEAP_TYPE_UPLOAD);
        auto *spectrum = static_cast<DirectX::XMFLOAT4 *>(cie.mapped);
        std::fill(spectrum, spectrum + 802, DirectX::XMFLOAT4{});
        for (uint32_t i = 0; i <= 400; ++i)
            spectrum[401 + i] = {1.333f, .05f + .0005f * float(400 - i), 0, 0};
        instances = gpu::buffer(device, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        require(info.ResultDataMaxSizeInBytes && info.ScratchDataSizeInBytes,
                "DXR surface prebuild unsupported");
        tlas = gpu::buffer(device, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        scratch = make(info.ScratchDataSizeInBytes, L"Phase surface / test TLAS scratch");
        D3D12_ROOT_PARAMETER p[8]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        uint32_t registers[]{0, 3, 4, 5, 9};
        for (uint32_t i = 0; i < 5; ++i) {
            p[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[i + 1].Descriptor.ShaderRegister = registers[i];
        }
        p[6].ParameterType = p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[6].Descriptor.ShaderRegister = 10;
        p[7].Descriptor.ShaderRegister = 24;
        D3D12_ROOT_SIGNATURE_DESC rd{8, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        gpu::check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                   "Surface ray root serialize");
        gpu::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root)),
                   "Surface ray root");
        auto code = gpu::bytes(folder / "shaders/CudaSurfaceRays.dxil");
        D3D12_DXIL_LIBRARY_DESC library{{code.data(), code.size()}, 0, nullptr};
        D3D12_GLOBAL_ROOT_SIGNATURE global{root.Get()};
        D3D12_RAYTRACING_SHADER_CONFIG config{16, 8};
        D3D12_RAYTRACING_PIPELINE_CONFIG recursion{1};
        D3D12_HIT_GROUP_DESC hit{L"FluidHit", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE, nullptr,
                                 L"FluidClosest", L"FluidIntersection"};
        D3D12_STATE_SUBOBJECT sub[]{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
                                    {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
                                    {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &config},
                                    {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &recursion},
                                    {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hit}};
        D3D12_STATE_OBJECT_DESC sd{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 5, sub};
        gpu::check(device->CreateStateObject(&sd, IID_PPV_ARGS(&state)), "Surface ray pipeline");
        Microsoft::WRL::ComPtr<ID3D12StateObjectProperties> properties;
        gpu::check(state.As(&properties), "Surface ray identifiers");
        table = gpu::buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
        std::memset(table.mapped, 0, 256);
        const wchar_t *exports[]{L"SurfaceRaygen", L"ProbeMiss", L"FluidHit"};
        for (uint32_t i = 0; i < 3; ++i) {
            auto *id = properties->GetShaderIdentifier(exports[i]);
            require(id != nullptr, "Missing surface ray export");
            std::memcpy(static_cast<char *>(table.mapped) + 64 * i, id, 32);
        }
    }
    void record(ID3D12GraphicsCommandList4 *cmd, const FluidSurface &surface, bool fullBounds = false) {
        gpu::Event label(cmd, L"Validation / phase surface DispatchRays and dielectric continuation");
        Constants c{};
        c.fluidMinimumSpacing = surface.minimumSpacing;
        c.fluidBricks = surface.brickGrid;
        c.fluidState = {1, 0, 0, 17u | (fullBounds ? 64u : 0u)};
        c.dimensions.w = uint32_t(rays.size());
        std::memcpy(frame.mapped, &c, sizeof(c));
        auto *instance = static_cast<D3D12_RAYTRACING_INSTANCE_DESC *>(instances.mapped);
        *instance = {};
        instance->Transform[0][0] = instance->Transform[1][1] = instance->Transform[2][2] = 1;
        instance->InstanceID = 1;
        instance->InstanceMask = 8;
        instance->AccelerationStructure = surface.accelerationStructure();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.NumDescs = 1;
        build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.InstanceDescs = instances.resource->GetGPUVirtualAddress();
        build.DestAccelerationStructureData = tlas.resource->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch.resource->GetGPUVirtualAddress();
        cmd->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        gpu::uav(cmd, tlas.resource.Get());
        gpu::transition(cmd, stats.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyBufferRegion(stats.resource.Get(), 0, zeros.resource.Get(), 0, 256);
        gpu::transition(cmd, stats.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, frame.resource->GetGPUVirtualAddress());
        ID3D12Resource *srvs[]{tlas.resource.Get(), cie.resource.Get(), surface.fieldResource(),
                               surface.mapResource(), input.resource.Get()};
        for (uint32_t i = 0; i < 5; ++i)
            cmd->SetComputeRootShaderResourceView(i + 1, srvs[i]->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(6, stats.resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(7, output.resource->GetGPUVirtualAddress());
        cmd->SetPipelineState1(state.Get());
        const auto base = table.resource->GetGPUVirtualAddress();
        D3D12_DISPATCH_RAYS_DESC dispatch{};
        dispatch.RayGenerationShaderRecord = {base, 32};
        dispatch.MissShaderTable = {base + 64, 32, 32};
        dispatch.HitGroupTable = {base + 128, 32, 32};
        dispatch.Width = uint32_t(rays.size());
        dispatch.Height = dispatch.Depth = 1;
        cmd->DispatchRays(&dispatch);
        gpu::uav(cmd);
        d.copyOut(output.resource.Get(), readback.resource.Get(), rays.size() * sizeof(Result), 0);
        d.copyOut(stats.resource.Get(), readback.resource.Get(), 256, rays.size() * sizeof(Result));
    }
    void collect(uint32_t mode, const std::array<double, 4> &plane = {}) {
        void *data = nullptr;
        D3D12_RANGE range{0, rays.size() * sizeof(Result) + 256}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &data), "Surface ray readback");
        const auto *values = static_cast<const Result *>(data);
        const auto *diagnostics = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) +
                                                                     rays.size() * sizeof(Result));
        require(diagnostics[29] == 0, "Production fluid intersection exhausted DDA steps");
        require(mode == 2 ? diagnostics[31] == 0 : diagnostics[31] > 0,
                "DXR traversal diagnostics did not match empty/nonempty geometry");
        V lo{0, mode == 1 ? 1.75 : 0, 0}, hi{2.5, mode == 1 ? 3.5 : 1.25, 1.5};
        const V planeNormal{plane[0], plane[1], plane[2]};
        const double planeNorm = std::sqrt(dot(planeNormal, planeNormal));
        if (planeNorm > 0)
            hi[1] = 3.5;
        for (uint32_t i = 0; i < rays.size(); ++i) {
            const Ray &ray = rays[i];
            const Result &r = values[i];
            V o = point(ray.originMin), v = point(ray.directionMax);
            auto expected = box(o, v, lo, hi, ray.originMin.w, ray.directionMax.w, plane);
            if (mode == 2 || !(ray.control.x & 8))
                expected.hit = false;
            const bool originInside =
                mode != 2 && inside(o, lo, hi) && (planeNorm == 0 || dot(planeNormal, o) < plane[3]);
            const auto *scalars = reinterpret_cast<const float *>(&r);
            for (uint32_t k = 0; k < sizeof(Result) / 4; ++k)
                require(std::isfinite(scalars[k]), "Nonfinite DXR surface guide");
            if ((r.hit.x >= 0) != expected.hit) {
                std::cerr << "DXR mode " << mode << " ray " << i << " got " << r.hit.x << " expected "
                          << (expected.hit ? expected.t : -1) << '\n';
                throw std::runtime_error("Phase DXR hit/miss differs from independent box");
            }
            require(r.media.w == (expected.hit ? 1.f : 0.f),
                    "Procedural visibility differs from closest-hit result");
            require(r.media.x == (originInside ? 8.f : 0.f),
                    "Phase ambient medium differs from geometric interior");
            ++tested;
            if (!expected.hit)
                continue;
            ++hits;
            double error = std::abs(r.hit.x - expected.t);
            maxHitError = std::max(maxHitError, error);
            if (error > 5e-4) {
                std::cerr << "DXR distance mode " << mode << " ray " << i << " error " << error << " got "
                          << r.hit.x << " expected " << expected.t << '\n';
                throw std::runtime_error("Phase DXR hit distance error");
            }
            V n{r.hit.y, r.hit.z, r.hit.w}, p = at(o, v, expected.t);
            require(std::abs(dot(n, n) - 1) < 2e-5, "Nonunit phase surface normal");
            bool smooth = true;
            for (uint32_t a = 0; a < 3; ++a)
                if (std::abs(expected.normal[a]) < 1 - 1e-10 && std::min(p[a] - lo[a], hi[a] - p[a]) < .15)
                    smooth = false;
            if (planeNorm > 0 && std::abs(dot(expected.normal, planeNormal) / planeNorm) < 1 - 1e-10 &&
                std::abs(dot(planeNormal, p) - plane[3]) / planeNorm < .15)
                smooth = false;
            // The production normal deliberately filters nonunique sharp box
            // corners. Compare analytic normals only on planar face interiors.
            if (!smooth)
                continue;
            ++normalChecks;
            double normalError = 0;
            for (uint32_t a = 0; a < 3; ++a)
                normalError += std::pow(n[a] - expected.normal[a], 2);
            normalError = std::sqrt(normalError);
            maxNormalError = std::max(maxNormalError, normalError);
            if (normalError >= .005) {
                std::cerr << "DXR normal mode " << mode << " ray " << i << " error " << normalError << " at "
                          << p[0] << ',' << p[1] << ',' << p[2] << " got " << n[0] << ',' << n[1] << ','
                          << n[2] << " expected " << expected.normal[0] << ',' << expected.normal[1] << ','
                          << expected.normal[2] << '\n';
                throw std::runtime_error("Phase surface normal seam");
            }
            require(r.media.y == 0 && r.media.z == 8,
                    "Surface normal offsets do not straddle the medium boundary");
            const bool entering = dot(expected.normal, v) < 0;
            const double ni = entering ? 1 : double(1.333f), nt = entering ? double(1.333f) : 1;
            const double cosine = std::abs(dot(expected.normal, v)), eta = ni / nt,
                         sin2 = eta * eta * (1 - cosine * cosine);
            double F = 1;
            V refracted{};
            if (sin2 < 1) {
                double ct = std::sqrt(1 - sin2), rs = (ni * cosine - nt * ct) / (ni * cosine + nt * ct),
                       rp = (nt * cosine - ni * ct) / (nt * cosine + ni * ct);
                F = (rs * rs + rp * rp) * .5;
                for (uint32_t a = 0; a < 3; ++a)
                    refracted[a] = eta * v[a] + (eta * cosine - ct) *
                                                    (entering ? expected.normal[a] : -expected.normal[a]);
            } else
                ++tir;
            double opticsError = std::abs(r.optics.x - F);
            for (uint32_t a = 0; a < 3; ++a)
                opticsError = std::max(opticsError, std::abs((&r.optics.y)[a] - refracted[a]));
            maxOpticsError = std::max(maxOpticsError, opticsError);
            require(opticsError < 2e-5, "Fresnel/Snell/TIR differs from independent oracle");
            require(r.attenuation.w == (entering ? 8.f : 0.f),
                    "Production dielectric interface chooses wrong next medium");
            constexpr double sigma[]{.135, .165, .21};
            for (uint32_t a = 0; a < 3; ++a)
                require(std::abs((&r.attenuation.x)[a] -
                                 std::exp(-(originInside ? sigma[a] : 0) * expected.t)) < 2e-5,
                        "Beer attenuation uses wrong water distance");
            if (entering && sin2 < 1) {
                auto exit = box(p, refracted, lo, hi, .0002, 100, plane);
                require(exit.hit, "Missing analytic transmitted boundary");
                ++transmittedPaths;
                double distanceError = std::abs(r.continuation.x - exit.t);
                maxContinuationError = std::max(maxContinuationError, distanceError);
                require(r.continuation.w == 1 && r.continuation.z == 0 && distanceError < 8e-4,
                        "Transmitted ray missed/changed the exit boundary");
                require(std::abs(r.continuation.y - std::exp(-sigma[1] * exit.t)) < 1e-4,
                        "Transmitted water segment attenuation mismatch");
            }
        }
        readback.resource->Unmap(0, &written);
    }
    void collectCurved(const DirectX::XMFLOAT4 &shape) {
        curvedChecks = true;
        void *data = nullptr;
        D3D12_RANGE range{0, rays.size() * sizeof(Result) + 256}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &data), "Curved surface ray readback");
        const auto *values = static_cast<const Result *>(data);
        const auto *diagnostics = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) +
                                                                     rays.size() * sizeof(Result));
        require(diagnostics[29] == 0 && diagnostics[31] > 0, "Curved surface DDA diagnostics");
        for (uint32_t i = 0; i < rays.size(); ++i) {
            const auto &ray = rays[i];
            const auto &r = values[i];
            const V o = point(ray.originMin), v = point(ray.directionMax);
            auto expected = curved(o, v, shape, ray.originMin.w, ray.directionMax.w);
            if (!(ray.control.x & 8))
                expected.hit = false;
            for (uint32_t k = 0; k < sizeof(Result) / 4; ++k)
                require(std::isfinite(reinterpret_cast<const float *>(&r)[k]), "Nonfinite curved guide");
            require((r.hit.x >= 0) == expected.hit && r.media.w == (expected.hit ? 1.f : 0.f),
                    "Curved DXR visibility mismatch");
            const bool originInside = inside(o, {0, 0, 0}, {2.5, 3.5, 1.5}) && curvedPhi(o, shape) < 0;
            require(r.media.x == (originInside ? 8.f : 0.f), "Curved ambient medium mismatch");
            ++tested;
            if (!expected.hit)
                continue;
            ++hits;
            const V p = at(o, v, r.hit.x), n{r.hit.y, r.hit.z, r.hit.w};
            const double incidence = std::abs(dot(expected.normal, v));
            maxRawHitError = std::max(maxRawHitError, std::abs(r.hit.x - expected.t));
            const double error = std::abs(r.hit.x - expected.t) * std::max(incidence, .05);
            maxHitError = std::max(maxHitError, error);
            if (error > 5e-4) {
                std::cerr << "Curved hit ray " << i << " projected error " << error << '\n';
                throw std::runtime_error("Curved phase position error");
            }
            require(std::abs(dot(n, n) - 1) < 2e-5, "Nonunit curved normal");
            bool smooth = true;
            for (uint32_t a = 0; a < 3; ++a)
                if (std::abs(expected.normal[a]) < 1 - 1e-10 &&
                    std::min(p[a], V{2.5, 3.5, 1.5}[a] - p[a]) < .15)
                    smooth = false;
            const bool surface = std::abs(curvedPhi(at(o, v, expected.t), shape)) < 1e-8;
            if (!surface && std::abs(curvedPhi(p, shape)) < .15)
                smooth = false;
            if (!smooth)
                continue;
            ++normalChecks;
            const V analytic = surface ? curvedNormal(p, shape) : expected.normal;
            double normalError = 0;
            for (uint32_t a = 0; a < 3; ++a)
                normalError += std::pow(n[a] - analytic[a], 2);
            normalError = std::sqrt(normalError);
            maxNormalError = std::max(maxNormalError, normalError);
            if (normalError >= .005) {
                std::cerr << "Curved normal ray " << i << " error " << normalError << '\n';
                throw std::runtime_error("Curved phase normal error");
            }
            require(r.media.y == 0 && r.media.z == 8, "Curved interface offsets fail medium classification");
            const bool entering = dot(n, v) < 0;
            const double ni = entering ? 1 : double(1.333f), nt = entering ? double(1.333f) : 1;
            // Geometry is checked independently above. This separate optical
            // oracle checks the dielectric law for the actual reconstructed
            // normal, not an unattainable exact smooth normal on a finite grid.
            const double cosine = std::abs(dot(n, v)), eta = ni / nt,
                         sin2 = eta * eta * (1 - cosine * cosine);
            double F = 1;
            V transmitted{};
            if (sin2 < 1) {
                const double ct = std::sqrt(1 - sin2), rs = (ni * cosine - nt * ct) / (ni * cosine + nt * ct),
                             rp = (nt * cosine - ni * ct) / (nt * cosine + ni * ct);
                F = (rs * rs + rp * rp) * .5;
                for (uint32_t a = 0; a < 3; ++a)
                    transmitted[a] = eta * v[a] + (eta * cosine - ct) * (entering ? n[a] : -n[a]);
            } else
                ++tir;
            double opticsError = std::abs(r.optics.x - F);
            for (uint32_t a = 0; a < 3; ++a)
                opticsError = std::max(opticsError, std::abs((&r.optics.y)[a] - transmitted[a]));
            maxOpticsError = std::max(maxOpticsError, opticsError);
            require(opticsError < 2e-5, "Curved Snell/Fresnel/TIR law mismatch");
            require(r.attenuation.w == (entering ? 8.f : 0.f), "Curved interface next medium mismatch");
            constexpr double sigma[]{.135, .165, .21};
            for (uint32_t a = 0; a < 3; ++a)
                require(std::abs((&r.attenuation.x)[a] - std::exp(-(originInside ? sigma[a] : 0) * r.hit.x)) <
                            2e-5,
                        "Curved Beer mismatch");
            if (entering && sin2 < 1) {
                const auto exit = curved(p, transmitted, shape, .0002, 100);
                ++transmittedPaths;
                const double exitError = std::abs(r.continuation.x - exit.t) *
                                         std::max(std::abs(dot(exit.normal, transmitted)), .05);
                maxContinuationError = std::max(maxContinuationError, exitError);
                require(exit.hit && r.continuation.w == 1 && r.continuation.z == 0 && exitError < 8e-4,
                        "Curved transmitted boundary mismatch");
                require(std::abs(r.continuation.y - std::exp(-sigma[1] * r.continuation.x)) < 1e-4,
                        "Curved transmitted attenuation mismatch");
                const V entryPoint = at(o, v, expected.t);
                double entryF = 0, exitF = 0;
                const V idealWater = refractReference(v, expected.normal, 1, double(1.333f), entryF);
                const auto idealExit = curved(entryPoint, idealWater, shape, .0002, 100);
                require(idealExit.hit, "Missing ideal optical continuation");
                const V exitPoint = at(entryPoint, idealWater, idealExit.t);
                const V idealAir = refractReference(idealWater, idealExit.normal, double(1.333f), 1, exitF);
                bool receives = idealAir[1] < 0 && exitPoint[1] > -.5;
                if (receives)
                    receives =
                        !curved(exitPoint, idealAir, shape, .0002, (-.5 - exitPoint[1]) / idealAir[1] - .0002)
                             .hit;
                require((r.receiver.w >= 0) == receives, "Curved receiver visibility/TIR disagreement");
                if (receives) {
                    ++receiverPaths;
                    const V target = at(exitPoint, idealAir, (-.5 - exitPoint[1]) / idealAir[1]);
                    double positionError = 0;
                    for (uint32_t a = 0; a < 3; ++a)
                        positionError += std::pow((&r.receiver.x)[a] - target[a], 2);
                    positionError = std::sqrt(positionError);
                    maxReceiverError = std::max(maxReceiverError, positionError);
                    const double flux = (1 - entryF) * (1 - exitF) * std::exp(-sigma[1] * idealExit.t);
                    const double fluxError = std::abs(r.receiver.w - flux);
                    maxReceiverFluxError = std::max(maxReceiverFluxError, fluxError);
                    require(positionError < .003 && fluxError < 2e-4, "Curved receiver focusing/flux error");
                }
            }
        }
        readback.resource->Unmap(0, &written);
    }
    std::array<double,3> curvedMetrics() const {return {maxHitError,maxNormalError,maxReceiverError};}
    void report(bool graph, const char *scenario = nullptr) const {
        require(tested && hits && normalChecks && transmittedPaths && tir,
                "DXR surface fixture missed required path families");
        std::cout << "{\"case\":\"surface-dxr-" << (scenario ? scenario : (graph ? "graph" : "direct"))
                  << "\",\"raysChecked\":" << tested << ",\"hits\":" << hits
                  << ",\"normalChecks\":" << normalChecks << ",\"transmittedPaths\":" << transmittedPaths
                  << ",\"tir\":" << tir << ",\"maxHitError\":" << maxHitError
                  << ",\"maxNormalError\":" << maxNormalError << ",\"maxOpticsError\":" << maxOpticsError
                  << ",\"maxContinuationError\":" << maxContinuationError << ",\"pass\":true}\n";
        if (curvedChecks) {
            require(receiverPaths > 0, "Curved fixture omitted receiver paths");
            std::cout << "{\"case\":\"surface-receiver-" << (scenario?scenario:"curved") << "\",\"paths\":" << receiverPaths
                      << ",\"maxPositionError\":" << maxReceiverError
                      << ",\"maxUnitFluxError\":" << maxReceiverFluxError
                      << ",\"maxRawHitError\":" << maxRawHitError
                      << ",\"projectedHitIncidenceFloor\":0.05,\"pass\":true}\n";
        }
    }
};
} // namespace lab::cuda_test
