# Changelog

All notable changes to Flint IgniteSDK are documented here.
The format follows Keep a Changelog.

## [Unreleased]

### Fixed
- Keep long thermometer values and system presentation text in explicit regions. Add optional `overflow` text boxes (`clip`, `wrap`, `ellipsis`); free positioning remains unchanged. Breaking development contract `0x20261115` requires rebuilding packages, factory, OS and Flare together without erasing user data. See [text layout](docs/en/UI-LAYOUT.md).
- Convert fractional thermometer readings to text before publishing UI values; the executable documentation scenario now exercises UI delivery and permission revocation.
- Label shared thermometer values as last-known readings, so stopping its collector cannot leave a false freshness claim in the UI.
- Release service resources on pause/resume callback faults, persist the crash autostart latch, preserve unrelated owners on package removal, and retain sensor conversion deadlines without busy polling deferred service events.
- Preserve incompatible installed package objects, references and private state across firmware contract changes; explicit launch reports incompatible_contract instead of treating incompatibility as storage corruption.
- Keep revocation denied in RAM if durable persistence fails, suppress pending readings after revoke, and distinguish saved output policy from failed hardware acknowledgement.

### Added
- Documentation checks for local/cross-repository links, anchors, config examples, generated registry drift and compiled/replayed examples.
- Isolated per-component background Lua services, explicit execute/autostart consent, model-bounded memory and one fixed shared service CPU pool, independent of foreground budgets.
- Typed same-package IPC, atomic revision-checked RAM state, asynchronous globally throttled persistence and bounded six-reading thermometer history.
- Bounded typed sensor contracts, a Core-owned Sensor Broker, asynchronous refresh/subscriptions, coherent snapshots and deterministic simulation/scenario controls.
- Lua ignite.sensors catalog/read/refresh/subscribe/unsubscribe with real callbacks, shared tick budgets and lifecycle cancellation; narrow beep and temporary brightness APIs.
- Explicit, expiring Companion sensor diagnostics and asynchronous RTC time-status acknowledgement; automatic-brightness policy through system demand.
- FLIM v2 prepared images: A4 thematic masks, variable INDEX8 palettes with RGB565/A8 entries, and RGB565+A8 pixels; desktop conversion, preview and strict resource validation.
- Application icon declarations, theme variant selection, bounded file cache and a 48 px launcher grid with standard-icon fallback.
- Touch navigation, scrolling, interactive factory screens, operation progress and protected boot/status presentations.
- Build-local, atomic and idempotent registry, factory-package and example generation shared by SDK and firmware builds.
- Regression coverage for malformed images, clipping, theme/cache invalidation, large catalogs, touch and generation isolation.

### Changed
- Add canonical RU/EN audience routes, first-app/service walkthroughs, implemented Lua/registry references, contributor checks and source-verified user/recovery guides.
- Breaking development contract `0x20261115` and FLM3 manifests: move logic, budgets and permissions into components and rebuild all packages/consumers. Package envelope and nominal API/Companion versions remain 1; incompatible installed objects/references are retained. See [background-service migration](docs/en/BACKGROUND-SERVICES.md).
- Ambient-light access requires persistent consent; temperature, pressure and humidity use persistent permissions. Foreground and services share one Sensor Broker with owner-bound handles. FLIM v2 is unchanged.
- Render alpha against the RGB565 canvas using bounded scratch storage without decoded-image caches or an additional framebuffer.
- Keep build outputs under `build/` and temporary artifacts under `temp/`.
