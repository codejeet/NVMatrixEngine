# NVMatrixEngine 0.1.0 — Water Lab preview

First public engine-only portfolio release. The main demo is a playable room-scale pool with a wall inlet, glass ball, powered boat, underwater views and spectral illumination.

## Included

- Native DX12/DXR camera path tracing and light-side spectral photon caustics.
- GPU APIC / FLIP-PIC liquid, continuous anisotropic scalar-field reconstruction and procedural DXR water geometry.
- Dielectric reflection/refraction, depth-dependent absorption, lasers, surface foam and secondary bubbles/spray.
- DLSS Ray Reconstruction / super resolution, optional frame generation and Reflex.
- RmlUi settings for lighting, camera, flashlight, sink/float density, simulation quality and audio.
- Optional CUDA graph-replay solver, deep-pool narrow-band ownership and ReSTIR PT modes.

## Run

Download **NVMatrixEngine-0.1.0-preview-win64.zip**, extract everything, and open **Play Water Lab.cmd**. Do not download GitHub's source-code ZIP expecting an executable. No installer, administrator access or CUDA Toolkit is needed for the portable build.

Target: Windows 11 x64, high-end NVIDIA RTX GPU and compatible current driver. Tested on **RTX 5090 / driver 616.64**. The executable is unsigned. Fisheye projection pauses frame generation; the default launcher uses the normal lens. See `START-HERE.txt` for controls.

**Known FG limitation:** the release machine reported FG enabled with no SDK error, but zero extra presented frames. The predecessor build reproduced this. The feature remains selectable, but generated output is **not verified in this preview**; the download's verification report records this explicitly. Raw rendering and DLSS Ray Reconstruction are separate checks.

## Scope

This is a research preview, not production middleware or a blanket 60 FPS claim. CUDA/adaptive modes are experimental and not demonstrated faster than the default DX12 solver. ReSTIR PT covers opaque-primary diffuse indirect paths; **ReSTIR BDPT is not implemented**. Camera + photon transport is not a claim of general unbiased BDPT.

The ZIP includes dependency notices, a per-file manifest and the custom soundtrack. The adjacent `.sha256` and `.verification.json` describe the exact archive and relocated-extraction smoke checks. Testing on the development RTX 5090 is not a clean-OS, other-GPU or D3D12 debug-layer certification.

Future work focuses on raw path-tracing latency, stable refractive reuse, and reducing dense/synchronization costs in adaptive water simulation. See the repository roadmap.
