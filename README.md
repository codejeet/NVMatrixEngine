# NVMatrixEngine

**A native real-time simulation and spectral light-transport engine for high-end NVIDIA RTX GPUs.**

NVMatrixEngine brings GPU liquid simulation, procedural ray-traced water, spectral caustics, rigid-body interaction, and DLSS Ray Reconstruction together in a custom DirectX 12 renderer. The flagship **Water Lab** is a playable, room-scale pool: open the wall inlet, roll or dive as a glass ball, float a powered boat, and watch animated water redirect light onto the white grid floor.

[Download the Windows demo](https://github.com/codejeet/NVMatrixEngine/releases/tag/v0.1.0-preview) · [Architecture](docs/ARCHITECTURE.md) · [Build instructions](docs/BUILDING.md) · [Roadmap](docs/ROADMAP.md)

![NVMatrixEngine Water Lab: path-traced water, glass and spectral illumination](docs/screenshots/water-room.png)

This is an independently developed **research/portfolio preview**, not a finished middleware product. The source includes the native engine and the shared code needed by its demos; it does not include the predecessor browser renderer or its application. NVIDIA does not sponsor or endorse this project.

## What is running today

| System | Implemented capability |
| --- | --- |
| Native rendering | Custom C++20 / DX12 / DXR renderer; camera and photon `DispatchRays` passes; capability-gated Shader Execution Reordering |
| Light transport | Camera path tracing plus light-side spectral photon tracing; reflection, refraction, Fresnel, total internal reflection, dispersion, and texture-space caustics |
| Water simulation | GPU APIC or FLIP/PIC, staggered MAC velocities, pressure projection, moving solid boundaries, particle advection, viscosity and surface-tension models |
| Continuous water geometry | Anisotropic particle reconstruction into a sparse brick scalar field; procedural AABB BLAS/TLAS; cell-wise root-refined DXR intersections |
| Water optics | Animated dielectric boundaries, depth-dependent Beer–Lambert absorption, underwater views, and laser single scattering |
| Reconstruction / presentation | DLSS Ray Reconstruction with super resolution, optional DLSS Frame Generation / Multi Frame Generation, and Reflex integration |
| Interaction | Bullet rigid bodies, glass ball sink/float density, GPU-sampled buoyancy, powered boat, wall emitter, bounded foam/bubbles/spray |
| Interface | RmlUi settings, lighting presets, flashlight, lens/view modes, simulation sliders, audio, debug overlays and GPU timings |
| Research modes | CUDA/DX12 interop, graph replay, mixed-resolution pressure, live narrow-band particle/grid ownership, adaptive optical sampling, and RTXDI-based ReSTIR PT |

### Light transport, in plain terms

The renderer traces from **both the camera and the lights**, combining eye-path illumination with light-side photons that have passed through glass or water. This makes difficult refractive caustics practical without asking random camera paths to discover every focused light path.

That is a hybrid, two-sided transport architecture—not a claim that general bidirectional path tracing with vertex connection/MIS, or **ReSTIR BDPT**, is complete. The optional **ReSTIR PT** mode is implemented for multi-bounce diffuse indirect lighting from opaque primary surfaces. It does not yet reuse GI behind refractive camera prefixes. [Algorithm details and scope](docs/ARCHITECTURE.md#light-transport).

## Water Lab

### Live recordings

Recorded from the running engine at 720p / 30 fps with DLSS RR Quality and frame generation off. The main showcase uses a solid sinking ball, ordinary roll/jump inputs and a following camera. These are visual demonstrations, not FPS benchmarks. [Capture setup](docs/videos/README.md).

**Water room — sinking glass ball rolling, jumping and disturbing the water:**

https://github.com/user-attachments/assets/9f3146c7-efc4-493c-8bf2-2a3a0286eda1

**Underwater — first-person view beneath the animated dielectric surface:**

https://github.com/user-attachments/assets/9dcae60b-1e34-4413-a2ed-c8f541ffc389

**Wall inlet — animated fluid refraction and surface interaction:**

https://github.com/user-attachments/assets/58ea2f2e-5936-4e31-9a43-6acbeb748464

![Wall inlet feeding the simulated pool](docs/screenshots/wall-inlet.png)

Water is simulated in three dimensions, not as a displaced plane. Simulation particles carry motion; a MAC grid enforces approximate incompressibility; a reconstructed continuous field is the geometry seen by camera rays, shadow rays, and caustic photons. There is no screen-space water surface or marching-cubes mesh in the primary rendering path.

The default room starts with approximately **100,000 particles**. Settings reserve up to **1 million particles**, expose MAC cell spacing and simulation frequency, and keep filling bounded: the inlet closes at capacity instead of silently deleting water. Higher capacity is not the same as higher active particle count.

The main release launcher uses the **DX12 uniform-grid baseline**. CUDA and adaptive modes are opt-in because their additional machinery is not yet a demonstrated end-to-end speedup in this scene. The larger deep-water pool is an optional stress/demo preset, not the default performance target.

**Deep pool — a larger body of water with calmer interior regions:**

https://github.com/user-attachments/assets/74b52c64-3163-4d68-ab98-331f22f1a4bf

![Larger deep-water pool research scenario](docs/screenshots/deep-pool.png)

### Try it

1. Download the ZIP from the [preview release](https://github.com/codejeet/NVMatrixEngine/releases/tag/v0.1.0-preview).
2. Extract the **entire** folder to a writable location; do not run inside the ZIP.
3. Open **Play Water Lab.cmd**, or double-click `NVMatrixFluidLab.exe` for the default water room.
4. Press **Esc** for lighting, lens, buoyancy, water quality, frame generation, and music settings.

No installer, administrator access, CUDA Toolkit, Visual Studio, or SDK downloads are required to run the packaged demo. A current compatible NVIDIA graphics driver is required. This preview is unsigned; Windows may display a reputation warning. Only use release assets from this repository and verify the accompanying SHA-256 checksum if needed.

| Controls | Action |
| --- | --- |
| WASD / Space | Roll the glass ball / jump |
| Right-drag / wheel | Orbit or look / zoom |
| Tab / Ctrl | First-person view / dive |
| T / I | Toggle the wall inlet / inspect the inlet |
| E | Interact, grab, or board/exit the boat near it |
| W/S, A/D aboard | Thrust and steering |
| P / period / B | Pause water / single step / reset water |
| Esc / U | Settings / additional HUD detail |
| F8 | Cycle supported frame-generation modes |

The equisolid fisheye is selectable in settings. It models a lens projection, not a full compound lens assembly. **Frame generation pauses in fisheye mode**; the portable launcher starts with the normal lens so FG can operate.

The glass ball starts as **solid glass in Sink mode**. Esc → Ball buoyancy switches to the lighter hollow Float mode without resetting the scene.

## Hardware and performance

- Windows 11 x64 and a high-end NVIDIA RTX GPU are the intended platform.
- The release is validated on an **RTX 5090**. Other GPUs are not certified by this preview.
- DLSS feature availability is queried at runtime. Frame generation depends on GPU, driver, OS configuration, and supported presentation mode; it does not accelerate simulation or raw rendering.
- **Preview FG caveat:** the release check loaded FG/Reflex without errors but recorded no extra presented frames on the test PC; the predecessor executable reproduced this. FG remains selectable, but generated output is **not verified for this release**. See [validation](docs/VALIDATION.md).
- Optional CUDA kernels are built for SM 86, 89, and 120. This is architecture coverage, not evidence of equivalent performance or validation on every card.
- Resolution, active particles, fluid depth, inlet activity, photon budget, and experimental solvers materially affect frame time. There is **no blanket 60 FPS guarantee**.

The engine reports raw rendering/simulation timings separately from generated presentation frames. See [release validation and measurement scope](docs/VALIDATION.md). The project prioritizes reproducible comparisons over multiplying an FPS counter by the frame-generation factor.

## Engine map

```text
engine/src/renderer.*       DX12 resources, DXR pipelines, frame orchestration
engine/src/fluid/           GPU liquid subsystem, surface, pressure, CUDA, buoyancy
engine/shaders/             HLSL transport, caustics, reconstruction and fluid kernels
engine/src/streamline.*     DLSS RR / FG and Reflex integration
engine/src/gameplay.*       Demo interaction and rigid-body integration
engine/ui/                 RmlUi markup and styling
engine/tests/              Native numerical, gameplay and GPU fixtures
shared/                    Minimal common audio, UI, camera and rigid-body support
docs/                      Current architecture, build guide, captures and roadmap
release/                   Portable launchers and packaging inputs
```

See [BUILDING.md](docs/BUILDING.md) to build the DX12-only or optional CUDA configuration. Algorithm experiments and their evidence are retained in [the engine notes](engine/README.md); older checkpoint statements describe their date, not necessarily the current integrated feature set.

## Next steps

The next major work is **path-tracing optimization** and **water-simulation optimization**: reduce procedural intersection and photon cost, improve stable reuse through refraction, bound adaptive topology work, and make coarse bulk ownership save actual frame time. Full ReSTIR BDPT, fully sparse multiresolution simulation, and general multiple-scattering liquids remain future work, not shipping claims. [Detailed roadmap and acceptance criteria](docs/ROADMAP.md).

## Credits and terms

Built around original engine integration with NVIDIA Streamline/DLSS, NVAPI, RTXDI PT, the DirectX 12 Agility SDK, Bullet, RmlUi, FreeType, and miniaudio. Spectral conversion uses the [CIE 1931 standard-observer dataset](https://doi.org/10.25039/CIE.DS.xvudnb9b). The demo includes a custom soundtrack; startup selection is randomized and volume defaults low.

Public source is available for portfolio review; **an open-source license has not been granted**. See [COPYRIGHT.md](COPYRIGHT.md) and [third-party notices](THIRD_PARTY_NOTICES.md). Third-party components and the CIE dataset retain their own licenses. SDK caches and development credentials are not included in this repository.
