#pragma once
#include "cuda_surface_rays.h"
#include <bit>
#include <map>

namespace lab::cuda_test {
inline std::array<double,4> curvedSurfaces(Fixture &d, const std::filesystem::path &folder,uint32_t refinement) {
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cmd;
    gpu::check(d.device.As(&device), "Curved surface device");
    gpu::check(d.cmd.As(&cmd), "Curved surface command list");
    FluidSystemDesc desc{};
    desc.minimum = {0, 0, 0};
    desc.maximum = {2.5f, 3.5f, 1.5f};
    desc.gridCellSize = .125f/float(refinement);
    desc.maxParticles = 1;
    FluidSurface surface(device.Get(), folder, desc);
    SurfaceRays rays(d, device.Get(), folder);
    const uint32_t nx = 20*refinement, ny = 28*refinement, nz = 12*refinement, owners = (nx / 2) * (ny / 2) * (nz / 2);
    const uint64_t fieldBytes=surface.fieldResource()->GetDesc().Width,mapBytes=surface.mapResource()->GetDesc().Width;
    auto readback=gpu::buffer(d.device.Get(),fieldBytes+mapBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    uint64_t frequency=0,sharedNodes=0,bandNodes=0,heightNodes=0;
    gpu::check(d.queue->GetTimestampFrequency(&frequency),"Curved surface timestamp frequency");
    double maxFieldError=0,maxVolumeError=0;
    auto phase = d.make(owners * 16), planes = d.make(owners * 32), dummy = d.make(256),
         faces = d.make((nx + 1) * (ny + 1) * (nz + 1) * 48);
    auto zeros = gpu::buffer(d.device.Get(), faces.resource->GetDesc().Width, D3D12_HEAP_TYPE_UPLOAD);
    std::memset(zeros.mapped, 0, size_t(zeros.resource->GetDesc().Width));
    FluidGpuView view{};
    view.grid = {nx, ny, nz, nx * ny * nz};
    view.particles = view.offsets = view.indices = view.previousPositions = dummy.resource.Get();
    view.meshPhi = view.interior = view.interiorTotals = dummy.resource.Get();
    view.faces = faces.resource.Get();
    view.colliderAddress = dummy.resource->GetGPUVirtualAddress();
    {
        auto wrong=desc;wrong.maximum.x+=desc.gridCellSize*2;
        auto wrongView=view;wrongView.grid.x+=2;wrongView.grid.w=wrongView.grid.x*ny*nz;
        d.begin();bool rejected=false;
        try{surface.record(cmd.Get(),{wrongView,wrong,0,true,true,0,phase.resource.Get(),planes.resource.Get()},Camera{},1.f/60);}
        catch(const std::runtime_error &e){rejected=std::string(e.what()).find("grid dimensions")!=std::string::npos;}
        require(rejected,"Phase surface accepted a changed allocation lattice");d.submit();
    }
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 8;
    for (uint32_t i = 1; i < 3; ++i) {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[i].Descriptor.ShaderRegister = i - 1;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{3, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> seed;
    gpu::check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error),
               "Curved seed root serialization");
    gpu::check(d.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root)),
               "Curved seed root");
    const auto code = gpu::bytes(folder / "shaders/CudaSurfaceCurveSeed.dxil");
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
    ps.pRootSignature = root.Get();
    ps.CS = {code.data(), code.size()};
    gpu::check(d.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&seed)), "Curved seed PSO");
    const DirectX::XMFLOAT4 sequence[]{{1.25f, 1.325f, .75f, 0},
                                       {1.25f, 1.325f, .75f, .08f},
                                       {1.25f, 1.325f, .75f, -.08f},
                                       {1.267f, 1.331f, .737f, .12f}};
    uint32_t frame = 0;
    for (const auto &shape : sequence) {
        d.begin();
        if (!frame) {
            gpu::transition(cmd.Get(), faces.resource.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
            cmd->CopyBufferRegion(faces.resource.Get(), 0, zeros.resource.Get(), 0,
                                  faces.resource->GetDesc().Width);
            gpu::transition(cmd.Get(), faces.resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        uint32_t grid[]{nx, ny, nz, std::bit_cast<uint32_t>(desc.gridCellSize)};
        cmd->SetComputeRootSignature(root.Get());
        cmd->SetComputeRoot32BitConstants(0, 4, grid, 0);
        cmd->SetComputeRoot32BitConstants(0, 4, &shape, 4);
        cmd->SetComputeRootUnorderedAccessView(1, phase.resource->GetGPUVirtualAddress());
        cmd->SetComputeRootUnorderedAccessView(2, planes.resource->GetGPUVirtualAddress());
        cmd->SetPipelineState(seed.Get());
        cmd->Dispatch((owners + 63) / 64, 1, 1);
        gpu::uav(cmd.Get());
        ++frame;
        surface.record(cmd.Get(),
                       {view, desc, frame, true, frame == 1, 0, phase.resource.Get(), planes.resource.Get()},
                       Camera{}, 1.f / 60);
        rays.record(cmd.Get(), surface);
        surface.recordReadback(cmd.Get());
        uint64_t offset=0;
        for(auto *r:{surface.fieldResource(),surface.mapResource()}){
            gpu::transition(cmd.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(readback.resource.Get(),offset,r,0,r->GetDesc().Width);offset+=r->GetDesc().Width;
            gpu::transition(cmd.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        d.submit();
        rays.collectCurved(shape);
        surface.collect(frequency);
        require(surface.phaseHeightColumns&&surface.phaseHeightNodes,"Curved fixture did not use volume height reconstruction");
        heightNodes+=surface.phaseHeightNodes;
        void *data=nullptr;D3D12_RANGE range{0,size_t(fieldBytes+mapBytes)},written{0,0};
        gpu::check(readback.resource->Map(0,&range,&data),"Curved field readback");
        const auto *nodes=static_cast<const DirectX::XMFLOAT4*>(data);
        const auto *pages=reinterpret_cast<const uint32_t*>(static_cast<const char*>(data)+fieldBytes);
        std::map<std::array<uint32_t,3>,DirectX::XMFLOAT4> seen;const auto bricks=surface.brickGrid;
        for(uint32_t brick=0;brick<bricks.w;++brick){const uint32_t slot=pages[brick];if(slot==0xffffffff)continue;
            require(slot<bricks.w,"Curved field page overflow");
            for(uint32_t node=0;node<729;++node){
                const std::array<uint32_t,3> key{brick%bricks.x*8+node%9,brick/bricks.x%bricks.y*8+node/9%9,brick/(bricks.x*bricks.y)*8+node/81};
                const auto value=nodes[slot*729+node];
                require(std::isfinite(value.x)&&value.y==0&&value.z==0&&value.w==0,"Invalid curved field/material motion");
                auto [entry,inserted]=seen.emplace(key,value);if(!inserted){++sharedNodes;require(!std::memcmp(&value,&entry->second,sizeof(value)),"Curved brick seam mismatch");}
                const double h=desc.gridCellSize,x=(key[0]*.5-2)*h,y=(key[1]*.5-2)*h,z=(key[2]*.5-2)*h;
                if(x<0||x>2.5||y<0||y>3.5||z<0||z>1.5)continue;
                const double dx=x-shape.x,dz=z-shape.z,q=shape.w;
                const double expected=(y-shape.y-q*(dx*dx+dz*dz))/std::sqrt(1+4*q*q*(dx*dx+dz*dz));
                if(std::abs(expected)<h*.5){++bandNodes;maxFieldError=std::max(maxFieldError,std::abs(expected-value.x));
                    require(std::abs(expected-value.x)<1e-6,"Height reconstruction loses quadratic cell-average precision");}
            }
        }
        readback.resource->Unmap(0,&written);
        const double q=shape.w,dx=1.25-shape.x,dz=.75-shape.z,area=2.5*1.5,s=surface.minimumSpacing.w;
        const double exact=area*(shape.y+q*(dx*dx+dz*dz+(2.5*2.5+1.5*1.5)/12));
        const double volumeError=std::abs(surface.renderedVolume-exact);maxVolumeError=std::max(maxVolumeError,volumeError);
        require(volumeError<std::abs(q)*area*s*s+area*s/256+1e-6,"Curved contour volume exceeds interpolation/quantization bound");
        d.begin();
        rays.record(cmd.Get(), surface, true);
        d.submit();
        rays.collectCurved(shape);
    }
    const std::string label="curved-"+std::to_string(refinement);
    rays.report(false,label.c_str());
    require(sharedNodes&&bandNodes,"Curved fixture omitted nodal/seam checks");
    std::cout<<"{\"case\":\"surface-field-"<<label<<"\",\"frames\":"<<frame<<",\"sharedNodes\":"<<sharedNodes
             <<",\"bandNodes\":"<<bandNodes<<",\"heightNodes\":"<<heightNodes<<",\"maxFieldError\":"<<maxFieldError
             <<",\"maxVolumeError\":"<<maxVolumeError<<",\"pass\":true}\n";
    const auto metrics=rays.curvedMetrics();return {metrics[0],metrics[1],metrics[2],maxVolumeError};
}
} // namespace lab::cuda_test
