# Live engine recordings

The README embeds GitHub-hosted H.264 videos recorded from the running DX12 application. MP4 copies are retained in this source directory for reproducibility and direct download; the portable ZIP links to the hosted videos instead of duplicating them.

- **Water room:** solid sinking ball driven with ordinary movement/jump inputs (`--demo-tour`), a gently following camera, 200k initial particles, 0.55 m initial depth, and a running wall inlet. Waves are produced by the existing moving-solid/fluid interaction, not an animated water plane or injected visual splash.
- **Wall inlet:** normal room solver with the inspection camera, 100k initial particles and the live emitter.
- **Deep pool:** the larger/deeper pool using the DX12 baseline.
- **Underwater:** first-person lens below a 1.4 m room pool, 400k initial particles, solid sinking ball and a slow look-around. This uses the same dielectric/medium renderer, not a color filter over an above-water shot.

Captures use 1280×720 output, DLSS RR Quality, no frame generation, 30 fps Desktop Duplication capture and hardware encoding. Each clip is 14 seconds, silent, with unchanged wall-clock playback timing. The bounded engine runs use a fixed 1/60 simulation step; these are visual demonstrations, **not frame-pacing/performance benchmarks**. The capture encoder also uses GPU time.

`engine/record-portfolio.ps1` reproduces the capture setup with a Windows FFmpeg build supporting `ddagrab` and `h264_nvenc`. The demo is temporarily topmost and only its client rectangle is recorded. Inspect all footage for occlusion/notifications before publishing. Web copies are compressed to H.264 / yuv420p with fast-start MP4 metadata. There is no AI generation, motion interpolation, artificial speed-up or retouching of simulation behavior.

Normal interactive launches remain user-controlled; `--demo-tour` is an explicit bounded recording mode. The default player density is Sink; the Float toggle remains in Esc settings.
