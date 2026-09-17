# Next work

The priority is lower **raw frame latency**, not higher generated-frame counters. Keep the present runnable baseline and require matched-scene evidence before promoting experimental modes.

## Path tracing and caustics

1. Profile fluid intersection, secondary visibility, camera transport, photon tracing and reservoir passes separately. Track median/p95/p99 raw latency, not only average FPS.
2. Reduce field fetches and procedural traversal work without changing canonical roots/normals; improve coherent ray scheduling and measure SER on the actual divergent workload.
3. Concentrate photon effort on active refractive surfaces and caustic receivers, with explicit PDF/energy accounting and bounded footprint cost.
4. Improve motion/confidence for refracted receiver history. Require top-down orbit, rolling, underwater, inlet and moving-light tests to pass without shimmer, ghost trails or overly dark water shadows.
5. Extend compatible path reuse to more materials and refractive prefixes. Evaluate ReSTIR BDPT / guided specular transport as a separate research milestone, not a rename of the existing photon renderer.

Acceptance: lower raw GPU time at matched output resolution, particle state and error; no regression in caustic energy, temporal stability, disocclusions or thin surface intersections.

## Water simulation

1. Make narrow-band particle retirement save measured P2G/G2P/binning cost; eliminate remaining capacity-sized scans where safe.
2. Reduce dense fine-grid and reconstruction work using compact GPU schedules. Preserve global pressure coupling and a single authoritative volume/momentum inventory.
3. Amortize refinement, restoration and allocation so topology changes cannot create unbounded frame-time spikes.
4. Improve mixed-resolution pressure convergence per millisecond and conservative fine/coarse interfaces. A larger solver is not automatically a faster solver.
5. Reduce CUDA/DX12 handoff and staging cost; overlap simulation/rendering only after double-buffered ownership and latency are well defined.
6. Improve sheet/tendril reconstruction, robust nested-water media, secondary whitewater and rigid-body coupling independently of solver LOD.

Acceptance: matched-room and deep-pool comparisons with sustained inlets/moving bodies; bounded p99 latency; no mass/momentum drift, unsafe restoration, pressure discontinuities, surface seams or resolution popping. GPU residency and lower active counts are necessary architecture properties, not sufficient proof of speedup.

## Productization

- Broader RTX hardware/driver validation and repeatable quality presets.
- D3D12 debug-layer / GPU-validation runs on a provisioned development machine.
- Automated release smoke/capture checks, explicit supported-hardware matrix and signed application binaries.
- Source-license decision before presenting this project as reusable open-source middleware.

Full two-phase air, ST-FLIP spacetime stepping, unrestricted multiple-scattering liquids and a production sparse multiscale solver remain future research. They are not prerequisites for optimizing the existing demo.
