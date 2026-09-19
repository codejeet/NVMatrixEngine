#include "model_asset.h"
#include <assimp/Importer.hpp>
#include <assimp/DefaultIOSystem.h>
#include <assimp/GltfMaterial.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace lab::asset {
Image decodeImage(const std::string &, const uint8_t *, size_t, bool);
void buildMips(Image &);
namespace {
std::string utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
std::filesystem::path fromUtf8(const std::string &text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
// Preserve the source directory of each MTL material, including libraries in
// subdirectories. Assimp returns bare map paths in its material properties.
struct MaterialIO : Assimp::DefaultIOSystem {
    std::unordered_map<std::string, std::filesystem::path> folders;
    Assimp::IOStream *Open(const char *filename, const char *mode = "rb") override {
        auto *stream = Assimp::DefaultIOSystem::Open(filename, mode);
        auto path = fromUtf8(filename);
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (stream && extension == ".mtl") {
            std::ifstream file(path);
            std::string line;
            while (std::getline(file, line)) {
                std::istringstream row(line); std::string command, name;
                row >> command;
                if (command != "newmtl") continue;
                std::getline(row >> std::ws, name);
                const auto end = name.find_last_not_of(" \t\r");
                if (end != std::string::npos) folders[name.substr(0, end + 1)] = path.parent_path();
            }
        }
        return stream;
    }
};
Float4 vector4(const aiVector3D &v, float w = 0) { return {v.x, v.y, v.z, w}; }
bool finite(const aiVector3D &v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float unit(float v) {
    if (!std::isfinite(v)) throw std::runtime_error("Nonfinite material factor");
    return std::clamp(v, 0.f, 1.f);
}
uint32_t address(aiTextureMapMode mode) {
    if (mode == aiTextureMapMode_Wrap) return 0;
    if (mode == aiTextureMapMode_Mirror) return 2;
    return 1;
}
aiVector3D texcoord(const Attributes &a, const TextureBinding &t) {
    float x = t.uvSet ? a.uv.z : a.uv.x, y = t.uvSet ? a.uv.w : a.uv.y;
    return {t.u.x * x + t.u.y * y + t.u.z, t.v.x * x + t.v.y * y + t.v.z, 0};
}
} // namespace

Model loadModel(const std::filesystem::path &path) {
    try {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const bool gltf = extension == ".gltf" || extension == ".glb";
        if (!gltf && extension != ".obj") throw std::runtime_error("Expected .gltf, .glb or .obj");
        Assimp::Importer importer;
        auto *io = new MaterialIO;
        importer.SetIOHandler(io); // importer owns it
        const aiScene *scene = importer.ReadFile(utf8(path), aiProcess_Triangulate | aiProcess_GenSmoothNormals |
            aiProcess_ValidateDataStructure | aiProcess_SortByPType);
        if (!scene || !scene->mRootNode || !scene->HasMeshes())
            throw std::runtime_error(std::string("Import failed: ") + importer.GetErrorString());
        Model model;
        if (scene->HasAnimations()) model.warnings.emplace_back("Animation is not played; using the static node transforms.");
        std::unordered_map<std::string, uint32_t> images;
        size_t imageBytes = 0, vertexCount = 0;
        auto textureFolder = path.parent_path();
        auto image = [&](const aiString &name, bool srgb) {
            std::string relative = name.C_Str();
            std::replace(relative.begin(), relative.end(), '\\', '/');
            auto filename = (textureFolder / fromUtf8(relative)).lexically_normal();
            if (!std::filesystem::exists(filename)) {
                auto fallback = (path.parent_path() / fromUtf8(relative)).lexically_normal();
                if (std::filesystem::exists(fallback)) filename = fallback;
            }
            const auto *embedded = scene->GetEmbeddedTexture(name.C_Str());
            const auto key = (embedded ? std::string(name.C_Str()) : utf8(filename)) + (srgb ? "|srgb" : "|linear");
            if (auto it = images.find(key); it != images.end()) return it->second;
            if (model.images.size() >= maxTextures) throw std::runtime_error("Model exceeds 512 texture views");
            Image decoded;
            if (embedded) {
                if (!embedded->mHeight)
                    decoded = decodeImage(name.C_Str(), reinterpret_cast<const uint8_t *>(embedded->pcData), embedded->mWidth, srgb);
                else {
                    if (!embedded->mWidth || uint64_t(embedded->mWidth) * embedded->mHeight > 67108864)
                        throw std::runtime_error("Invalid embedded image dimensions");
                    decoded = {name.C_Str(), srgb, {}};
                    Mip mip{embedded->mWidth, embedded->mHeight, {}};
                    mip.rgba.reserve(size_t(mip.width) * mip.height * 4);
                    for (size_t i = 0; i < size_t(mip.width) * mip.height; ++i) {
                        auto p = embedded->pcData[i];
                        mip.rgba.insert(mip.rgba.end(), {p.r, p.g, p.b, p.a});
                    }
                    decoded.mips.push_back(std::move(mip));
                    buildMips(decoded);
                }
            } else {
                std::ifstream file(filename, std::ios::binary | std::ios::ate);
                const auto size = file ? std::streamoff(file.tellg()) : -1;
                if (size <= 0 || size > 268435456) throw std::runtime_error("Missing, empty or oversized texture: " + utf8(filename));
                std::vector<uint8_t> bytes(static_cast<size_t>(size));
                file.seekg(0);
                if (!file.read(reinterpret_cast<char *>(bytes.data()), size)) throw std::runtime_error("Cannot read texture: " + utf8(filename));
                decoded = decodeImage(utf8(filename), bytes.data(), bytes.size(), srgb);
            }
            for (const auto &mip : decoded.mips) imageBytes += mip.rgba.size();
            if (imageBytes > 1024ull * 1024 * 1024) throw std::runtime_error("Model exceeds 1 GiB decoded texture budget");
            const auto index = uint32_t(model.images.size());
            model.images.push_back(std::move(decoded));
            images.emplace(key, index);
            return index;
        };
        for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
            const auto &src = *scene->mMaterials[i];
            aiString materialName; src.Get(AI_MATKEY_NAME, materialName);
            const auto location = io->folders.find(materialName.C_Str());
            textureFolder = location == io->folders.end() ? path.parent_path() : location->second;
            Material m;
            aiColor4D color(1, 1, 1, 1);
            if (src.Get(AI_MATKEY_BASE_COLOR, color) != AI_SUCCESS) src.Get(AI_MATKEY_COLOR_DIFFUSE, color);
            m.baseColor = {unit(color.r), unit(color.g), unit(color.b), unit(color.a)};
            if (!gltf) { float opacity = 1; src.Get(AI_MATKEY_OPACITY, opacity); m.baseColor.w = unit(opacity); }
            aiColor3D emissive(0, 0, 0);
            src.Get(AI_MATKEY_COLOR_EMISSIVE, emissive);
            float intensity = 1; src.Get(AI_MATKEY_EMISSIVE_INTENSITY, intensity);
            if (!finite({emissive.r, emissive.g, emissive.b}) || !std::isfinite(intensity) || intensity < 0)
                throw std::runtime_error("Invalid emissive factor");
            m.emissive = {std::max(0.f, emissive.r * intensity), std::max(0.f, emissive.g * intensity), std::max(0.f, emissive.b * intensity), 0};
            src.Get(AI_MATKEY_METALLIC_FACTOR, m.factors.x);
            if (src.Get(AI_MATKEY_ROUGHNESS_FACTOR, m.factors.y) != AI_SUCCESS) {
                float shininess = 0; src.Get(AI_MATKEY_SHININESS, shininess);
                m.factors.y = std::sqrt(2.f / (std::max(0.f, shininess) + 2));
            }
            m.factors.x = unit(m.factors.x); m.factors.y = unit(m.factors.y);
            src.Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), m.factors.z);
            // Assimp 5.4.3 emits this property under $tex.file.strength.
            if (src.Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0), m.factors.w) != AI_SUCCESS)
                src.Get("$tex.file.strength", aiTextureType_LIGHTMAP, 0, m.factors.w);
            if (!std::isfinite(m.factors.z)) throw std::runtime_error("Invalid normal scale");
            m.factors.w = unit(m.factors.w);
            aiString alpha; src.Get(AI_MATKEY_GLTF_ALPHAMODE, alpha);
            m.alphaMode = alpha == aiString("MASK") ? 1 : (alpha == aiString("BLEND") || (!gltf && m.baseColor.w < 1) ? 2 : 0);
            src.Get(AI_MATKEY_GLTF_ALPHACUTOFF, m.alphaCutoff); m.alphaCutoff = unit(m.alphaCutoff);
            int sided = gltf ? 0 : 1, shading = 0;
            src.Get(AI_MATKEY_TWOSIDED, sided); src.Get(AI_MATKEY_SHADING_MODEL, shading);
            m.doubleSided = sided != 0; m.unlit = gltf && shading == aiShadingMode_Unlit;
            auto binding = [&](TextureSlot slot, std::initializer_list<aiTextureType> types, bool srgb, uint32_t channel = 0) {
                for (auto type : types) {
                    if (!src.GetTextureCount(type)) continue;
                    auto &t = m.textures[slot];
                    aiString name; unsigned uv = 0; aiTextureMapMode modes[3]{aiTextureMapMode_Wrap, aiTextureMapMode_Wrap, aiTextureMapMode_Wrap};
                    aiTextureMapping mapping = aiTextureMapping_UV;
                    if (src.GetTexture(type, 0, &name, &mapping, &uv, nullptr, nullptr, modes) != AI_SUCCESS || mapping != aiTextureMapping_UV || uv > 1)
                        throw std::runtime_error("Only UV0 and UV1 texture mappings are supported");
                    t.image = image(name, srgb); t.uvSet = uv; t.channel = channel;
                    int mag = 9729; src.Get(AI_MATKEY_GLTF_MAPPINGFILTER_MAG(type, 0), mag);
                    t.sampler = address(modes[0]) + 3 * address(modes[1]) + (mag == 9728 ? 9 : 0);
                    aiUVTransform transform;
                    if (src.Get(AI_MATKEY_UVTRANSFORM(type, 0), transform) == AI_SUCCESS) {
                        float sx = transform.mScaling.x, sy = transform.mScaling.y;
                        float r = gltf ? -transform.mRotation : transform.mRotation, c = std::cos(r), s = std::sin(r);
                        float ox = transform.mTranslation.x, oy = transform.mTranslation.y;
                        if (gltf) {
                            ox -= .5f * sx * (-c + s + 1);
                            oy = .5f * sy * (s + c - 1) + 1 - sy - oy;
                            t.u = {c * sx, s * sy, ox - s * sy, 0};
                            t.v = {s * sx, -c * sy, oy + c * sy, 0};
                        } else {
                            t.u = {c * sx, -s * sy, ox, 0};
                            t.v = {-s * sx, -c * sy, 1 - oy, 0};
                        }
                    }
                    return;
                }
            };
            binding(BaseColor, {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}, true);
            binding(Metallic, {aiTextureType_METALNESS}, false, gltf ? 2 : 0);
            binding(Roughness, {aiTextureType_DIFFUSE_ROUGHNESS}, false, gltf ? 1 : 0);
            binding(Normal, {aiTextureType_NORMALS}, false);
            binding(Occlusion, {aiTextureType_AMBIENT_OCCLUSION, aiTextureType_LIGHTMAP, aiTextureType_AMBIENT}, false);
            binding(Emissive, {aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE}, true);
            binding(Opacity, {aiTextureType_OPACITY}, false);
            if (m.textures[Opacity].image != noTexture && !m.alphaMode) m.alphaMode = 2;
            if (src.GetTextureCount(aiTextureType_HEIGHT)) model.warnings.emplace_back("Height/bump maps are not tangent-space normal maps; export an OBJ 'norm' map instead.");
            float transmission = 0; src.Get(AI_MATKEY_TRANSMISSION_FACTOR, transmission);
            if (transmission > 0) model.warnings.emplace_back("Imported transmission is not supported; material uses its metallic/roughness surface.");
            model.materials.push_back(m);
        }
        std::function<void(const aiNode &, const aiMatrix4x4 &, unsigned)> visit;
        visit = [&](const aiNode &node, const aiMatrix4x4 &parent, unsigned depth) {
            if (depth > 256) throw std::runtime_error("Node hierarchy exceeds 256 levels");
            const auto world = parent * node.mTransformation;
            aiMatrix3x3 linear(world), normal(linear);
            const float determinant = linear.Determinant();
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f)
                throw std::runtime_error("Singular node transform");
            normal.Inverse().Transpose();
            for (unsigned i = 0; i < node.mNumMeshes; ++i) {
                const auto &src = *scene->mMeshes[node.mMeshes[i]];
                if (src.HasBones() || src.mNumAnimMeshes) throw std::runtime_error("Skinned/morph meshes require a static export");
                if (!(src.mPrimitiveTypes & aiPrimitiveType_TRIANGLE)) continue;
                Mesh mesh; mesh.name = node.mName.C_Str(); mesh.material = src.mMaterialIndex;
                const auto &material = model.materials.at(mesh.material);
                for (const auto &t : material.textures)
                    if (t.image != noTexture && !src.HasTextureCoords(t.uvSet)) throw std::runtime_error("Textured mesh is missing the requested UV set");
                if (uint64_t(src.mNumFaces) * 3 + vertexCount > 12000000) throw std::runtime_error("Model exceeds 12 million triangle vertices");
                for (unsigned f = 0; f < src.mNumFaces; ++f) {
                    const auto &face = src.mFaces[f];
                    if (face.mNumIndices != 3) continue;
                    std::array<Vertex, 3> triangle;
                    aiVector3D positions[3];
                    for (unsigned k = 0; k < 3; ++k) {
                        const auto index = face.mIndices[determinant < 0 && k ? 3 - k : k];
                        auto &v = triangle[k];
                        positions[k] = world * src.mVertices[index];
                        auto n = normal * src.mNormals[index];
                        if (!finite(positions[k]) || !finite(n) || n.SquareLength() < 1e-20f) throw std::runtime_error("Invalid vertex position or normal");
                        n.Normalize(); v.position = {positions[k].x, positions[k].y, positions[k].z}; v.attributes.normal = vector4(n);
                        const auto &normalMap = material.textures[Normal];
                        if (src.HasTangentsAndBitangents() && !normalMap.uvSet && normalMap.u.x == 1 &&
                            normalMap.u.y == 0 && normalMap.v.x == 0 && normalMap.v.y == -1) {
                            auto t = linear * src.mTangents[index], b = linear * src.mBitangents[index];
                            t -= n * (n * t);
                            if (finite(t) && finite(b) && t.SquareLength() > 1e-20f) {
                                t.Normalize();
                                v.attributes.tangent = vector4(t, ((n ^ t) * b) < 0 ? -1.f : 1.f);
                            }
                        }
                        for (unsigned set = 0; set < 2; ++set) if (src.HasTextureCoords(set)) {
                            auto uv = src.mTextureCoords[set][index];
                            if (!finite(uv)) throw std::runtime_error("Nonfinite UV");
                            if (set == 0) { v.attributes.uv.x = uv.x; v.attributes.uv.y = uv.y; }
                            else { v.attributes.uv.z = uv.x; v.attributes.uv.w = uv.y; }
                        }
                        if (src.HasVertexColors(0)) {
                            auto c = src.mColors[0][index]; v.attributes.color = {unit(c.r), unit(c.g), unit(c.b), unit(c.a)};
                        }
                    }
                    auto e = positions[1] - positions[0], fedge = positions[2] - positions[0];
                    if ((e ^ fedge).SquareLength() < 1e-20f) continue;
                    const auto &normalMap = material.textures[Normal];
                    auto uv0 = texcoord(triangle[0].attributes, normalMap);
                    auto du = texcoord(triangle[1].attributes, normalMap) - uv0, dv = texcoord(triangle[2].attributes, normalMap) - uv0;
                    float den = du.x * dv.y - du.y * dv.x;
                    for (auto &v : triangle) {
                        if (v.attributes.tangent.w != 0) continue;
                        aiVector3D n(v.attributes.normal.x, v.attributes.normal.y, v.attributes.normal.z);
                        aiVector3D t = std::abs(den) > 1e-12f ? (e * dv.y - fedge * du.y) / den : (std::abs(n.y) < .95f ? aiVector3D(0, 1, 0) : aiVector3D(1, 0, 0)) ^ n;
                        t -= n * (n * t);
                        if (t.SquareLength() < 1e-20f) t = (std::abs(n.y) < .95f ? aiVector3D(0, 1, 0) : aiVector3D(1, 0, 0)) ^ n;
                        t.Normalize();
                        auto b = std::abs(den) > 1e-12f ? (fedge * du.x - e * dv.x) / den : n ^ t;
                        v.attributes.tangent = vector4(t, ((n ^ t) * b) < 0 ? -1.f : 1.f);
                    }
                    mesh.vertices.insert(mesh.vertices.end(), triangle.begin(), triangle.end());
                }
                vertexCount += mesh.vertices.size();
                if (!mesh.vertices.empty()) model.meshes.push_back(std::move(mesh));
            }
            for (unsigned i = 0; i < node.mNumChildren; ++i) visit(*node.mChildren[i], world, depth + 1);
        };
        visit(*scene->mRootNode, aiMatrix4x4(), 0);
        if (model.meshes.empty()) throw std::runtime_error("No nondegenerate triangles in model");
        return model;
    } catch (const std::exception &error) {
        throw std::runtime_error("Model '" + utf8(path) + "': " + error.what());
    }
}
} // namespace lab::asset
