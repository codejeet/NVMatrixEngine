#pragma once
#include "cuda_surface_rays.h"
#include <map>

namespace lab::cuda_test {
// Independent inclusion/exclusion integral over the physical domain. This is
// not the sorted piecewise CDF/inverse used by the reconstruction shader.
inline double planeVolume(const DirectX::XMFLOAT4 &plane) {
    const double extent[]{2.5, 3.5, 1.5};
    long double alpha = plane.w, denominator = 1;
    std::array<long double, 3> support{};
    uint32_t dimensions = 0;
    for (uint32_t a = 0; a < 3; ++a) {
        const double n = (&plane.x)[a];
        if (n < 0)
            alpha -= n * extent[a];
        if (n != 0) {
            support[dimensions++] = std::abs(n) * extent[a];
            denominator *= support[dimensions - 1];
        }
    }
    long double integral = 0;
    for (uint32_t mask = 0; mask < (1u << dimensions); ++mask) {
        long double t = alpha;
        int sign = 1;
        for (uint32_t a = 0; a < dimensions; ++a)
            if (mask & (1u << a)) {
                t -= support[a];
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
        denominator *= a;
    return std::clamp(double(integral / denominator), 0., 1.) * 2.5 * 3.5 * 1.5;
}
// Renderer reconstruction oracle: exact GPU-manufactured PLIC descriptors.
// This does not certify the Solver's separate normal/plane estimation.
inline void movingPlanes(Fixture &d, const std::filesystem::path &folder) {
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd;
    gpu::check(d.device.As(&device), "Plane surface device");
    gpu::check(d.cmd.As(&cmd), "Plane surface command list");
    FluidSystemDesc desc{};
    desc.minimum = {0, 0, 0};
    desc.maximum = {2.5f, 3.5f, 1.5f};
    desc.gridCellSize = .5f;
    desc.maxParticles = 1;
    FluidSurface surface(device.Get(), folder, desc);
    SurfaceRays rays(d, device.Get(), folder);
    const uint64_t fieldBytes = surface.fieldResource()->GetDesc().Width;
    const uint64_t mapBytes = surface.mapResource()->GetDesc().Width;
    auto readback = gpu::buffer(d.device.Get(), fieldBytes + mapBytes, D3D12_HEAP_TYPE_READBACK,
                                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    uint64_t frequency = 0, sharedNodes = 0, bandNodes = 0;
    gpu::check(d.queue->GetTimestampFrequency(&frequency), "Plane surface timestamps");
    double maxFieldError = 0, maxVolumeError = 0;
    auto phase = d.make(24 * 16), planes = d.make(24 * 32), dummy = d.make(256),
         faces = d.make(6 * 8 * 4 * 3 * 16);
    auto zero = gpu::buffer(d.device.Get(), faces.resource->GetDesc().Width, D3D12_HEAP_TYPE_UPLOAD);
    std::memset(zero.mapped, 0, size_t(zero.resource->GetDesc().Width));
    FluidGpuView view{};
    view.grid = {5, 7, 3, 105};
    view.particles = view.offsets = view.indices = view.previousPositions = dummy.resource.Get();
    view.meshPhi = view.interior = view.interiorTotals = dummy.resource.Get();
    view.faces = faces.resource.Get();
    view.colliderAddress = dummy.resource->GetGPUVirtualAddress();
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    for (uint32_t i = 1; i < 3; ++i) {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, rootError;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> seed;
    gpu::check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &rootError),
               "Moving plane root serialize");
    gpu::check(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root)),
               "Moving plane root");
    auto code = gpu::bytes(folder / "shaders/CudaSurfaceGeometrySeed.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = root.Get();
    ps.CS = {code.data(), code.size()};
    gpu::check(d.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&seed)), "Moving plane seed");
    std::vector<DirectX::XMFLOAT4> sequence{{0, 1, 0, 1.325f},    {0, 1, 0, 1.3375f},
                                            {.15f, 1, .1f, 1.5f}, {-.25f, 1, .12f, 1.35f},
                                            {.6f, 1, .45f, 2.1f}, {1, 1, 1, 2.75f}};
    for (uint32_t sign = 0; sign < 8; ++sign) {
        DirectX::XMFLOAT4 p{sign & 1 ? -.25f : .25f, sign & 2 ? -1.f : 1.f, sign & 4 ? -.12f : .12f, 0};
        p.w = p.x * 1.25f + p.y * 1.75f + p.z * .75f + .037f;
        sequence.push_back(p);
    }
    for (uint32_t step = 0; step < 9; ++step)
        sequence.push_back({.15f, 1, .1f, 1.31f + .013f * float(step)});
    uint32_t frame = 0;
    for (const auto &p : sequence) {
        d.begin();
        if (!frame) {
            gpu::transition(cmd.Get(), faces.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(faces.resource.Get(), 0, zero.resource.Get(), 0,
                                  faces.resource->GetDesc().Width);
            gpu::transition(cmd.Get(), faces.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        const uint32_t grid[]{5, 7, 3, 0};
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRoot32BitConstants(0, 4, grid, 0);
        cmd->SetComputeRoot32BitConstants(0, 4, &p, 4);
        cmd->SetComputeRootUnorderedAccessView(1, phase.resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2, planes.resource->GetGPUVirtualAddress());
        cmd->SetPipelineState(seed.Get());
        cmd->Dispatch(1, 1, 1);
        gpu::uav(cmd.Get());
        ++frame;
        surface.record(cmd.Get(),
                       {view, desc, frame, true, frame == 1, 0, phase.resource.Get(), planes.resource.Get()},
                       Camera{}, 1.f / 60);
        rays.record(cmd.Get(), surface);
        surface.recordReadback(cmd.Get());
        uint64_t offset = 0;
        for (auto *resource : {surface.fieldResource(), surface.mapResource()}) {
            gpu::transition(cmd.Get(), resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(), offset, resource, 0, resource->GetDesc().Width);
            offset += resource->GetDesc().Width;
            gpu::transition(cmd.Get(), resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        d.submit();
        rays.collect(0, {p.x, p.y, p.z, p.w});
        surface.collect(frequency);
        void *data = nullptr;
        D3D12_RANGE range{0, size_t(fieldBytes + mapBytes)}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &data), "Plane field readback");
        const auto *nodes = static_cast<const DirectX::XMFLOAT4 *>(data);
        const auto *pages = reinterpret_cast<const uint32_t *>(static_cast<const char *>(data) + fieldBytes);
        const auto bricks = surface.brickGrid;
        require(pages[bricks.w * 18 + 1] == 1, "Missing canonical phase domain metadata");
        std::map<std::array<uint32_t, 3>, DirectX::XMFLOAT4> seen;
        const double norm = std::sqrt(double(p.x) * p.x + double(p.y) * p.y + double(p.z) * p.z);
        for (uint32_t brick = 0; brick < bricks.w; ++brick) {
            const uint32_t slot = pages[brick];
            if (slot == 0xffffffff)
                continue;
            require(slot < bricks.w, "Plane field page overflow");
            for (uint32_t node = 0; node < 729; ++node) {
                const std::array<uint32_t, 3> key{brick % bricks.x * 8 + node % 9,
                                                  brick / bricks.x % bricks.y * 8 + node / 9 % 9,
                                                  brick / (bricks.x * bricks.y) * 8 + node / 81};
                const auto value = nodes[slot * 729 + node];
                require(std::isfinite(value.x) && value.y == 0 && value.z == 0 && value.w == 0,
                        "Nonfinite field/stale plane motion");
                auto [entry, inserted] = seen.emplace(key, value);
                if (!inserted) {
                    ++sharedNodes;
                    require(!std::memcmp(&value, &entry->second, sizeof(value)), "Moving plane brick seam");
                }
                const double domain[]{2.5, 3.5, 1.5};
                double phi = -p.w, minimum = -p.w, maximum = -p.w;
                bool interior = true;
                for (uint32_t a = 0; a < 3; ++a) {
                    double x = key[a] * .25 - 1, n = (&p.x)[a];
                    phi += n * x;
                    interior = interior && x >= 0 && x <= domain[a];
                    const double first = n * std::max(0., x - .5), last = n * std::min(domain[a], x + .5);
                    minimum += std::min(first, last);
                    maximum += std::max(first, last);
                }
                if (interior && std::abs(phi / norm) < .25 && minimum < 0 && maximum > 0) {
                    ++bandNodes;
                    const double error = std::abs(value.x - phi / norm);
                    maxFieldError = std::max(maxFieldError, error);
                    require(error < 1e-6, "Moving phase field lost affine plane precision");
                }
            }
        }
        readback.resource->Unmap(0, &written);
        uint32_t cutCells = 0;
        for (uint32_t z = 0; z < 6; ++z)
            for (uint32_t y = 0; y < 14; ++y)
                for (uint32_t x = 0; x < 10; ++x) {
                    double low = 1e30, high = -1e30;
                    for (uint32_t k = 0; k < 8; ++k) {
                        double phi = p.x * (x + (k & 1)) * .25 + p.y * (y + ((k >> 1) & 1)) * .25 +
                                     p.z * (z + (k >> 2)) * .25 - p.w;
                        low = std::min(low, phi);
                        high = std::max(high, phi);
                    }
                    if (low < 0 && high > 0)
                        ++cutCells;
                }
        const double error = std::abs(surface.renderedVolume - planeVolume(p));
        maxVolumeError = std::max(maxVolumeError, error);
        // The existing diagnostic rounds each cut cell to 1/256 of its volume.
        // This is its derived quantization bound, not a relaxed geometry gate.
        require(error <= cutCells * (.25 * .25 * .25 / 512) + 2e-6,
                "Oblique contour volume exceeds diagnostic quantization");
        d.begin();
        rays.record(cmd.Get(), surface, true);
        d.submit();
        rays.collect(0, {p.x, p.y, p.z, p.w});
    }
    require(sharedNodes > 0 && bandNodes > 0, "Plane fixture did not check shared/near-interface nodes");
    rays.report(false, "moving-planes");
    std::cout << "{\"case\":\"surface-moving-plane-field\",\"frames\":" << frame
              << ",\"sharedNodes\":" << sharedNodes << ",\"bandNodes\":" << bandNodes
              << ",\"maxFieldError\":" << maxFieldError << ",\"maxContourVolumeError\":" << maxVolumeError
              << ",\"pass\":true}\n";
}
} // namespace lab::cuda_test
