# Loading 3D models

The DXR renderer loads static `.gltf`, `.glb`, and `.obj` objects with PBR materials. Run `engine/setup.ps1` and rebuild to fetch the pinned Assimp dependency.

```powershell
NVMatrixFluidLab.exe --model="C:\Models\Helmet.glb" --model-y=2 --model-scale=1
NVMatrixFluidLab.exe --model="assets\chair.obj" --model="assets\table.gltf" --model-x=-2
```

Paths resolve from the directory in which the application was launched, including paths containing spaces or Unicode. Repeat `--model` to load multiple files. `--model-scale` (positive, default 1) and `--model-x`, `--model-y`, `--model-z` (default 0) apply to all imported objects after their node transforms. Coordinates remain right handed, Y up; one engine unit is one metre. OBJ has no intrinsic unit convention, so use the scale flag when needed.

Objects are added to the current lab. They participate in visibility, shadows, reflection and refraction, and remain static. Generic model additions do not create Bullet bodies or fluid colliders. The Neon Night preset explicitly builds static Bullet triangle collisions from its imported meshes. The existing lab camera and controls remain available.

## Materials and images

- glTF metallic/roughness factors and textures: base color, metallic (blue), roughness (green), tangent-space normals, occlusion (red), and emissive. Base color and emissive images use sRGB; data maps use linear values. Vertex color multiplies base color.
- OBJ/MTL `Kd`, `Ke`, `d`, `Pm`, `Pr`, `map_Kd`, `map_Ke`, `map_Pm`, `map_Pr`, `norm`/`map_Kn`, `map_Ka` (occlusion), and `map_d` (opacity). Without `Pr`, `Ns` is converted to roughness. Separate metallic and roughness maps use red. Texture paths resolve relative to the MTL, with the OBJ directory as a fallback.
- External PNG/JPEG images, glTF data URIs, and GLB embedded images. The bundled image decoder also accepts common OBJ image formats such as TGA and BMP. Missing or corrupt referenced images report an error with the model path.
- UV0 and UV1, per-texture UV selection, and `KHR_texture_transform` offset/rotation/scale. Select the UV set with the texture's `texCoord`; the extension's optional `texCoord` override is not supported by the pinned importer.
- Repeat, clamp and mirrored addressing. The magnification filter selects nearest or linear filtering for the mip chain. Independent minification/magnification filter combinations are approximated.
- Color-correct CPU mip generation and ray-cone LOD selection. Referencing an image in both color and data slots creates separate color-space views.
- Opaque, alpha-mask and stochastic alpha-blend coverage, including shadow rays; glTF double-sided materials. OBJ surfaces default to two-sided. Blend coverage converges over frames and is not dielectric transmission.

Node hierarchy and instance transforms are baked into triangle lists. Nonuniform scales use inverse-transpose normals; mirrored transforms correct winding. Authored normals/tangents are preserved where applicable; missing normals and normal-map tangent frames are generated. Quads/polygons in OBJ are triangulated.

Imported camera surfaces use a bounded GGX metallic/roughness path with direct lighting and reflection. The existing diffuse ReSTIR reservoir remains unchanged; imported primary hits use the PBR path when ReSTIR is enabled. Imported emissive triangles are registered as power-weighted area lights. Direct samples evaluate emissive textures and alpha coverage, trace visibility, and use multiple importance sampling against BSDF hits on imported paths. Unlit base color remains a visible-only shortcut; use an emissive material for a light source.

## Current limits

Animation is loaded at its static node pose with a warning. Skinning and morph targets require a static export. Draco/meshopt geometry, KTX2/Basis textures, displacement/bump maps, specular/glossiness materials, transmission, clearcoat, sheen and other layered material extensions are outside this path. Transmission and bump maps produce warnings. Unlit and emissive-strength glTF materials are supported.

The scene accepts up to 512 imported texture views and 12 million expanded imported vertices. Each model is limited to 1 GiB of decoded texture/mip data; an image is limited to 16,384 pixels per dimension and 64 million pixels. Geometry/images are imported at startup; there is no hot reload or asset editor yet.

## Importer checks

The normal Windows CTest suite includes `lab_model_assets`. The importer can also be tested on Linux without DirectX or a GPU, after extracting the pinned Assimp archive into `shared/.deps/assimp-5.4.3`:

```sh
cmake -S engine -B /tmp/nvmatrix-assets -DNVMATRIXENGINE_ASSET_TESTS_ONLY=ON
cmake --build /tmp/nvmatrix-assets -j 4
ctest --test-dir /tmp/nvmatrix-assets --output-on-failure
```

Tests generate their own small models and images, covering all three formats, external/data-URI/GLB images, material channels, mip color spaces, node transforms, mirrored winding, UV transforms, nested MTL paths, and malformed/missing inputs. Passing a directory to `NVMatrixEngineModelAssetTest` keeps the generated fixtures there for renderer smoke tests.
