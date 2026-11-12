# FLIM v2 and Ignite icons

[Canonical Russian specification](../ru/FLIM.md)

Development revision `0x20261112` accepts FLM2 manifests and FLIM v2 only. Rebuild all packages and consumers; no legacy decoder is provided. PNG/SVG conversion, sizing and quantization run on the host.

All integers are little-endian. The 16-byte header contains magic `FLIM` at0, u8 version2 at4, u8 representation at5, u16 width at6, u16 height at8, u16 paletteCount at10, zero u32 reserved at12. Dimensions are1…320. Rows run top-to-bottom, pixels left-to-right, without compression or alignment.

| Representation | Palette | Row |
| --- | --- | --- |
| A4=1 | none | ceil(width/2) bytes; even x in high nibble; unused low nibble of each odd-width row is zero |
| INDEX8=2 | 1…256 RGB565 u16/A8 u8 triples immediately after header | width u8 indices, each below paletteCount |
| RGB565+A8=3 | none | width consecutive RGB565 u16/A8 u8 triples |

Exact length is `16+3*paletteCount+stride*height`. RGB565 uses R5/G6/B5. Alpha is straight, A4 expands by17. Alpha0 skips the pixel,255 directly writes; intermediate channels use `(src*a+dst*(255-a)+127)/255` in RGB565 code values, reading the actual canvas including byte-swapped storage. A4 uses the drawing color. Invalid types, sizes, indices, padding, lengths and truncations fail safely.

Project `icons` optionally references resource IDs `main`, `mask`, `light`, `dark`. If any variant exists, main is required. Each icon must be48×48; mask must be A4, other variants colored. FLM2 appends these four u32 IDs after components, always including zero for absent entries. Native manifest validation checks references; invalid image contents produce the standard launcher tile. A broken package hash still rejects installation.

Theme `appearance=light|dark` defaults to light; `icons=normal|themed` defaults to normal. Normal selects light/dark if declared, otherwise main. Themed selects mask, otherwise main. Invalid/unavailable selected resources fall back to the standard tile. Mask tint is semantic `accent`; no color inversion occurs. Themes also expose surface/pressed/muted/background/text RGB565 colors.

Launcher uses two124px columns,94px cells at100px pitch,48px icons above fitted UTF-8 labels, and six recycled cells. Offscreen cells are hidden. Existing touch/scroll behavior remains; rotate/press/release supports focus and activation. Scene kind7 is `CatalogIcon` (`catalog_icon`), extra=catalog index+1;0 means the standard tile. It requires unscoped packages.list and CapInteractiveUi. `ignite.set(ui.icon,'catalog',index)` accepts0…65534. `ignite.catalog(index)` returns name,target,error. `Scene.resourceRevision` is derived RAM-only invalidation metadata.

```sh
python tools/flim.py icon.png icon.flim --representation INDEX8 --size 48 48 --colors 256 --preview quantized.png
python tools/flim.py mask.png mask.flim --representation A4 --mask alpha --size 48 48
python tools/compile_ignite.py examples/icons/project.json icons.ignit
```

Resource entries accept the same representation/size/colors/mask/preview fields. Pillow handles rasters and Lanczos sizing with alpha. Optional host CairoSVG plus system Cairo supports self-contained SVG shapes with explicit output size, excluding scripts, embedded images, external references, stylesheets, DTD, imports/base URLs. No embedded dependency is added. CairoSVG is LGPL-3.0, Cairo LGPL-2.1 or MPL-1.1; install separately or export PNG.

Default is RGB565+A8, with no automatic palette loss. Explicit INDEX8 preserves exact RGB565/A8 pairs when possible, otherwise uses Pillow RGBA FASTOCTREE without dithering, up to1…256 colors. Different alpha values consume palette entries. The converter reports dimensions, format, bytes and composite RMSE over black/white backgrounds; inspect the actual quantized PNG preview and choose RGB565+A8 if needed. A4 requires explicit alpha or luminance-times-alpha masking.

Row scratch960 bytes and palette768 reuse the existing4096-byte glyph scratch. Only decoder metadata is additional; no RGBA image or new framebuffer is allocated. Six File/Package/View slots cache no pixels; stdio buffering is disabled for icon files. LittleFS/VFS/FILE allocations remain: configured512-byte LittleFS caches alone can add3072 bytes for six open files, excluding metadata. Core closes cached files before mutation; direct PackageManager users must do so too. Catalog revision, runtime generation and theme mode invalidate the cache.

Cold cache misses use the existing catalog scan/verification and stream the package hash, then validate the whole icon once. Warm rendering reads header, palette and clipped pixel rows only. Full48px draw needs49 resource reads for A4/RGB565+A8,50 for INDEX8. Resource sizes including header:1168 bytes A4,2320+3N INDEX8 (maximum3088),6928 RGB565+A8.

`flim_decoder` and `app_icons` cover alpha, odd widths, palettes, malformed inputs, clipping/swapped canvas, variants, fallback, installation/update/removal and package integrity. Existing UI tests cover navigation and long names. Run `python tools/benchmark_icons.py build/flim_tests.exe build/icon-bench` for200-iteration native host file-read validation/draw timings on monitor/ring/gradient samples. These warm-host-cache measurements exclude catalog/package verification and display SPI, and do not establish ESP32-S3 performance or peak heap. Object sizeof values are not whole-system memory measurements.

[Host measurements and RAM estimate](FLIM-BENCHMARK.md)

The launcher_icons test covers120 apps and maximal IDs. Lua can reposition image/catalog nodes but cannot arbitrarily resize them. Renderers without Core, including remote Preview with only the foreground package, use the shared standard icon for CatalogIcon rather than interpreting its catalog index as a package resource ID.

Derived resourceRevision includes catalog, theme mode and read-failure cache generations. Core synchronizes it before dirty comparison; a warm-resource failure during a clipped repaint requests a subsequent complete frame.

ResourceManager/Runtime close the decoder's borrowed Source before local views/read contexts expire. Scratch remains shared; validate without a subsequent open returns invalid_state.
