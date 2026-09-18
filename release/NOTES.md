# NVMatrixEngine 0.1.3 — Hamiltonian water and Ocean Island

This release restores the working ocean simulation from before dynamic cell sizing around rigid bodies. The ocean uses fixed 1 m fluid cells and a 2 m wave grid. Hamiltonian waves handle calm regions, while body interactions and complex flow activate existing 3D simulation regions. The unstable local refinement solver and its feedback are removed.

The release includes the larger indoor lab and the 256 × 256 m ocean with 6 m offshore depth, a beach island, pier, day/night HDR environments and night lanterns. It retains the larger powered boat, simulated foam/bubbles/spray, underwater Space propulsion, filling controls and shared camera/player controls. Small Water Lab defaults to legacy DX12 water; Large and Ocean default to Hamiltonian water.

Download **NVMatrixEngine-0.1.3-preview-win64.zip**, extract the whole folder, and open **Play Water Lab.cmd**, **Play Large Water Lab.cmd** or **Play Extra Large Water Lab.cmd**. The portable DX12 build includes runtime dependencies. Optional CUDA and old deep-pool experiments remain in source builds and are excluded from this package.

Target: Windows 11 x64, high-end NVIDIA RTX GPU and compatible driver. Validation uses the RTX 5090 development PC; it does not certify other GPUs or a clean Windows installation. The source and shaders are rebuilt, with native numerical tests, nine ocean/indoor render cases and portable tests after extracting the ZIP to a different directory. The adjacent verification JSON records the checks and checksum for this exact archive.

DLSS frame generation initialization and app-local DLL loading are checked. Additional generated presentations may remain unverified on this machine; consult `frameGenerationOutputVerified` in the verification report. Generated frames do not increase simulation speed. Wakes and hull contact remain limited by the fixed grid; this is a hybrid engine integration, not a reproduction of the paper's complete rendering pipeline.
