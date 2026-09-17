#pragma once
#include "../src/gpu_resources.h"
#include <d3dcompiler.h>
#include <array>

// Tests borrow the production GPU binning kernels. No CPU bin repair is allowed
// between a real retirement and a consumer that combines particle/grid owners.
class FluidTestBinning {
    lab::gpu::Buffer counts, cursors, scan, dummy;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 6> pipelines;
    uint32_t cells, particles;

  public:
    FluidTestBinning(ID3D12Device *device, const std::filesystem::path &folder, uint32_t cellCount,
                     uint32_t particleCount)
        : cells(cellCount), particles(particleCount) {
        auto make = [&](uint64_t bytes) {
            return lab::gpu::buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };
        counts = make(uint64_t(cells) * 4);
        cursors = make(uint64_t(cells) * 4);
        scan = make(uint64_t((cells + 255) / 256) * 4);
        dummy = make(256);
        D3D12_ROOT_PARAMETER p[10]{};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        const uint32_t registers[]{0, 1, 2, 3, 4, 5, 15, 22, 23};
        for (uint32_t i = 0; i < 9; i++) {
            p[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[i + 1].Descriptor.ShaderRegister = registers[i];
        }
        D3D12_ROOT_SIGNATURE_DESC d{10, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        lab::gpu::check(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
                        "Test bin root serialization");
        lab::gpu::check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    IID_PPV_ARGS(&root)),
                        "Test bin root");
        const char *names[]{"ClearBins", "CountBins", "ScanCells", "ScanSums", "FinishScan", "Scatter"};
        for (uint32_t i = 0; i < 6; i++) {
            auto code = lab::gpu::bytes(folder / "shaders" / (std::string("Fluid") + names[i] + ".dxil"));
            D3D12_COMPUTE_PIPELINE_STATE_DESC s{};
            s.pRootSignature = root.Get();
            s.CS = {code.data(), code.size()};
            lab::gpu::check(device->CreateComputePipelineState(&s, IID_PPV_ARGS(&pipelines[i])), names[i]);
        }
    }
    // Constants must select Display.w=0; the dummy is not a mass/interior cache.
    void record(ID3D12GraphicsCommandList *cmd, ID3D12Resource *frame, ID3D12Resource *samples,
                ID3D12Resource *offsets, ID3D12Resource *indices) {
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRootConstantBufferView(0, frame->GetGPUVirtualAddress());
        ID3D12Resource *resources[]{samples,
                                    counts.resource.Get(),
                                    offsets,
                                    cursors.resource.Get(),
                                    indices,
                                    scan.resource.Get(),
                                    dummy.resource.Get(),
                                    dummy.resource.Get(),
                                    dummy.resource.Get()};
        for (uint32_t i = 0; i < 9; i++)
            cmd->SetComputeRootUnorderedAccessView(i + 1, resources[i]->GetGPUVirtualAddress());
        const uint32_t groups[]{(cells + 255) / 256, (particles + 255) / 256, (cells + 255) / 256, 1,
                                (cells + 255) / 256, (particles + 255) / 256};
        for (uint32_t i = 0; i < 6; i++) {
            cmd->SetPipelineState(pipelines[i].Get());
            cmd->Dispatch(groups[i], 1, 1);
            lab::gpu::uav(cmd);
        }
    }
};
