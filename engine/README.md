# Engine implementation notes

Start with the [project README](../README.md), [current architecture](../docs/ARCHITECTURE.md), and [build guide](../docs/BUILDING.md).

The standalone application is **NVMatrixFluidLab.exe**. With no arguments it opens the water room. Portable release launchers use the normal lens and DX12 fluid backend. Small Water Lab defaults to legacy fluid simulation; Large and Ocean default to Hamiltonian waves with local 3D activity regions.

## Current subsystem references

- [Extra Large Water Lab: outdoor ocean, beach island and day/night HDRIs](OCEAN_LAB.md)
- [Hamiltonian nonlinear waves and two-way 3D coupling](HAMILTONIAN_WATER.md)
- [Live narrow-band particle/grid ownership](NARROW_BAND.md)
- [Fluid system integration](FLUID_IMPLEMENTATION.md)
- [Continuous surface and traversal tests](fluid-surface.test.mjs)
- [Room emitter and whitewater](ROOM_WATER.md), [surface foam](SURFACE_FOAM.md)
- [Camera, lighting and buoyancy](EXPERIENCE.md)
- [RTXDI ReSTIR PT integration](FLUID_RENDER_RESEARCH.md)
- [Adaptive optical importance](ADAPTIVE_OPTICS.md)
- [CUDA and pressure experiments](CUDA_FLUID.md)
- [Water-shadow temporal stability](WATER_SHADOW_STABILITY.md)

## Reading historical evidence

The extensive `.md` notes and `*-validation.json` files document incremental research checkpoints. A statement such as “not implemented yet” describes that checkpoint, and may be superseded by a later note. Stored hashes refer to the pre-export development snapshots; public-export branding and path cleanup intentionally change source hashes. Machine-specific capture paths have been replaced with placeholders.

[HISTORY.md](HISTORY.md) preserves the earlier running integration log. References there to a predecessor game, branch or omitted admin helper are historical context, not an extra build dependency or a promised public component. The root architecture/roadmap and per-release verification are the public preview's current contract.

Use [Play Large Water Lab.cmd](Play%20Large%20Water%20Lab.cmd) for a 24 × 28 m basin with 1.5 m water, the Water Lab props/lighting, and adaptive Hamiltonian/3D regions around submerged bodies and disturbed water. [Details and comparison commands](HAMILTONIAN_WATER.md#large-water-lab).

The large-room preset (`--water-lab=large`) defaults to Hamiltonian water; the small Water Lab defaults to the legacy full 3D solver. Override either with `--water-path=baseline` or `--water-path=hamiltonian`.
