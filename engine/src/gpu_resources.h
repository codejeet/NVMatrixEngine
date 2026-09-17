#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <pix3.h>

namespace lab::gpu {
using Microsoft::WRL::ComPtr;
inline void check(HRESULT hr, const char *operation) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(operation) + " (HRESULT " + std::to_string(unsigned(hr)) + ")");
}
struct Buffer {
    ComPtr<ID3D12Resource> resource;
    void *mapped = nullptr;
};
inline Buffer buffer(ID3D12Device *device, uint64_t size, D3D12_HEAP_TYPE type,
                     D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_GENERIC_READ,
                     const wchar_t *name = L"Lab buffer",
                     D3D12_HEAP_FLAGS heapFlags = D3D12_HEAP_FLAG_NONE) {
    Buffer b;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(size, 256);
    d.Height = 1;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    check(device->CreateCommittedResource(&hp, heapFlags, &d, state, nullptr,
                                          IID_PPV_ARGS(&b.resource)),
          "Create buffer");
    b.resource->SetName(name);
    if (type == D3D12_HEAP_TYPE_UPLOAD) {
        D3D12_RANGE range{0, 0};
        check(b.resource->Map(0, &range, &b.mapped), "Map upload");
    }
    return b;
}
inline void transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                       D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    cmd->ResourceBarrier(1, &x);
}
inline void uav(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r = nullptr) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    x.UAV.pResource = r;
    cmd->ResourceBarrier(1, &x);
}
inline std::vector<char> bytes(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Missing shader: " + path.string());
    std::vector<char> data(size_t(in.tellg()));
    in.seekg(0);
    in.read(data.data(), data.size());
    if (!in)
        throw std::runtime_error("Cannot read shader: " + path.string());
    return data;
}
// Official PIX encoding, with matching CPU/GPU regions in the lab build.
struct Event {
    ID3D12GraphicsCommandList *cmd;
    Event(ID3D12GraphicsCommandList *c, const wchar_t *label) : cmd(c) {
        PIXBeginEvent(cmd, PIX_COLOR(30, 150, 230), L"%s", label);
    }
    ~Event() {
        PIXEndEvent(cmd);
    }
};
} // namespace lab::gpu
