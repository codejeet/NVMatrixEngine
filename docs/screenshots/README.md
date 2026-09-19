# Runtime captures

The original indoor captures below were captured from the standalone NVMatrixEngine build with `engine/capture-portfolio.ps1` on RTX 5090, Windows 11. Output: 1600×900, DLSS Ray Reconstruction Quality, frame generation off, default 65,536 photons/frame and default solver settings. The bounded runs advance the same live GPU solver used by the interactive demo; they are not offline path-traced stills.

| File | Scene / capture frame |
| --- | --- |
| `water-room.png` | Default room, boat and running inlet; frame 180 |
| `wall-inlet.png` | Same room, inlet inspection camera; frame 240 |
| `deep-pool.png` | Larger/deeper pool, DX12 baseline; frame 96 |
| `underwater.png` | First-person beneath 1.4 m water, 400k particles and live inlet; frame 180 |

Those PNG files are lossless conversions of the application's PPM capture with no retouching, compositing or AI generation. The HUD FPS in a bounded capture is not a controlled benchmark and should not be used as one.

`hamiltonian-water.png` was captured separately with `engine/test-hamiltonian.ps1`:
RTX 5090, 960×540 output, DLSS RR Balanced, frame generation off, HOS-2 with
epsilon 0.2, frame 96. It uses the live coupled wave/3D solver and the same
lossless PPM-to-PNG conversion. See [validation](../../engine/hamiltonian-validation.json).

`large-water-lab.png` shows the new 24 × 28 m Water Lab with 1.5 m initial water depth, Hamiltonian HOS-2 and the expanded inspection camera. Captured from the running RTX 5090 build at 1280×720, DLSS Balanced, frame generation off, after 120 frames:

```powershell
NVMatrixFluidLab.exe --water-lab=large --water-path=hamiltonian --normal-lens --boat --fluid-view --quality=balanced --frame-gen=off --width=1280 --height=720 --frames=120 --capture --name=large-water-overview
```

The PNG is a lossless conversion of the runtime PPM capture. [Performance and validation](../../engine/large-water-validation.json).

`hamiltonian-inlet.png` shows water emitted from the large Water Lab spout, reconstructed from simulated free-water particles. Captured at 960×540, DLSS Balanced, frame generation off, using `--water-lab=large --water-path=hamiltonian --inlet-view --fluid-emitter --normal-lens --boat --frames=120`.

`hamiltonian-adaptive-regions.png` shows the activity-driven implementation after 600 frames, with separate 3D regions around wet solids and calm water handled by waves. Captured at 1280×720 with `--water-lab=large --water-path=hamiltonian --fluid-view --normal-lens --boat --quality=balanced --frame-gen=off --frames=600 --capture`. This is a visual regression capture, not an isolated performance measurement.

`ocean-island-day.png` and `ocean-island-night.png` show the 256 × 256 m outdoor
Ocean Island preset. Captured on RTX 5090 at 1280×720, DLSS RR Balanced, frame
generation off, after 180 frames using `engine/test-ocean.ps1`. Day uses the
player camera; night uses `--fluid-view --time-of-day=night` and shows the five
warm pier/path lanterns, their ground illumination and water reflections. These are lossless
PPM-to-PNG conversions. [Scene and physical limits](../../engine/OCEAN_LAB.md),
[GPU validation](../../engine/ocean-validation.json).

`ocean-boat-whitewater.png` shows the 5.8 m boat after the 600-frame ocean
swimming and powered-voyage fixture, at the same rendering settings. The wake
uses the 128² Hamiltonian grid and simulated secondary foam/bubbles/spray.
The 1 m 3D grid still limits bow detail; this is not a paper-quality comparison.

## Ocean boat release 0.1.5

`ocean-boat.png` is the README image for **v0.1.5-preview**, captured from the
updated Windows engine on **2026-09-19 UTC** with an RTX 5090. It shows the powered
boat, rolling-ball player, simulated ocean, island and pier.

```powershell
NVMatrixFluidLab.exe --water-lab=extra-large --normal-lens --boat --quality=quality --frame-gen=off --width=1600 --height=900 --name=release015-ocean-boat
```

The scene used the Ocean preset's **1.8 m amplitude scale, 13 m/s spectrum wind
speed and 48 m minimum initial wavelength**, with the hull-contact clearance fix.
The boat was boarded with **R**, then **E**, and backed away from the pier with
**S**; the camera was adjusted using the normal mouse controls.

The PNG is a direct **1600 × 900 client-area capture** from the live interactive
engine, saved without resizing, retouching, compositing or AI generation.
DLSS Ray Reconstruction was set to **Quality**, with **frame generation off**.
This documentation capture is not a benchmark; release tests and package
validation were skipped. [Release validation](../VALIDATION.md).

Captured executable SHA-256:
`acb12bfc73f253a53f00241e1b202e6426cedc6de2ed9fb205138cf1261cb24a`.
