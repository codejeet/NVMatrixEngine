# GPU-driven fluid execution tiles

Integration map for the adaptive specification's GPU execution and profiling
requirements. This milestone replaces uniform **execution**, not the existing
dense transfer-grid storage or the physical pressure operator.

| Existing system | Connection |
| --- | --- |
| `FluidSystem::bin`, `CellCounts` / `CellQuanta` | Exact current occupied cells drive conservative quadratic-support face tiles, including particle-free dormant owners. |
| `transfer.hlsl` P2G | Original gather arithmetic runs through `ExecuteIndirect` on compact 8×4×4 MAC-face tiles. Faces outside the current footprint are cleared, never reused stale. |
| `density.hlsl` | Existing assembled stencil selects compact pressure tiles. Two global Jacobi iterates retain their one-step halo and pressure connections across tile boundaries. |
| `FluidMac` / `FluidMacPressure` | Global fine/coarse pressure coupling remains unchanged; empty scheduling does not truncate incompressibility. |
| `gpu_resources.h`, fluid root signature, shader compiler | Persistent `FluidWork` buffers, original compute PSO infrastructure, root UAVs and shared dispatch convention. |
| Existing renderer frame fence | Deferred counters, phase timestamps and opt-in coverage snapshots; no substep/iteration CPU decisions. |
| Existing particle/grid debug view | F12/`--fluid-work-view`: cyan P2G footprint, orange density work, white overlap. Pause with the existing simulation controls to inspect it. |
| Surface reconstruction → DXR → caustics → DLSS | Consume unchanged physical output. No new surface approximation or radiance filtering. |

`--fluid-sparse-work` selects compact execution with either the normal or mixed
pressure solver. `--fluid-work-validate` additionally audits coverage per frame;
ordinary `--fluid-validate` audits the final frame. The normal launcher is not
changed before correctness and cost comparisons are complete.

From the lab executable directory:

```powershell
.\NVMatrixFluidLab.exe --fluid-room --fluid-emitter --fluid-sparse-work
```

F12 selects work coverage; V hides/shows the debug overlay. P pauses liquid and
`.` advances one simulation step while paused. `--fluid-work-view` opens with
the coverage overlay enabled and a fluid inspection camera.

The exact current occupied-cell support takes precedence over visibility or
requested LOD. This is necessary even for hidden water, moving colliders and
fine/coarse transitions. The scalar field and flowing coarse bulk remain
separate adaptive milestones.

`fluid.work` exposes active/capacity tile counts, occupied-bin visits and separate
classification, P2G, G2P, density-classification and density-solve GPU timings.
Validation independently queries each tile's inverse kernel support, checks
uniqueness, compares density coverage against all nonzero pressure stencils and
rejects stale/nonfinite face data outside the active footprint.

The validation path also reruns original dense P2G and scalar density Jacobi
against the **same** particles, bins, dormant owners and assembled stencil. Every
component must match exactly (zero tolerance). GPU comparison flags cover every
substep and both regular and error-triggered density solves; independent CPU
snapshots additionally audit the final substep. Reference buffers/dispatches exist only during validation and are
excluded from the stage timers. The ordinary runtime does not replay work.

`--fluid-deterministic-bins` now orders recycled-slot assignment as well as bin
indices. Merely sorting by particle ID did not make owner restoration or
resampling deterministic: atomic free lists assigned different physical samples
to those IDs between runs. The reference mode uses serial GPU allocation stages
for reproducibility, never CPU per-particle work. It is deliberately disabled
for gameplay and performance measurements. Paired endpoint tolerances are not
relaxed to accommodate allocator noise.

## Validated checkpoint

RTX 5090, Windows DX12, 1920×1080 Balanced, live room/inlet/foam/orbit,
frame generation off. Three interleaved runs per normal mode, 300 frames each
with the first 32 excluded; values below are means of run medians. The MGPCG
comparison is a single additional pair, not a statistically established result.
No validation replay or ordered allocator ran during profiling.

| Mode | Uniform fluid / raw frame | Compact fluid / raw frame |
| --- | --- | --- |
| Normal pressure | 2.937 / 12.038 ms | 2.834 / 11.922 ms |
| Mixed MAC MGPCG | 6.724 / 16.562 ms | 6.542 / 16.450 ms |

Normal fluid cost falls about **3.5%**, total raw frame time about **1.0%**.
Tail frame times are inconsistent across repeats; this does **not** establish a
stutter improvement. Compact execution stays opt-in, and the default pressure
solver, launchers and release package remain unchanged.

The [recorded runtime evidence](fluid-work-validation.json) includes 19 paired
GPU fixtures (38 runs), a visible-overlay smoke test, and binary/shader hashes.
The fixtures cover empty/transfer/control cases, calm/falling liquid, moving
solids, dormant ownership/restoration, resampling, a 1,200-frame run, room
emission, odd density sweeps, mixed MAC, MGPCG, frame generation and ReSTIR PT.
All paired endpoint metrics and reconstructed volumes matched at report
precision. The recorded sparse fixtures performed 2,720 same-state face and
5,428 density comparisons with zero difference; the long test audits its final
frame, while other fixtures audit each frame. The debug image contains 146,002
cyan work-overlay pixels, not just an enabled option in the report.

The [temporal comparison](fluid-work-temporal.json) separately checks raw and
DLSS-RR output during static, orbit, top-down, live-water and rolling phases,
using the existing preservation gates. Shader compilation, 93 Node tests and
seven native CTest cases passed. Windows GPU-based validation was **not** run:
the debug component remains unavailable (`0x887A002D`).

This is compact **execution**, not sparse MAC allocation. Density assembly,
forces and extrapolation still traverse the dense cache; global pressure stays
coupled. General flowing coarse ownership, adaptive canonical surface fidelity,
optical ray/caustic budgets, asynchronous overlap and timing-budget feedback
remain on the [full-spec checklist](ADAPTIVE_REQUIREMENTS.md).
