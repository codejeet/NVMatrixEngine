#include "../src/fluid/fluid_carrier_projection.h"
#include <dxgi1_6.h>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
using namespace lab;
using namespace DirectX;
using Microsoft::WRL::ComPtr;
struct Fixture {
    const char *name;
    std::array<float, 3> volume;
    double inlet, air, expected;
    bool closing = false, reject = false;
};
int main(int argc, char **argv) try {
    if (argc != 2)
        throw std::runtime_error("Pass the lab runtime folder containing shaders and D3D12");
    for (uint32_t invalidCycles : {0u, 5u}) {
        bool rejected = false;
        try {
            // The public subsystem API must reject invalid budgets before
            // touching the device or allocating GPU resources.
            FluidCarrierProjection invalid(nullptr, argv[1], {}, {}, {}, true, invalidCycles);
        } catch (const std::runtime_error &error) {
            rejected = std::string(error.what()) == "Capacity pressure cycles must be between 1 and 4";
        }
        if (!rejected)
            throw std::runtime_error("Capacity inner-work range was not validated");
    }
    std::cout << "PASS capacity inner-work range (0 and 5 rejected before allocation)\n";
    ComPtr<IDXGIFactory6> factory;
    gpu::check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Test DXGI factory");
    ComPtr<ID3D12Device> device;
    for (uint32_t i = 0; !device; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
            throw std::runtime_error("No hardware DX12 GPU");
        DXGI_ADAPTER_DESC1 desc{};
        gpu::check(adapter->GetDesc1(&desc), "Test GPU description");
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    gpu::check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)),
               "Test double precision support");
    if (!options.DoublePrecisionFloatShaderOps)
        throw std::runtime_error("Carrier validation requires GPU FP64");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    gpu::check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Test queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
               "Test allocator");
    ComPtr<ID3D12GraphicsCommandList> cmd;
    gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&cmd)),
               "Test list");
    gpu::check(cmd->Close(), "Initial close");
    ComPtr<ID3D12Fence> fence;
    gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Test fence");
    struct EventHandle {
        HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ~EventHandle() {
            if (value)
                CloseHandle(value);
        }
    } event;
    if (!event.value)
        throw std::runtime_error("Test event creation failed");
    uint64_t frequency = 0, serial = 0;
    gpu::check(queue->GetTimestampFrequency(&frequency), "Test timestamps");
    const Fixture fixtures[]{{"valid mixed fraction", {.5f, .7f, .9f}, .5, 0, 0},
                             {"compressive carrier", {1, 1, 0}, 1, 0, .5},
                             {"flow reversal", {1, 1, 0}, 1, -.25, .5},
                             {"closing capacity", {.2f, .5f, 0}, 1, 0, 0, true},
                             {"isolated protected overfill", {1, 1, 1}, 1, 0, 0, false, true}};
    for (const auto &test : fixtures) {
        gpu::check(allocator->Reset(), "Test allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "Test list reset");
        // Three unit coarse cells, each with eight half-unit fine children.
        const XMUINT4 fine{6, 2, 2, 24};
        constexpr uint32_t faceCount = 3 * 7 * 3 * 3;
        auto face = [](uint32_t x, uint32_t y, uint32_t z) { return (z * 3 + y) * 7 + x; };
        std::array<XMFLOAT4, 3> resident{};
        std::array<XMFLOAT2, 3> capacity{};
        std::array<XMFLOAT2, 24> volumes{};
        std::array<XMFLOAT4, 24> cells{};
        std::array<float, faceCount> area{};
        std::array<double, faceCount> canonical{};
        for (uint32_t i = 0; i < 3; ++i) {
            resident[i].w = test.volume[i];
            capacity[i] = {test.closing && i == 0 ? 0.f : 1.f, 1};
        }
        for (uint32_t i = 0; i < 24; ++i) {
            const uint32_t x = i % 6;
            volumes[i] = {test.closing && x < 2 ? 0.f : .125f, .125f};
            cells[i].z = x < 2 || test.reject ? 1.f : 0.f;
        }
        for (uint32_t z = 0; z < 2; ++z)
            for (uint32_t y = 0; y < 2; ++y) {
                area[face(2, y, z)] = area[face(4, y, z)] = .25f;
                canonical[face(2, y, z)] = test.inlet / 4;
                canonical[face(4, y, z)] = test.air / 4;
            }
        std::vector<gpu::Buffer> uploads;
        auto upload = [&](const auto &data) {
            auto staging = gpu::buffer(device.Get(), sizeof(data), D3D12_HEAP_TYPE_UPLOAD);
            memcpy(staging.mapped, data.data(), sizeof(data));
            auto target =
                gpu::buffer(device.Get(), sizeof(data), D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(target.resource.Get(), 0, staging.resource.Get(), 0, sizeof(data));
            gpu::transition(cmd.Get(), target.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uploads.push_back(std::move(staging));
            return target;
        };
        auto q = upload(resident), cap = upload(capacity), vol = upload(volumes), cell = upload(cells),
             aperture = upload(area), flux = upload(canonical);
        FluidCutCellGpuView cut{};
        cut.coarseVolume = cap.resource.Get();
        cut.fineVolume = vol.resource.Get();
        cut.pressureArea = aperture.resource.Get();
        cut.timeCentered = true;
        FluidCarrierProjection carrier(device.Get(), argv[1], fine, {0, 0, 0, .5f}, {3, 1, 1, 0});
        carrier.beginFrame(cmd.Get(), true, true);
        auto result =
            carrier.record(cmd.Get(), cut, q.resource.Get(), cell.resource.Get(), flux.resource.Get(), 1);
        carrier.finishFrame(cmd.Get());
        auto readback = gpu::buffer(device.Get(), sizeof(canonical), D3D12_HEAP_TYPE_READBACK,
                                    D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        gpu::transition(cmd.Get(), result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(readback.resource.Get(), 0, result, 0, sizeof(canonical));
        gpu::check(cmd->Close(), "Test close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Test signal");
        gpu::check(fence->SetEventOnCompletion(serial, event.value), "Test completion event");
        if (WaitForSingleObject(event.value, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("Carrier GPU test timed out");
        bool rejected = false;
        try {
            carrier.collect(frequency);
        } catch (const std::runtime_error &error) {
            if (!test.reject || std::string(error.what()).find("Carrier projection audit failed") != 0)
                throw;
            rejected = true;
        }
        if (rejected != test.reject)
            throw std::runtime_error("Capacity rejection mismatch");
        void *mapped;
        D3D12_RANGE range{0, sizeof(canonical)}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Test result map");
        const auto output = static_cast<const double *>(mapped);
        double actual = 0;
        for (uint32_t z = 0; z < 2; ++z)
            for (uint32_t y = 0; y < 2; ++y)
                actual += output[face(4, y, z)];
        const bool unchanged = !test.reject || memcmp(output, canonical.data(), sizeof(canonical)) == 0;
        readback.resource->Unmap(0, &written);
        if (!unchanged || std::abs(actual - test.expected) > 2e-6)
            throw std::runtime_error("Analytic carrier flux mismatch");
        std::cout << "PASS " << test.name << " | air flux " << actual << '\n';
    }
    {
        gpu::check(allocator->Reset(), "Multigrid test allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "Multigrid test list reset");
        std::array<FluidMacRow, 2> rows{};
        std::array<FluidCutPressureRow, 2> exact{};
        std::array<XMFLOAT4, 2> cells{};
        const std::array<uint32_t, 2> map{0, 1}, list{0, 1};
        std::array<uint32_t, 64> counts{};
        counts[0] = 2;
        const std::array<float, 2> solution{};
        const std::array<double, 2> rhs{.25, -.75};
        for (uint32_t i = 0; i < 2; ++i) {
            // Independently specified SPD operator [2 -1; -1 2].
            rows[i].count = 1;
            rows[i].diagonal = 2;
            rows[i].rhs = i == 0 ? 1.f : 0.f;
            rows[i].step = .6f;
            rows[i].neighbor[0] = 1 - i;
            rows[i].coefficient[0] = -1;
            rows[i].boundaryDiagonal = rows[i].volumeUnits = 1;
            exact[i].rhs = rows[i].rhs;
            exact[i].diagonal = 2;
            exact[i].conductance[0] = exact[i].conductance[1] = 1;
            cells[i].z = 1;
        }
        std::vector<gpu::Buffer> uploads;
        auto upload = [&](const auto &data) {
            auto staging = gpu::buffer(device.Get(), sizeof(data), D3D12_HEAP_TYPE_UPLOAD);
            memcpy(staging.mapped, data.data(), sizeof(data));
            auto target =
                gpu::buffer(device.Get(), sizeof(data), D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(target.resource.Get(), 0, staging.resource.Get(), 0, sizeof(data));
            gpu::transition(cmd.Get(), target.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            uploads.push_back(std::move(staging));
            return target;
        };
        auto r = upload(rows), ex = upload(exact), c = upload(cells), m = upload(map), l = upload(list),
             n = upload(counts), s = upload(solution), correction = upload(rhs);
        FluidMacPressure mg(device.Get(), argv[1], {2, 1, 1, 2}, 1, 1, 1, true);
        mg.beginFrame(true);
        mg.solve(cmd.Get(), r.resource.Get(), m.resource.Get(), c.resource.Get(), l.resource.Get(),
                 n.resource.Get(), s.resource.Get(), ex.resource.Get());
        auto readback = gpu::buffer(device.Get(), 256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
        auto capture = [&](uint32_t offset, ID3D12Resource *source) {
            gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(), offset, source, 0, 16);
            gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        capture(0, mg.pressureResource());
        mg.precondition(cmd.Get(), correction.resource.Get());
        capture(16, mg.pressureResource());
        capture(32, correction.resource.Get());
        gpu::check(cmd->Close(), "Multigrid test close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Multigrid test signal");
        gpu::check(fence->SetEventOnCompletion(serial, event.value), "Multigrid test event");
        if (WaitForSingleObject(event.value, 30000) != WAIT_OBJECT_0)
            throw std::runtime_error("Multigrid reuse test timed out");
        mg.collect(); // Original physical solve's independent operator/factor/residual audit.
        void *mapped;
        D3D12_RANGE range{0, 48}, written{0, 0};
        gpu::check(readback.resource->Map(0, &range, &mapped), "Multigrid test map");
        const auto values = static_cast<const double *>(mapped);
        const bool preserved = memcmp(values, values + 2, 16) == 0;
        const std::array<double, 2> actual{values[4], values[5]};
        readback.resource->Unmap(0, &written);
        // Independent dense V-cycle reference: two pre/post Richardson steps,
        // constant restriction/prolongation, and the documented bottom shift.
        std::array<double, 2> expected{};
        const auto residual = [&] {
            return std::array<double, 2>{rhs[0] - 2 * expected[0] + expected[1],
                                         rhs[1] + expected[0] - 2 * expected[1]};
        };
        auto smooth = [&] {
            const auto b = residual();
            for (uint32_t i = 0; i < 2; ++i)
                expected[i] += .48 * b[i];
        };
        smooth();
        smooth();
        const auto coarseRhs = residual();
        const double coarse = (coarseRhs[0] + coarseRhs[1]) / 2.0002;
        expected[0] += coarse;
        expected[1] += coarse;
        smooth();
        smooth();
        if (!preserved || std::abs(actual[0] - expected[0]) > 2e-7 ||
            std::abs(actual[1] - expected[1]) > 2e-7)
            throw std::runtime_error("Reused multigrid correction or physical pressure preservation failed");
        std::cout << "PASS reused multigrid V-cycle | original pressure unchanged | correction " << actual[0]
                  << ',' << actual[1] << '\n';
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
