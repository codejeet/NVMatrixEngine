#include "ui_renderer.h"
#include "gpu_error.h"
#include <RmlUi/Core/Matrix4.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <cstring>
#include <algorithm>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
void barrier(ID3D12GraphicsCommandList *cmd, ID3D12Resource *resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
    cmd->ResourceBarrier(1, &b);
}
} // namespace
UiRenderer::Ptr<ID3D12Resource> UiRenderer::upload(size_t size, const void *data) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<size_t>(size, 256);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Ptr<ID3D12Resource> r;
    hrCheck(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&r)),
            "RmlUi upload");
    if (data) {
        void *p;
        D3D12_RANGE read{0, 0};
        hrCheck(r->Map(0, &read, &p), "RmlUi map");
        memcpy(p, data, size);
        r->Unmap(0, nullptr);
    }
    return r;
}
D3D12_CPU_DESCRIPTOR_HANDLE UiRenderer::cpu(UINT slot) const {
    auto h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(slot) * descriptorSize;
    return h;
}
UiRenderer::Ptr<ID3D12Resource> UiRenderer::geometryUpload(size_t size, const void *data) {
    // Size classes prevent constantly changing FPS/status text from allocating
    // a new committed resource for every glyph-count change. Texture staging
    // and descriptor retirement retain their separate existing lifecycle.
    uint64_t capacity = 256;
    while (capacity < size)
        capacity *= 2;
    Ptr<ID3D12Resource> resource;
    for (size_t i = 0; i < cachedGeometry.size(); ++i) {
        if (cachedGeometry[i].bytes != capacity)
            continue;
        resource = std::move(cachedGeometry[i].resource);
        cachedGeometryBytes -= capacity;
        cachedGeometry[i] = std::move(cachedGeometry.back());
        cachedGeometry.pop_back();
        ++geometryReuses;
        break;
    }
    if (!resource) {
        resource = upload(size_t(capacity));
        resource->SetName(L"RmlUi / reusable geometry upload");
        ++geometryAllocations;
    }
    if (size) {
        void *mapped = nullptr;
        D3D12_RANGE read{0, 0};
        hrCheck(resource->Map(0, &read, &mapped), "RmlUi geometry map");
        std::memcpy(mapped, data, size);
        D3D12_RANGE written{0, size};
        resource->Unmap(0, &written);
    }
    return resource;
}
UiRenderer::UiRenderer(ID3D12Device *d, const std::filesystem::path &shader) : device(d) {
    cachedGeometry.reserve(geometryCacheEntries);
    retiredGeometry.reserve(geometryCacheEntries);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 256;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hrCheck(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)), "RmlUi descriptor heap");
    descriptorSize = device->GetDescriptorHandleIncrementSize(hd.Type);
    for (UINT i = 256; i > 0; i--)
        freeSlots.push_back(i - 1);
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants = {0, 0, 20};
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable = {1, &range};
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    D3D12_ROOT_SIGNATURE_DESC rd{2, parameters, 1, &sampler,
                                 D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3DBlob> signature, error, vs, ps;
    hrCheck(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error),
            "RmlUi root signature");
    hrCheck(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                        IID_PPV_ARGS(&root)),
            "RmlUi root");
    auto compile = [&](const char *entry, const char *target, ID3DBlob **out) {
        HRESULT result = D3DCompileFromFile(shader.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry,
                                            target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &error);
        if (FAILED(result))
            throw std::runtime_error(error ? static_cast<const char *>(error->GetBufferPointer())
                                           : "RmlUi shader compile failed");
    };
    compile("VS", "vs_5_0", &vs);
    compile("PS", "ps_5_0", &ps);
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    static_assert(sizeof(Rml::Vertex) == 20);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.InputLayout = {elements, 3};
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.SampleMask = UINT_MAX;
    p.SampleDesc.Count = 1;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    auto &blend = p.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    hrCheck(device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pipeline)), "RmlUi pipeline");
    const Rml::byte pixel[] = {255, 255, 255, 255};
    white = reinterpret_cast<Texture *>(GenerateTexture({pixel, 4}, {1, 1}));
    SetTransform(nullptr);
}
UiRenderer::~UiRenderer() {
    if (white) {
        delete white;
        white = nullptr;
    }
}
void UiRenderer::begin(ID3D12GraphicsCommandList *cmd, int w, int h) {
    // Caller waited for the previous frame, so descriptor slots and uploads can now be reused.
    for (auto &entry : retiredGeometry) {
        if (entry.bytes <= geometryCacheLimit - cachedGeometryBytes &&
            cachedGeometry.size() < geometryCacheEntries) {
            cachedGeometryBytes += entry.bytes;
            cachedGeometry.push_back(std::move(entry));
        }
    }
    peakCachedGeometryBytes = std::max(peakCachedGeometryBytes, cachedGeometryBytes);
    retiredGeometry.clear();
    retired.clear();
    freeSlots.insert(freeSlots.end(), retiredSlots.begin(), retiredSlots.end());
    retiredSlots.clear();
    commands = cmd;
    width = w;
    height = h;
    draws = 0;
    scissorEnabled = false;
    SetTransform(nullptr);
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    commands->SetDescriptorHeaps(1, heaps);
    commands->SetGraphicsRootSignature(root.Get());
    commands->SetPipelineState(pipeline.Get());
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
    commands->RSSetViewports(1, &viewport);
    applyScissor();
}
Rml::CompiledGeometryHandle UiRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                        Rml::Span<const int> indices) {
    if (vertices.size() > UINT_MAX / sizeof(Rml::Vertex) || indices.size() > UINT_MAX / sizeof(int))
        throw std::runtime_error("RmlUi geometry exceeds DX12 view limits");
    auto g = std::make_unique<Geometry>();
    g->vertices = UINT(vertices.size());
    g->indices = UINT(indices.size());
    g->vertex = geometryUpload(vertices.size() * sizeof(Rml::Vertex), vertices.data());
    g->index = geometryUpload(indices.size() * sizeof(int), indices.data());
    return reinterpret_cast<Rml::CompiledGeometryHandle>(g.release());
}
void UiRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    auto g = reinterpret_cast<Geometry *>(handle);
    retiredGeometry.push_back({g->vertex, g->vertex->GetDesc().Width});
    retiredGeometry.push_back({g->index, g->index->GetDesc().Width});
    delete g;
}
void UiRenderer::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                Rml::TextureHandle texture) {
    auto g = reinterpret_cast<Geometry *>(handle);
    auto t = texture ? reinterpret_cast<Texture *>(texture) : white;
    if (t->pending) {
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = t->upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = t->footprint;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = t->resource.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(commands, t->resource.Get());
        t->pending = false;
        retired.push_back(t->upload);
        t->upload.Reset();
    }
    const float dimensions[] = {float(width), float(height), translation.x, translation.y};
    commands->SetGraphicsRoot32BitConstants(0, 4, dimensions, 0);
    commands->SetGraphicsRoot32BitConstants(0, 16, transform.data(), 4);
    auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += UINT64(t->slot) * descriptorSize;
    commands->SetGraphicsRootDescriptorTable(1, gpu);
    D3D12_VERTEX_BUFFER_VIEW vb{g->vertex->GetGPUVirtualAddress(), g->vertices * sizeof(Rml::Vertex),
                                sizeof(Rml::Vertex)};
    D3D12_INDEX_BUFFER_VIEW ib{g->index->GetGPUVirtualAddress(), g->indices * sizeof(int),
                               DXGI_FORMAT_R32_UINT};
    commands->IASetVertexBuffers(0, 1, &vb);
    commands->IASetIndexBuffer(&ib);
    commands->DrawIndexedInstanced(g->indices, 1, 0, 0, 0);
    draws++;
}
Rml::TextureHandle UiRenderer::GenerateTexture(Rml::Span<const Rml::byte> pixels, Rml::Vector2i size) {
    if (size.x <= 0 || size.y <= 0 || size.x > 8192 || size.y > 8192 ||
        pixels.size() != size_t(size.x) * size.y * 4 || freeSlots.empty())
        return 0;
    auto t = std::make_unique<Texture>();
    t->pending = true;
    t->slot = freeSlots.back();
    freeSlots.pop_back();
    D3D12_HEAP_PROPERTIES h{};
    h.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = size.x;
    d.Height = size.y;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    hrCheck(device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                            nullptr, IID_PPV_ARGS(&t->resource)),
            "RmlUi texture");
    UINT64 total;
    device->GetCopyableFootprints(&d, 0, 1, 0, &t->footprint, nullptr, nullptr, &total);
    t->upload = upload(size_t(total));
    void *mapped;
    D3D12_RANGE read{0, 0};
    hrCheck(t->upload->Map(0, &read, &mapped), "RmlUi texture upload");
    for (int y = 0; y < size.y; y++)
        memcpy(static_cast<char *>(mapped) + size_t(y) * t->footprint.Footprint.RowPitch,
               pixels.data() + size_t(y) * size.x * 4, size_t(size.x) * 4);
    t->upload->Unmap(0, nullptr);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = d.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(t->resource.Get(), &srv, cpu(t->slot));
    return reinterpret_cast<Rml::TextureHandle>(t.release());
}
void UiRenderer::ReleaseTexture(Rml::TextureHandle handle) {
    auto t = reinterpret_cast<Texture *>(handle);
    retired.push_back(t->resource);
    if (t->upload)
        retired.push_back(t->upload);
    retiredSlots.push_back(t->slot);
    delete t;
}
Rml::TextureHandle UiRenderer::LoadTexture(Rml::Vector2i &size, const Rml::String &path) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return 0;
    auto file = std::filesystem::u8path(path);
    if (FAILED(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)))
        return 0;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
                                     nullptr, 0, WICBitmapPaletteTypeCustom)))
        return 0;
    UINT w, h;
    if (FAILED(converter->GetSize(&w, &h)) || w > 8192 || h > 8192)
        return 0;
    std::vector<Rml::byte> data(size_t(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, UINT(data.size()), data.data())))
        return 0;
    size = {int(w), int(h)};
    return GenerateTexture(data, size);
}
void UiRenderer::EnableScissorRegion(bool enabled) {
    scissorEnabled = enabled;
    applyScissor();
}
void UiRenderer::SetScissorRegion(Rml::Rectanglei r) {
    scissor = {r.Left(), r.Top(), r.Right(), r.Bottom()};
    applyScissor();
}
void UiRenderer::applyScissor() {
    if (!commands)
        return;
    D3D12_RECT r = scissorEnabled ? scissor : D3D12_RECT{0, 0, width, height};
    r.left = std::clamp(r.left, 0L, LONG(width));
    r.right = std::clamp(r.right, r.left, LONG(width));
    r.top = std::clamp(r.top, 0L, LONG(height));
    r.bottom = std::clamp(r.bottom, r.top, LONG(height));
    commands->RSSetScissorRects(1, &r);
}
void UiRenderer::SetTransform(const Rml::Matrix4f *matrix) {
    if (matrix)
        memcpy(transform.data(), matrix->data(), sizeof(float) * 16);
    else {
        transform.fill(0);
        transform[0] = transform[5] = transform[10] = transform[15] = 1;
    }
}
