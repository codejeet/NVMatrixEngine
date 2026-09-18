# Ocean environment maps

- `day.hdr`: [Kloppenheim 05 (Pure Sky)](https://polyhaven.com/a/kloppenheim_05_puresky), Poly Haven, 1K Radiance HDR.
- `night.hdr`: [Qwantani Night (Pure Sky)](https://polyhaven.com/a/qwantani_night_puresky), Greg Zaal and Jarod Guest / Poly Haven, 1K Radiance HDR.

Both assets are [CC0](https://polyhaven.com/license). They may be redistributed with this application.
Original downloads: `https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/kloppenheim_05_puresky_1k.hdr`
and `https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/qwantani_night_puresky_1k.hdr`.

The renderer decodes linear RGBE, builds a solid-angle-weighted importance CDF,
and normalizes upper-hemisphere horizontal illuminance to 80,000 lux for day
and 0.002 lux for the moonless night preset. Camera exposure changes by 19 stops
between those presets. These normalizations are artistic reference conditions,
not absolute radiometric measurements of the captures. No tone-mapped JPEG is
used for lighting or reflection. Both assets are sky-only panoramas, so switching time does not replace the island or horizon geometry.
