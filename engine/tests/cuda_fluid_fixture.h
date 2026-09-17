#pragma once
#include "../src/gpu_resources.h"
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <span>
#include <array>
namespace lab::cuda_test {
using Microsoft::WRL::ComPtr;
inline void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> root;
    std::vector<ComPtr<ID3D12PipelineState>> pipelines;
    ComPtr<ID3D12CommandSignature> dispatch;
    ComPtr<ID3D12InfoQueue> info;
    HANDLE event = nullptr;
    uint64_t serial = 0;
    explicit Fixture(const std::filesystem::path &folder, bool core = false) {
        ComPtr<ID3D12Debug1> debug;
        const auto debugResult = D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (SUCCEEDED(debugResult)) {
            debug->EnableDebugLayer();
            debug->SetEnableGPUBasedValidation(TRUE);
        }
        std::cout << "{\"gpuValidation\":" << (debug ? "true" : "false")
                  << ",\"debugHRESULT\":" << uint32_t(debugResult) << "}\n";
        pipelines.resize(core ? 31 : 10);
        ComPtr<IDXGIFactory6> factory;
        gpu::check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CUDA transfer DXGI");
        for (UINT i = 0; !device; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                    IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND)
                throw std::runtime_error("No NVIDIA DX12 adapter");
            DXGI_ADAPTER_DESC1 desc{};
            gpu::check(adapter->GetDesc1(&desc), "CUDA transfer adapter");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
        }
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC q{};
        gpu::check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CUDA transfer queue");
        gpu::check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
                   "CUDA transfer allocator");
        gpu::check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&cmd)),
                   "CUDA transfer list");
        gpu::check(cmd->Close(), "CUDA transfer initial close");
        gpu::check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                   "CUDA transfer fence");
        D3D12_ROOT_PARAMETER p[22]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        for (uint32_t i = 0; i < 18; ++i) {
            p[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[i + 1].Descriptor.ShaderRegister = i < 16 ? i : (i == 16 ? 22 : 23);
        }
        p[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[19].Descriptor.ShaderRegister = 28;
        p[20].ParameterType = p[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[20].Descriptor.ShaderRegister = 1;
        p[21].Descriptor.ShaderRegister = 2;
        D3D12_ROOT_SIGNATURE_DESC d{22, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC signature{12, 1, &argument, 0};
        gpu::check(device->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch)),
                   "Core indirect dispatch");
        ComPtr<ID3DBlob> blob, error;
        gpu::check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                   "CUDA transfer root serialization");
        gpu::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                               IID_PPV_ARGS(&root)),
                   "CUDA transfer root");
        const char *names[]{"FluidClearBins",
                            "FluidCountBins",
                            "FluidScanCells",
                            "FluidScanSums",
                            "FluidFinishScan",
                            "FluidScatter",
                            "FluidSortBins",
                            "FluidP2G",
                            "FluidG2P",
                            "CudaPerturbFaces",
                            "FluidClassify",
                            "FluidForces",
                            "FluidViscosity",
                            "FluidSurfaceColor",
                            "FluidSurfaceCurvature",
                            "FluidMaterialFixture",
                            "FluidDivergence",
                            "FluidJacobi",
                            "FluidProject",
                            "FluidMeasure",
                            "FluidExtrapolate",
                            "FluidDensityGather",
                            "FluidDensityGatherAdaptive",
                            "FluidDensityDisplace",
                            "FluidDensityMeasure",
                            "FluidDensityClearArguments",
                            "FluidDensityContinueArguments",
                            "FluidDensityPrepareArguments",
                            "FluidDensityJacobi",
                            "FluidBakeSolids",
                            "FluidCollide"};
        for (uint32_t i = 0; i < pipelines.size(); ++i) {
            auto code = gpu::bytes(folder / "shaders" / (std::string(names[i]) + ".dxil"));
            D3D12_COMPUTE_PIPELINE_STATE_DESC s{};
            s.pRootSignature = root.Get();
            s.CS = {code.data(), code.size()};
            gpu::check(device->CreateComputePipelineState(&s, IID_PPV_ARGS(&pipelines[i])), names[i]);
        }
        // Last potentially throwing construction: no raw event leaks if a
        // shader/PSO creation above fails.
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "CUDA transfer event");
    }
    ~Fixture() {
        if (event)
            CloseHandle(event);
    }
    void begin() {
        gpu::check(allocator->Reset(), "Transfer allocator reset");
        gpu::check(cmd->Reset(allocator.Get(), nullptr), "Transfer list reset");
    }
    void submit() {
        gpu::check(cmd->Close(), "Transfer close");
        ID3D12CommandList *lists[]{cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        gpu::check(queue->Signal(fence.Get(), ++serial), "Transfer signal");
        gpu::check(fence->SetEventOnCompletion(serial, event), "Transfer event");
        require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "CUDA transfer GPU timeout");
        if (info) {
            const auto count = info->GetNumStoredMessagesAllowedByRetrievalFilter();
            for (UINT64 i = 0; i < count; ++i) {
                SIZE_T bytes = 0;
                gpu::check(info->GetMessage(i, nullptr, &bytes), "CUDA validation message size");
                std::vector<char> storage(bytes);
                auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
                gpu::check(info->GetMessage(i, message, &bytes), "CUDA validation message");
                if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    throw std::runtime_error(message->pDescription);
            }
            info->ClearStoredMessages();
        }
    }
    void bind(ID3D12Resource *frame, std::span<const gpu::Buffer> buffers,
              ID3D12Resource *colliders = nullptr, ID3D12Resource *mesh = nullptr) {
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, frame->GetGPUVirtualAddress());
        for (uint32_t i = 0; i < 18; ++i)
            cmd->SetComputeRootUnorderedAccessView(i + 1, buffers[i].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(
            19, buffers[buffers.size() > 18 ? 18 : 16].resource->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(
            20, (colliders ? colliders : buffers[16].resource.Get())->GetGPUVirtualAddress());
        cmd->SetComputeRootShaderResourceView(
            21, (mesh ? mesh : buffers[16].resource.Get())->GetGPUVirtualAddress());
    }
    void indirect(uint32_t pipeline, ID3D12Resource *arguments, uint32_t slot) {
        cmd->SetPipelineState(pipelines[pipeline].Get());
        cmd->ExecuteIndirect(dispatch.Get(), 1, arguments, 12ull * slot, nullptr, 0);
        gpu::uav(cmd.Get());
    }
    void pass(uint32_t p, uint32_t groups) {
        cmd->SetPipelineState(pipelines[p].Get());
        cmd->Dispatch(groups, 1, 1);
        gpu::uav(cmd.Get());
    }
    gpu::Buffer make(uint64_t bytes, bool shared = false) {
        return gpu::buffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           L"CUDA transfer fixture", shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE);
    }
    void copyOut(ID3D12Resource *source, ID3D12Resource *destination, uint64_t size, uint64_t offset) {
        gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_COPY_SOURCE);
        cmd->CopyBufferRegion(destination, offset, source, 0, size);
        gpu::transition(cmd.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
};

} // namespace lab::cuda_test
