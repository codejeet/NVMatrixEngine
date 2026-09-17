#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Rectangle.h>
#include <array>
#include <filesystem>
#include <vector>
// Small RmlUi backend sharing the game's DX12 device, command list and fenced frame.
// Supports text, textured geometry, transforms and scissor clipping. No offscreen CSS filters.
class UiRenderer final : public Rml::RenderInterface {
  public:
    UiRenderer(ID3D12Device *device, const std::filesystem::path &shader);
    void begin(ID3D12GraphicsCommandList *commands, int width, int height);
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex>, Rml::Span<const int>) override;
    void RenderGeometry(Rml::CompiledGeometryHandle, Rml::Vector2f, Rml::TextureHandle) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i &, const Rml::String &) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte>, Rml::Vector2i) override;
    void ReleaseTexture(Rml::TextureHandle) override;
    void EnableScissorRegion(bool enabled) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f *) override;
    unsigned draws = 0;
    // Committed geometry allocations, reuse count, cached bytes, high-water cached bytes.
    std::array<uint64_t, 4> geometryUploadStats() const {
        return {geometryAllocations, geometryReuses, cachedGeometryBytes, peakCachedGeometryBytes};
    }

  private:
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    struct Geometry {
        Ptr<ID3D12Resource> vertex, index;
        UINT vertices, indices;
    };
    struct Texture {
        Ptr<ID3D12Resource> resource, upload;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT slot;
        bool pending;
    };
    ID3D12Device *device;
    ID3D12GraphicsCommandList *commands = nullptr;
    Ptr<ID3D12DescriptorHeap> heap;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pipeline;
    std::vector<Ptr<ID3D12Resource>> retired;
    // Geometry released while recording must not be reused in that same frame.
    // begin() moves it into the cache only after the caller's existing frame fence.
    struct CachedUpload {
        Ptr<ID3D12Resource> resource;
        uint64_t bytes;
    };
    std::vector<CachedUpload> retiredGeometry, cachedGeometry;
    static constexpr uint64_t geometryCacheLimit = 8 * 1024 * 1024;
    static constexpr size_t geometryCacheEntries = 256;
    uint64_t cachedGeometryBytes = 0, peakCachedGeometryBytes = 0;
    uint64_t geometryAllocations = 0, geometryReuses = 0;
    std::vector<UINT> freeSlots, retiredSlots;
    Texture *white = nullptr;
    UINT descriptorSize = 0;
    int width = 1, height = 1;
    bool scissorEnabled = false;
    D3D12_RECT scissor{};
    std::array<float, 16> transform{};
    Ptr<ID3D12Resource> upload(size_t size, const void *data = nullptr);
    Ptr<ID3D12Resource> geometryUpload(size_t size, const void *data);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot) const;
    void applyScissor();

  public:
    ~UiRenderer();
};
