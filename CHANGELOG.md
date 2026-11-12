# Changelog

All notable changes to Flint IgniteSDK are documented here.
The format follows Keep a Changelog.

## [Unreleased]

### Added
- FLIM v2 prepared images: A4 thematic masks, variable INDEX8 palettes with RGB565/A8 entries, and RGB565+A8 pixels; desktop conversion, preview and strict resource validation.
- Application icon declarations, theme variant selection, bounded file cache and a 48 px launcher grid with standard-icon fallback.
- Touch navigation, scrolling, interactive factory screens, operation progress and protected boot/status presentations.
- Build-local, atomic and idempotent registry, factory-package and example generation shared by SDK and firmware builds.
- Regression coverage for malformed images, clipping, theme/cache invalidation, large catalogs, touch and generation isolation.

### Changed
- Breaking development contract revision `0x20261112`, FLM2 manifests and FLIM v2 only. Rebuild packages and update consumers together; no legacy decoder is included.
- Render alpha against the RGB565 canvas using bounded scratch storage without decoded-image caches or an additional framebuffer.
- Keep build outputs under `build/` and temporary artifacts under `temp/`.

### Validation
- Clean Windows MinGW 15.1 Debug builds: 20 tests with Lua and 10 without Lua passed. Hardware validation belongs to the consuming firmware; host timings are documented separately in `docs/en/FLIM-BENCHMARK.md`.
