# Third-party components

Runtime and source dependencies retain their own copyrights and licenses. Setup downloads pinned SDKs into ignored `shared/.deps`; those SDK caches are not included in Git. The portable release includes the applicable license files under `licenses/`.

| Component | Purpose / notice |
| --- | --- |
| NVIDIA Streamline 2.12.0 | DLSS / Reflex integration; `Streamline-LICENSE.txt` |
| NVIDIA DLSS / NGX binaries | Production SDK runtime modules; `NVIDIA-RTX-SDK-LICENSE.txt`; not standalone redistributable SDKs |
| NVIDIA NVAPI | SER / atomics / capabilities; `NVAPI-LICENSE.txt` |
| NVIDIA RTXDI Library | ReSTIR PT kernels; `RTXDI-LICENSE.txt` |
| NVIDIA CUDA 13.1 | Optional solver, statically linked redistributable runtime; `CUDA-EULA.txt` |
| Microsoft DX12 Agility 1.619.5 | App-local D3D12 runtime; `Agility-LICENSE.txt`, `Agility-LICENSE-CODE.txt` |
| Microsoft PIX runtime | GPU instrumentation; `PIX-LICENSE.txt`, `PIX-ThirdPartyNotices.txt` |
| Microsoft Visual C++ runtime | App-local dependencies; `Microsoft-VC-REDIST.txt` |
| Bullet 3.25 | Rigid bodies; `Bullet-LICENSE.txt` |
| RmlUi 6.3 | Native markup/style UI; `RmlUi-LICENSE.txt` |
| FreeType 2.14.1 | Font rasterization; `FreeType-LICENSE.txt` |
| miniaudio 0.11.25 | Audio playback; `miniaudio-LICENSE.txt` |
| Poppins font | UI typography; `Font-LICENSE.txt` |
| Poly Haven sky HDRIs | CC0 day/night environments; [sources and attribution](engine/assets/ocean/README.md) |

## CIE observer dataset

CIE (2019), *Colour-matching functions of CIE 1931 standard colorimetric observer*, International Commission on Illumination, Vienna, DOI [10.25039/CIE.DS.xvudnb9b](https://doi.org/10.25039/CIE.DS.xvudnb9b).

The unmodified `assets/CIE_xyz_1931_2deg.csv` is licensed **CC BY-SA 4.0**, as recorded in the [publisher's metadata](https://files.cie.co.at/Publications-datasets/CIE_xyz_1931_2deg.csv_metadata.json). License: https://creativecommons.org/licenses/by-sa/4.0/. Dataset SHA-256: `fa663e3535a7e0763a745993a1f0a192eb0275ac46ad2d1befd7626841e713c1`. Runtime lookup/interpolation does not replace the packaged source dataset. The dataset's license is not a license for the engine code.

## Soundtrack

The demo's five custom music tracks were provided by the project owner: AFTERIMAGE, the untitled September 2026 instrumental, Velvet Circuit, Chrome Honey, and Resurgence Loop. They and the custom event cues are included for demo playback, not separately licensed as a music library. Startup music selection is randomized. These assets remain reserved to their respective rights holders.
