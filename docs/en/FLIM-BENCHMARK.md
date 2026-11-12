# FLIM v2 measurements — November 12, 2026

[Specification](FLIM.md) · [Detailed Russian report](../ru/FLIM-BENCHMARK.md)

Windows, AMD Ryzen7 5700X, MinGW GCC15.1 Debug; 200 iterations with warm host filesystem cache and unbuffered FILE. Validation reads the FLIM header, palette and every row; draw reads header/palette and renders the complete48×48 image with alpha. FILE opening, catalog traversal, whole-package hash verification and SPI are excluded. These are means from one host run, not latency guarantees or ESP32-S3 timings.

| Sample | Format | Bytes | Validate, us | Draw, us |
| --- | --- | ---: | ---: | ---: |
| monitor | A4 | 1168 | 82.5 | 126.2 |
| monitor | INDEX8 | 2584 | 90.3 | 125.3 |
| monitor | RGB565+A8 | 6928 | 83.4 | 111.8 |
| ring | A4 | 1168 | 82.0 | 109.4 |
| ring | INDEX8 | 2605 | 91.8 | 124.0 |
| ring | RGB565+A8 | 6928 | 83.5 | 109.4 |
| gradient | A4 | 1168 | 81.7 | 126.3 |
| gradient | INDEX8 | 3088 | 91.3 | 143.6 |
| gradient | RGB565+A8 | 6928 | 83.0 | 177.6 |

Monitor is the actual sample icon, ring has antialiased edges, gradient stresses RGBA palette quantization. INDEX8 uses88/95/256 entries. A4 is an explicitly selected alpha mask, not an automatic substitute for the color icon. Clock/Settings color payloads are2872/2881 bytes, masks1168 each (8089 bytes total before package metadata).

With actual ESP-IDF/Xtensa/picolibc flags: decoder36, cache1088, ResourceManager6136 and Scene1360 bytes. Existing4096-byte glyph scratch holds the960-byte row and768-byte palette, so no new pixel buffers are added. Added fields total 1148 bytes before possible alignment padding (roughly1.1 KiB) for the main Core; six512-byte LittleFS caches add3072 bytes before FILE/VFS/filesystem and allocator metadata. Roughly4.1 KiB is a base estimate, not a measured complete heap peak. Cold catalog validation may also use existing4096-byte manifest and16384-byte Lua-section buffers plus manifest containers. Cache capacity is independent of application count.

Verified: SDK20/20, no-Lua10/10 (explicit not_supported for Lua packages), Flare24/24 including Studio/Preview/offline smoke, ESP-IDF6.1 firmware build. Combined firmware751856 bytes leaves about28% of a1 MiB OTA slot; this includes concurrent UI/touch work and is not a FLIM-only delta. Factory files total28463 bytes in a196608-byte image; image listing confirmed9 canonical packages, excluding staging files by generating OS/build/factory separately.

Optional CairoSVG/Cairo was unavailable, so its positive backend test is skipped; external SVG reference rejection is tested. Remote Preview with only the foreground package uses standard catalog icons; OS/local Core uses actual app resources. No device timing, heap high-water mark, visual display or power measurements were performed. Reproduce using `python tools/benchmark_icons.py build/icons-mingw/flim_tests.exe build/icons-mingw/icon-bench`. Consumer pins were not changed; integration used explicit FLINT_PLATFORM_DIR pointing to the working SDK.

Final OS host5/5 and tooling9/9 passed after the directory reorganization. Cache generation also tracks theme mode with unchanged RGB colors, and a failed warm resource requests another full redraw. The open-file mutation fault test is POSIX-only; Windows sharing intentionally prohibits it, so that branch was not executed locally.
