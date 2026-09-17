# Runtime captures

Captured from the standalone NVMatrixEngine build with `engine/capture-portfolio.ps1` on RTX 5090, Windows 11. Output: 1600×900, DLSS Ray Reconstruction Quality, frame generation off, default 65,536 photons/frame and default solver settings. The bounded runs advance the same live GPU solver used by the interactive demo; they are not offline path-traced stills.

| File | Scene / capture frame |
| --- | --- |
| `water-room.png` | Default room, boat and running inlet; frame 180 |
| `wall-inlet.png` | Same room, inlet inspection camera; frame 240 |
| `deep-pool.png` | Larger/deeper pool, DX12 baseline; frame 96 |
| `underwater.png` | First-person beneath 1.4 m water, 400k particles and live inlet; frame 180 |

PNG files are lossless conversions of the application's PPM capture with no retouching, compositing or AI generation. The HUD FPS in a bounded capture is not a controlled benchmark and should not be used as one.
