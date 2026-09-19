#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lab::asset {
constexpr uint32_t materialBase = 64, maxTextures = 512, noTexture = ~0u;
struct Float3 { float x = 0, y = 0, z = 0; };
struct Float4 { float x = 0, y = 0, z = 0, w = 0; };
// These three structures are also consumed by model-material.hlsli.
struct Attributes {
    Float4 normal, tangent, uv, color{1, 1, 1, 1}; // uv.xy = UV0, uv.zw = UV1
};
struct TextureBinding {
    uint32_t image = noTexture, uvSet = 0, channel = 0, sampler = 0;
    Float4 u{1, 0, 0, 0}, v{0, -1, 1, 0}; // Assimp UVs -> image coordinates
};
enum TextureSlot { BaseColor, Metallic, Roughness, Normal, Occlusion, Emissive, Opacity, TextureSlotCount };
struct Material {
    Float4 baseColor{1, 1, 1, 1}, emissive{};
    Float4 factors{0, 1, 1, 1}; // metallic, roughness, normal scale, AO strength
    float alphaCutoff = .5f;
    uint32_t alphaMode = 0, doubleSided = 0, unlit = 0; // opaque, mask, stochastic blend
    std::array<TextureBinding, TextureSlotCount> textures;
};
struct Mip {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
struct Image {
    std::string name;
    bool srgb = false;
    std::vector<Mip> mips;
};
struct Vertex {
    Float3 position;
    Attributes attributes;
};
struct Mesh {
    std::string name;
    uint32_t material = 0;
    std::vector<Vertex> vertices; // triangle list; node transforms baked in
};
struct Model {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<std::string> warnings;
};
// Throws with source context on malformed input or a missing referenced image.
// Static glTF 2.0 / GLB and OBJ + MTL. Does not modify renderer state on failure.
Model loadModel(const std::filesystem::path &path);
static_assert(sizeof(Attributes) == 64 && sizeof(TextureBinding) == 48 && sizeof(Material) == 400);
} // namespace lab::asset
