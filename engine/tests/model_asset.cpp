#include "../src/model_asset.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace lab::asset;
namespace fs = std::filesystem;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void write(const fs::path &p, const std::string &s) { std::ofstream(p, std::ios::binary).write(s.data(), s.size()); }
void word(std::string &s, uint32_t value) { for (int i = 0; i < 4; ++i) s += char(value >> (8 * i)); }
std::string base64(const std::string &bytes) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t v = uint8_t(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= uint8_t(bytes[i + 1]) << 8;
        if (i + 2 < bytes.size()) v |= uint8_t(bytes[i + 2]);
        out += alphabet[v >> 18]; out += alphabet[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? alphabet[(v >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? alphabet[v & 63] : '=';
    }
    return out;
}
std::string png() {
    const unsigned char bytes[]{137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,6,0,0,0,114,182,13,36,0,0,0,19,73,68,65,84,120,156,99,248,207,192,240,31,12,129,52,8,52,0,0,73,73,9,120,40,160,219,119,0,0,0,0,73,69,78,68,174,66,96,130};
    return {reinterpret_cast<const char *>(bytes), sizeof(bytes)};
}
std::string geometry() {
    // Interleaved position / UV0 / UV1; deliberately different UV sets.
    const float data[]{0,0,0, 0,0, .25f,.75f, 1,0,0, 1,0, .75f,.75f, 0,1,0, 0,1, .25f,.25f};
    std::string bytes(reinterpret_cast<const char *>(data), sizeof(data));
    bytes += std::string("\0\0\1\0\2\0", 6);
    while (bytes.size() % 4) bytes += '\0';
    return bytes;
}
std::string json(const std::string &buffer, const std::string &image, size_t byteLength) {
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"translation":[2,3,4],"children":[1,2]},{"mesh":0,"scale":[2,3,1]},{"mesh":0,"scale":[-1,1,1]}],
"buffers":[{)" + buffer + "\"byteLength\":" + std::to_string(byteLength) + R"(}],
"bufferViews":[{"buffer":0,"byteLength":84,"byteStride":28},{"buffer":0,"byteOffset":84,"byteLength":6},
{"buffer":0,"byteOffset":92,"byteLength":)" + std::to_string(png().size()) + R"(}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":0,"byteOffset":12,"componentType":5126,"count":3,"type":"VEC2"},
{"bufferView":0,"byteOffset":20,"componentType":5126,"count":3,"type":"VEC2"},
{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
"images":[{)" + image + R"(}],"samplers":[{"wrapS":33071,"wrapT":33648,"magFilter":9728}],
"textures":[{"source":0,"sampler":0}],"materials":[{"doubleSided":true,"alphaMode":"MASK","alphaCutoff":0.4,
"pbrMetallicRoughness":{"baseColorFactor":[0.8,0.6,0.4,0.5],"baseColorTexture":{"index":0},
"metallicFactor":0.7,"roughnessFactor":0.3,"metallicRoughnessTexture":{"index":0}},
"normalTexture":{"index":0,"texCoord":1,"scale":0.8},"occlusionTexture":{"index":0,"strength":0.6},
"emissiveFactor":[1,0.5,0.25],"emissiveTexture":{"index":0}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TEXCOORD_1":2},"indices":3,"material":0}]}]})";
}
void verify(const Model &m) {
    require(m.meshes.size() == 2, "node instances missing");
    const auto &v = m.meshes[0].vertices;
    require(v.size() == 3 && v[0].position.x == 2 && v[0].position.y == 3 && v[1].position.x == 4 && v[2].position.y == 6,
        "hierarchical/nonuniform transform not applied");
    const auto &mirrored = m.meshes[1].vertices;
    require(mirrored[1].position.y == 4 && mirrored[2].position.x == 1, "mirrored winding not corrected");
    require(std::abs(v[0].attributes.normal.z - 1) < 1e-5, "generated normal incorrect");
    require(std::abs(v[0].attributes.uv.w - .25f) < 1e-5, "UV1 not preserved in Assimp convention");
    require(m.images.size() == 2 && m.images[0].srgb && !m.images[1].srgb, "image dedup / color-space separation failed");
    require(m.images[0].mips.size() == 2 && m.images[0].mips[0].width == 2, "mips missing");
    require(m.images[0].mips[1].rgba[0] > m.images[1].mips[1].rgba[0] + 30, "sRGB mip filter used gamma-space average");
    const auto &mat = m.materials[m.meshes[0].material];
    require(mat.alphaMode == 1 && mat.doubleSided && std::abs(mat.alphaCutoff - .4f) < 1e-5, "alpha mode missing");
    require(mat.textures[Metallic].channel == 2 && mat.textures[Roughness].channel == 1, "packed glTF channels incorrect");
    require(mat.textures[Normal].uvSet == 1 && mat.textures[BaseColor].sampler == 16, "UV set or sampler lost");
    require(std::abs(mat.factors.x - .7f) < 1e-5 && std::abs(mat.factors.w - .6f) < 1e-5, "PBR factors missing");
}
int main(int argc, char **argv) {
    const auto root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() /
        ("nvmatrix-model-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root);
        auto bytes = geometry() + png();
        write(root / "mesh.bin", bytes); write(root / "color.png", png());
        auto external = json("\"uri\":\"mesh.bin\",", "\"uri\":\"color.png\"", bytes.size());
        write(root / "external.gltf", external); verify(loadModel(root / "external.gltf"));
        auto transformed = external;
        transformed.insert(1, R"("extensionsUsed":["KHR_texture_transform"],)");
        const std::string normalTexture = R"("normalTexture":{"index":0,"texCoord":1,"scale":0.8})";
        transformed.replace(transformed.find(normalTexture), normalTexture.size(),
            R"("normalTexture":{"index":0,"texCoord":1,"scale":0.8,"extensions":{"KHR_texture_transform":{"offset":[0.1,0.2],"scale":[2,3],"rotation":1.5707963267948966}}})");
        write(root / "transformed.gltf", transformed);
        auto transformedModel = loadModel(root / "transformed.gltf");
        const auto &binding = transformedModel.materials[transformedModel.meshes[0].material].textures[Normal];
        auto uv = transformedModel.meshes[0].vertices[0].attributes.uv;
        require(std::abs(binding.u.x*uv.z+binding.u.y*uv.w+binding.u.z+2.15f)<1e-5 &&
            std::abs(binding.v.x*uv.z+binding.v.y*uv.w+binding.v.z-.7f)<1e-5, "KHR_texture_transform or V orientation incorrect");
        auto sloped = bytes; const float one = 1;
        std::memcpy(sloped.data() + 16*sizeof(float), &one, sizeof(one));
        write(root / "mesh.bin", sloped);
        auto slopedModel = loadModel(root / "external.gltf");
        const auto normal = slopedModel.meshes[0].vertices[0].attributes.normal;
        require(std::abs(normal.y+1/std::sqrt(10.f))<1e-5 && std::abs(normal.z-3/std::sqrt(10.f))<1e-5,
            "normals did not use inverse-transpose of nonuniform scale");
        write(root / "mesh.bin", bytes);
        auto embedded = json("\"uri\":\"data:application/octet-stream;base64," + base64(bytes) + "\",",
            "\"uri\":\"data:image/png;base64," + base64(png()) + "\"", bytes.size());
        write(root / "embedded.gltf", embedded); verify(loadModel(root / "embedded.gltf"));
        auto glbJson = json("", "\"bufferView\":2,\"mimeType\":\"image/png\"", bytes.size());
        while (glbJson.size() % 4) glbJson += ' ';
        while (bytes.size() % 4) bytes += '\0';
        std::string glb;
        word(glb, 0x46546c67); word(glb, 2); word(glb, uint32_t(28 + glbJson.size() + bytes.size()));
        word(glb, uint32_t(glbJson.size())); word(glb, 0x4e4f534a); glb += glbJson;
        word(glb, uint32_t(bytes.size())); word(glb, 0x004e4942); glb += bytes;
        write(root / "binary.glb", glb); verify(loadModel(root / "binary.glb"));
        write(root / "sample.mtl", "newmtl pbr\nKd 0.8 0.4 0.2\nPm 0.75\nPr 0.25\nmap_Kd color.png\nmap_Pm color.png\nmap_Pr color.png\nnorm color.png\nmap_Ke color.png\nKe 1 1 1\nnewmtl plain\nKd 0.2 0.3 0.4\n");
        write(root / "sample.obj", "mtllib sample.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl pbr\nf -4/1 -3/2 -2/3 -1/4\nusemtl plain\nf 1 3 2\n");
        auto obj = loadModel(root / "sample.obj");
        require(obj.meshes.size() == 2 && obj.meshes[0].vertices.size() == 6, "OBJ triangulation/material groups failed");
        const auto &mat = obj.materials[obj.meshes[0].material];
        require(mat.textures[Metallic].channel == 0 && mat.textures[Roughness].channel == 0 &&
            std::abs(mat.factors.x - .75f) < 1e-5 && std::abs(mat.factors.y - .25f) < 1e-5, "OBJ PBR conversion failed");
        fs::create_directories(root / "materials");
        write(root / "materials/mask.png", png());
        write(root / "materials/nested.mtl", "newmtl nested\nKd 1 1 1\nmap_Kd mask.png\nmap_d mask.png\n");
        write(root / "nested.obj", "mtllib materials/nested.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nusemtl nested\nf 1/1 2/2 3/3\n");
        auto nested = loadModel(root / "nested.obj");
        const auto &nestedMaterial = nested.materials[nested.meshes[0].material];
        require(nestedMaterial.alphaMode == 2 && nestedMaterial.textures[Opacity].image != noTexture,
            "nested MTL or separate opacity map missing");
        auto fails = [&](const fs::path &file, const char *message) {
            try { (void)loadModel(file); } catch (const std::exception &e) {
                require(std::string(e.what()).find(file.filename().string()) != std::string::npos, "error omitted model path"); return;
            }
            throw std::runtime_error(message);
        };
        write(root / "bad.glb", glb.substr(0, 20)); fails(root / "bad.glb", "truncated GLB accepted");
        write(root / "bad.gltf", "{bad json}"); fails(root / "bad.gltf", "malformed JSON accepted");
        fails(root / "missing.obj", "missing OBJ accepted"); fails(root / "mesh.fbx", "unsupported extension accepted");
        fs::remove(root / "color.png"); fails(root / "external.gltf", "missing texture silently ignored");
        write(root / "color.png", "not an image"); fails(root / "external.gltf", "corrupt texture accepted");
        write(root / "color.png", png());
        if (argc == 1) fs::remove_all(root);
        std::cout << "Model asset import tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\nFixtures: " << root << '\n'; return 1;
    }
}
