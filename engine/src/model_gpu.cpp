#include "renderer.h"
#include <cstring>
#include <limits>

namespace lab {
void Renderer::loadModels() {
    std::vector<asset::Material> materials;
    std::vector<asset::Attributes> attributes;
    // Entry zero stores the count; remaining entries are a power-weighted CDF.
    struct Light { asset::Float4 pArea, e1Cdf, e2Pdf; uint32_t a, b, c, material; };
    static_assert(sizeof(Light) == 64);
    std::vector<Light> lights(1);
    std::vector<double> weights;
    double totalPower = 0;
    // Every descriptor is valid even when the scene has no imported objects.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    for (uint32_t i = 0; i < asset::maxTextures; ++i)
        device->CreateShaderResourceView(nullptr, &srv, cpu(32 + i));
    for (const auto &path : options.models) {
        auto model = asset::loadModel(path);
        for (const auto &warning : model.warnings) logLine("Model: " + warning);
        if (modelTextures.size() + model.images.size() > asset::maxTextures)
            throw std::runtime_error("Scene exceeds 512 imported texture views");
        if (materials.size() + model.materials.size() > UINT32_MAX - asset::materialBase)
            throw std::runtime_error("Too many imported materials");
        const auto materialOffset = uint32_t(materials.size()), textureOffset = uint32_t(modelTextures.size());
        for (auto &m : model.materials) {
            for (auto &t : m.textures) if (t.image != asset::noTexture) t.image += textureOffset;
            materials.push_back(m);
        }
        for (const auto &source : model.meshes) {
            if (attributes.size() + source.vertices.size() > 12000000)
                throw std::runtime_error("Scene exceeds 12 million imported triangle vertices");
            Mesh mesh; mesh.mask = 1; mesh.vertices.reserve(source.vertices.size());
            mesh.opaque = model.materials[source.material].alphaMode == 0;
            mesh.doubleSided = model.materials[source.material].doubleSided != 0;
            for (const auto &v : source.vertices) {
                const XMFLOAT3 p{v.position.x * options.modelScale + options.modelPosition.x,
                    v.position.y * options.modelScale + options.modelPosition.y,
                    v.position.z * options.modelScale + options.modelPosition.z};
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                    throw std::runtime_error("Model placement produces nonfinite positions");
                mesh.vertices.push_back({p, asset::materialBase + materialOffset + source.material,
                    {v.attributes.uv.x, v.attributes.uv.y}, ~0u, uint32_t(attributes.size())});
                attributes.push_back(v.attributes);
            }
            const auto &material = materials[materialOffset + source.material];
            XMFLOAT3 emission{material.emissive.x, material.emissive.y, material.emissive.z};
            if (material.textures[asset::Emissive].image != asset::noTexture) {
                const auto &image = model.images[material.textures[asset::Emissive].image - textureOffset];
                const auto &average = image.mips.back().rgba;
                auto linear = [&](int c) {
                    float v = average[c] / 255.f;
                    return image.srgb ? (v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f)) : v;
                };
                emission.x *= linear(0); emission.y *= linear(1); emission.z *= linear(2);
            }
            const double luminance = .2126 * emission.x + .7152 * emission.y + .0722 * emission.z;
            if (luminance > 0) for (size_t i = 0; i < mesh.vertices.size(); i += 3) {
                const auto &a = mesh.vertices[i], &b = mesh.vertices[i + 1], &c = mesh.vertices[i + 2];
                auto e1 = XMVectorSubtract(XMLoadFloat3(&b.position), XMLoadFloat3(&a.position));
                auto e2 = XMVectorSubtract(XMLoadFloat3(&c.position), XMLoadFloat3(&a.position));
                float area = .5f * XMVectorGetX(XMVector3Length(XMVector3Cross(e1, e2)));
                if (area <= 1e-12f) continue;
                XMFLOAT3 u, v; XMStoreFloat3(&u, e1); XMStoreFloat3(&v, e2);
                lights.push_back({{a.position.x, a.position.y, a.position.z, area},
                    {u.x, u.y, u.z, 0}, {v.x, v.y, v.z, 0}, a.pad, b.pad, c.pad, materialOffset + source.material});
                double power = area * luminance * (material.doubleSided ? 2 : 1);
                weights.push_back(power); totalPower += power;
            }
            meshes.push_back(std::move(mesh));
        }
        // Fence each texture upload before releasing its staging allocation.
        for (const auto &image : model.images) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = image.mips[0].width; desc.Height = image.mips[0].height;
            desc.DepthOrArraySize = 1; desc.MipLevels = UINT16(image.mips.size());
            desc.Format = image.srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> texture;
            gpu::check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "Create model texture");
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(desc.MipLevels);
            uint64_t bytes = 0;
            device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, footprints.data(), nullptr, nullptr, &bytes);
            auto staging = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
            for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
                const auto &src = image.mips[mip]; const auto &fp = footprints[mip];
                for (uint32_t y = 0; y < src.height; ++y)
                    memcpy(static_cast<uint8_t *>(staging.mapped) + fp.Offset + size_t(y) * fp.Footprint.RowPitch,
                        src.rgba.data() + size_t(y) * src.width * 4, size_t(src.width) * 4);
            }
            begin();
            for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = staging.resource.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                from.PlacedFootprint = footprints[mip];
                to.pResource = texture.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; to.SubresourceIndex = mip;
                commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
            gpu::transition(commands.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            submit(false);
            srv.Format = desc.Format; srv.Texture2D.MipLevels = desc.MipLevels;
            device->CreateShaderResourceView(texture.Get(), &srv, cpu(32 + uint32_t(modelTextures.size())));
            modelTextures.push_back(std::move(texture));
        }
        logLine("Imported model: " + std::to_string(model.meshes.size()) + " meshes, " +
            std::to_string(model.materials.size()) + " materials, " + std::to_string(model.images.size()) + " texture views");
    }
    if (!std::isfinite(totalPower) || totalPower > std::numeric_limits<float>::max())
        throw std::runtime_error("Imported emission exceeds finite GPU power limits");
    double cumulative = 0;
    for (size_t i = 1; i < lights.size(); ++i) {
        auto &light = lights[i];
        cumulative += weights[i - 1] / totalPower;
        light.e1Cdf.w = i + 1 == lights.size() ? 1.f : float(cumulative);
        light.e2Pdf.w = float(weights[i - 1] / totalPower / light.pArea.w);
        attributes[light.a].normal.w = attributes[light.b].normal.w = attributes[light.c].normal.w = light.e2Pdf.w;
    }
    lights[0].pArea.x = float(lights.size() - 1);
    lights[0].pArea.y = float(totalPower);
    modelLights = staticBuffer(lights.data(), lights.size() * sizeof(Light));
    logLine("Imported emissive triangles: " + std::to_string(lights.size() - 1));
    if (materials.empty()) materials.emplace_back();
    if (attributes.empty()) attributes.emplace_back();
    modelMaterials = staticBuffer(materials.data(), materials.size() * sizeof(asset::Material));
    modelAttributes = staticBuffer(attributes.data(), attributes.size() * sizeof(asset::Attributes));
}
} // namespace lab
