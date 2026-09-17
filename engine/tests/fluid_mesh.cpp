#include "../src/fluid/mesh_sdf.h"
#include "../src/scene.h"
#include <iostream>
int main() {
    try {
        auto meshes = lab::makePlayScene(false);
        auto bake = [&](uint32_t i) {
            std::vector<DirectX::XMFLOAT3> points;
            for (auto &v : meshes[i].vertices)
                points.push_back(v.position);
            return lab::MeshSdfAsset::bake(points, .04f);
        };
        const auto cube = bake(3), prism = bake(2);
        meshes = lab::makePlayScene(false, false, true);
        if (meshes.size() != 7 || meshes.back().mask != 2)
            throw std::runtime_error("Glass pit is not a separate transmissive instance");
        for (const auto &v : meshes.back().vertices)
            if (v.material != 10 || v.chart != ~0u)
                throw std::runtime_error("Glass pit incorrectly participates in diffuse receivers");
        // The import baker independently checks watertightness and oriented edges.
        std::vector<DirectX::XMFLOAT3> shellPoints;
        for (const auto &v : meshes.back().vertices)
            shellPoints.push_back(v.position);
        const auto shell = lab::MeshSdfAsset::bake(shellPoints, .1f);
        for (const auto p : {DirectX::XMFLOAT3{1.7f, .6f, -3.1f},
                             {5.7f, .6f, -3.1f},
                             {3.7f, .6f, -5.f},
                             {3.7f, .6f, -1.2f},
                             {1.7f, .6f, -5.f}})
            if (shell.sample(p) >= -.045f)
                throw std::runtime_error("Glass shell interior/corner missing");
        if (shell.sample({3.7f, .6f, -3.1f}) <= 0 || shell.sample({1.7f, 1.4f, -3.1f}) <= 0)
            throw std::runtime_error("Glass shell filled the cavity or capped the pool");
        double signedVolume = 0;
        for (size_t i = 0; i < shellPoints.size(); i += 3) {
            auto a = DirectX::XMLoadFloat3(&shellPoints[i]), b = DirectX::XMLoadFloat3(&shellPoints[i + 1]),
                 c = DirectX::XMLoadFloat3(&shellPoints[i + 2]);
            signedVolume +=
                DirectX::XMVectorGetX(DirectX::XMVector3Dot(a, DirectX::XMVector3Cross(b, c))) / 6.;
        }
        if (std::abs(signedVolume - (4.2 * 4.0 - 3.8 * 3.6) * 1.2) > .00002)
            throw std::runtime_error("Glass shell orientation/volume mismatch");
        // The cube's medial axis is a nondifferentiable distance maximum;
        // trilinear sampling can differ there by half a voxel, not near a face.
        if (std::abs(cube.sample({0, 0, 0}) + .38f) > .0201f ||
            std::abs(cube.sample({.5f, 0, 0}) - .12f) > .0001f || prism.sample({0, 0, 0}) >= 0)
            throw std::runtime_error("Mesh SDF inside/outside/distance test failed");
        for (float x : {-.8f, -.45f, 0.f, .45f, .8f}) {
            const float expected = std::abs(x) - .38f;
            if (std::abs(cube.sample({x, 0, 0}) - expected) > (x == 0 ? .0201f : .0001f))
                throw std::runtime_error("Mesh SDF signed distance mismatch");
        }
        bool rejected = false;
        try {
            lab::MeshSdfAsset::bake({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, .1f);
        } catch (const std::runtime_error &) {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("Open mesh incorrectly accepted as a solid");
        std::cout << "PASS closed mesh distance/sign, prism interior, open-mesh rejection\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
