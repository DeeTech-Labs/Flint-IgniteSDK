[Русский оригинал](../ru/ARCHITECTURE.md)

# Flint IgniteSDK

The shared C++20 platform used by FlinT OS, Flare Preview and the local simulator. Public OS API, Face SDK, Ignite format and Companion protocol versions remain **1**. Development contract revision and build identity are checked separately; incompatible binaries must never be accepted merely because both report v1.

This repository is the single source of the portable runtime, package contracts,
compiler, SDK tools and tests. [FlinT OS](https://github.com/DeeTech-Labs/FlinT-OS)
and [Flint Flare](https://github.com/DeeTech-Labs/Flint-Flare) consume pinned commits
as Git submodules at `platform/`. See [SDK.md](SDK.md) for standalone installation,
Lua programming and consumer updates. No generated source copies are maintained.

## Architecture

The ESP-IDF layer owns hardware, native USB, authenticated NimBLE, mounting and recovery. Portable Core owns package transactions, consent, time, component selection, event scheduling and application lifecycle. Runtime reads compiled UI and individual resources through indexed file access. Factory packages and user packages use the same manifest, validator and Runtime. Core selects factory components by role metadata; it contains no built-in watchface layouts or application-name switch statements.

Flare preserves its QML shell, visual theme, icons, fonts, effects and localization. Its service layer consists of PlatformClient (device/transport lifecycle), IgniteHub (packages and consent), Studio (project compilation), Preview (shared renderer) and CompanionService (host policy and metrics). Its simulator runs the exact same portable Core through binary framed requests, in a disposable directory, without opening a device.

API and permission registries are generated from `spec/services.json` and `spec/permissions.json`. Services that are not implemented return `not_supported`. Listing a future permission does not advertise a working hardware or host integration.

## Ignite container

Integers are little endian, except compiled UI scenes, whose codec explicitly uses big endian. Never cast wire bytes to native C++ structs.

The 64-byte container header contains: magic `FLIGNIT\0` at 0; u16 format version at 8; u16 header size at 10; u32 development revision at 12; u32 total length at 16; u32 entry count at 20; u32 index offset at 24; u32 reserved zero at 28; SHA-256 at 32. The hash covers the entire file with the digest field zeroed.

Index entries are 24 bytes: u32 ID, u16 section type, u16 codec, u32 offset, u32 length, u32 flags, u32 reserved. IDs increase strictly, sections do not overlap and the final section ends at EOF. Exactly one section is a manifest. Supported sections are manifest=1, component=2, resource=3, UI=4, Lua=5, locale=6, signature=7. Codec, flags and reserved must be zero. Package size is bounded to 1 MiB, section count to 1024 and manifest size to 4096 bytes. These are parser/security limits, not fixed installation slots.

The manifest starts with `FLM1`, followed by u16-length UTF-8 strings for ID/name/publisher; u32 package version; three u32 memory/instruction budgets; u64 required capabilities; u16 permission count with permission/scope string pairs; u16 component count with ID string, u16 type, u8 role and u32 section ID. Permission count is bounded to 64 and components to 32. IDs are restricted identifiers and can never contain path traversal. Unknown permissions are rejected.

Compiled UI supports rectangles, rounded rectangles, lines, text, small masks and indexed RGB565 images. Text bindings expose only declared APIs. Own resource access requires `ui.resources`. Fonts use indexed FLGF glyph atlases with bounded scratch storage; images use FLIM plus dimensions and RGB565 rows. The compiler performs TTF/PNG conversion on the computer. Nothing on the watch parses TTF, SVG, YAML or source JSON.

The scene description and built-in rasterizer/font algorithms were retained as portable graphics primitives. The old FaceRegistry, fixed layouts, IgnitDefinition, three-slot manager, textual installer and command parser were removed.

The resource cache holds 32 views with round-robin eviction: a full cache does not limit accessible sections. FLIM reads cover only the intersection of the image, screen and `Canvas::clip`. FLGF metrics and bitmaps are read separately; the metrics cache holds 16 glyphs and resets on package rebinding. Runtime stores derived resource-text bounds in the in-memory `Op::ink*` fields. These fields are not serialized; decoded scenes without derived bounds retain conservative full-screen invalidation.

Runtime parses `${...}` at launch: an unclosed substitution returns `invalid_package`, and more than 32 substitutions in an operation returns `execution_limit`. The fixed substitution-end table uses 1024 bytes; text assembly uses bounded buffers without copying `Values`. Lua updates keep the same frequency, but unchanged properties and published values do not request repaint. Core reuses bindings between changes to time, settings, catalog and external data. Catalog traversal caches up to 32 factory IDs per visit; larger factory catalogs fall back to scanning to preserve factory identity protection.

`PackageManager::revision()` invalidates catalog caches after `begin()` and successful commit/remove/rollback. The launcher summary retains its existing string limit; full manifests are not retained. Metrics identity is cached per Runtime generation and catalog revision and checked against the loaded package; declarations, capabilities, application state and live grants are checked on every access. Files changed outside PackageManager require reinitialization through `begin()` and restarting the affected application. SHA-256 still does not authenticate publishers.

Wire formats, nominal versions and ContractRevision are unchanged. C++ object sizes (including `Op`, `Runtime`, `Core`) changed, and `Values` now uses a transparent comparator (`std::less<>`); rebuild SDK and consumers of its headers together. Custom renderer integrations should set `resourceMetrics` alongside `resourceGlyph` to avoid reading bitmaps during measurement. The added caches are bounded; memory capacity and latency on a specific board still require separate validation.

## Storage and recovery

Factory packages are read-only filesystem contents built with the firmware. User packages are immutable objects named by their container digest. Each user package has an atomic `.ref` and a previous-version reference. Installation uses `incoming.part`, durable block writes, a complete-file transfer digest, container/manifest/UI validation, then an atomic reference commit. Repeating a block is allowed only when its existing bytes match. Reopening the same transfer resumes at its durable offset.

An interrupted install cannot become active. Startup restores a valid previous reference when the current object is invalid. Failed active updates reload the previous version. User operations cannot replace or remove factory identities. Unreferenced objects and interrupted atomic-write temporaries are collected; the current transfer is retained for resume. The production catalog visits one manifest at a time instead of retaining every inactive manifest in RAM.

Recovery remains outside user packages. Flare can restore factory role selections. The firmware can show its own recovery screen and process USB independently of the active Ignite. NVS trust keys and non-erased corrupt filesystems are never automatically wiped.

## Security

A/P/S/U/X/R grants bind application, claimed publisher, installed content generation, exact scope, expiry, session or explicit action. Automatic access still requires a manifest declaration. Restricted rights are unavailable to ordinary user packages. Session consent ends on disconnect; upgrades do not inherit expanded or earlier-version consent. Revocation invalidates samples and queued events.

CPU/RAM are available only with an active matching `companion.pc.metrics` grant scoped to `cpu,ram`, and the independent Flare policy enabled. Samples received without watch consent are rejected. Samples expire after ten seconds, and switching apps clears them. The host collector currently supports Linux; unsupported collectors do not fabricate measurements.

SHA-256 verifies integrity, not publisher authenticity. User publisher strings are never treated as signatures or privileged identity. Factory trust comes from the protected firmware-provisioned volume. No user package has access to QProcess, arbitrary shell commands, raw GPIO, another app's memory or transport internals.

BLE commands require encryption, authentication and bonding. Numeric comparison is authorized through the retained local USB channel; BLE cannot open or approve its own pairing window. USB and BLE use the same framed service contract, without a textual fallback.

## Companion framing

A frame is `FLCP`, u8 version=1, u8 response flag, u16 method ID, u32 nonzero request ID, u32 payload length, payload, then CRC-32 over header and payload. Maximum payload is 4096 bytes. Readers accept fragmented frames, reject invalid checksums and discard leading noise. Responses start with a u16 Error code. Payloads use the shared explicit Writer/Reader codecs; strings carry u16 byte lengths. Service IDs are fixed by `spec/services.json` and checked against the C++ enum.

The handshake carries protocol, development revision, build ID and required capabilities. No state-changing operation is accepted before compatibility and transport trust checks. Transfer chunks are independent of ATT fragmentation. BLE uses acknowledged 20-byte GATT fragments and generation checks to discard disconnected work.

## Build and local validation

```sh
python3 -m pip install Pillow
python3 tools/generate_registry.py
python3 tools/build_system.py
cmake -S . -B /tmp/flint-platform -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/flint-platform
ctest --test-dir /tmp/flint-platform --output-on-failure
```

Build an application without changing or flashing OS source:

```sh
python3 tools/compile_ignite.py \
    examples/pc-monitor/project.json /tmp/pc-monitor.ignit
```

Integration tests exercise resume, duplicate chunks, integrity failures, updates, rollback, factory protection, permission checks and revocation, custom fonts/images, stale handles, repeated switching and framing. Flare's offline smoke test loads the actual QML interface and simulator. Physical device tests are excluded.

Use four spaces, no tabs, in maintained source. `.editorconfig` and `.clang-format` capture the project style. Generated font data is not manually reformatted. `tools/build_builtin_fonts.py --font-dir /path/to/lato` regenerates the common built-in font subsets.

## Explicit remaining limits

Lua 5.4.9 is the only application scripting engine. Its bounded prototype runs on
host builds, the shared simulator and the validated Spark firmware configuration. Executable background services are not
supported. Registry entries for future permissions do not imply implemented APIs.

The development unit was identified as 4 MiB Flash and 2 MiB PSRAM. Firmware owns the partition layout with two application slots, a factory volume and writable storage. PSRAM is optional; the renderer prefers it when available and stages DMA rows in internal memory. Real memory headroom, wiring, BLE behavior, flash/power-loss behavior and battery life still require device validation. No battery-life or on-device reliability claim follows from host tests.

Design sources: [full discussion](https://chatgpt.com/share/6aea633e-6744-83eb-8ef1-564b610456bb), [final summary](https://chatgpt.com/share/6aeb20f7-8fbc-83ed-8fde-b505b0d2baf9). The current Lua-only implementation supersedes earlier execution proposals.

## Standalone Ignite SDK

See [SDK.md](SDK.md) for the independent `ignite init/build/check` workflow,
installation, exported `Flint::Platform` CMake dependency, and pinned Git submodule integration. The validator uses this runtime directly; its report
does not claim measured device fit or publisher authenticity.


## Licensing and language policy

See [licensing scope](LICENSING.md). Russian is canonical; this is the English translation. Earlier license grants remain intact.

## Application API 1

This additive Lua extension requires capability 128; binary formats, protocol identifiers and ContractRevision are unchanged. Runtime owns the input queue (16), timers (8), bounded string values and diagnostics. Core implements authorized read-only services and private package storage (16 keys, 4096 bytes, atomic replacement, CRC-32, one successful write per second per Core). Callbacks in a tick share the instruction budget; service calls are capped at 32. Adapters use postInput and nextWake; hardware integration remains in consumers. C++ class sizes changed and require rebuilding consumers. Additional fixed structures and storage temporaries do not expand the Lua heap: the existing 16 KiB deduction is unchanged, but total native peaks/stack need new device measurements. See [SDK.md](SDK.md#application-api-1-input-state-and-services) for the full contract and limits. The simulator executes the same Core/Runtime with virtual-time scenarios.


## Companion Demand and diagnostics

[Companion Demand v1](COMPANION.md) separates Device Control and session-bound data
providers within one Core. Disconnecting another client does not clear metrics or
local runtime events. Consumer generations also change on suspend/resume while
input handle generations retain their existing contract.

Optional `Platform::microseconds` enables cumulative count/total/max timings for
requests, logic ticks, scenes, paint and resource reads. Resource reads/errors and
EventBus overflows are counted without extra tasks or queues. Phases may be nested.
Diagnostics do not dirty the scene. Replies fit MaxPayload and 128 fields, prioritize
Lua fault details, and mark omitted optional fields with `diagnostics_truncated=1`.
Python clients accept numeric and textual values. C++ class sizes changed: rebuild
SDK and consumers together. Wire/package ContractRevision remains unchanged.


## Resumable installation verification

Adapters use `Core::process`, `tick`, `replyReady` and `takeReply` on one owner.
`install_commit` may defer its existing reply (same method, request ID and payload)
until validation and publication complete. `Core::handle` remains the synchronous
native interface. Wire, package and `incoming.part.meta` formats stay unchanged.

Core holds one pending installation and one result slot. Index/hash verification
reads at most 4096 bytes per tick. Lua/UI sections are read in chunks; each step
decodes at most one bounded scene. Header, manifest and reference validation have
separate fixed bounds. Runtime, input and other clients continue between steps;
catalog mutations and further upload chunks return `invalid_state`. Application
switching remains allowed; restart requirements are decided at publication.

Owner disconnect cancels unfinished verification and closes files while retaining
staging/metadata for resume. Once publication commits, disconnect only discards an
undelivered reply; reconnecting clients must inspect the catalog. Flare cancels by
disconnecting, waits up to 45 seconds for commit and reports completion only after
success.

`PreparedInstall` binds proof to an open immutable file and PackageManager revision.
Publication checks the revision, original reference, open-file version and staging
object identity. Windows read handles deny writes and permit transfer into
content-addressed storage; POSIX also compares inode/device. Storage has one owner;
external changes must invalidate proof. LittleFS lacks complete host file IDs, so
Core/hardware adapters prohibit concurrent staging writes.

Runtime adopts the same verified open file without a second full SHA-256 pass.
Permissions are never cached. Factory/current packages use resumable validators.
Firmware sets `Platform::require_factory=true`; standalone simulation may omit a
factory directory. An available but corrupt factory catalog is never ignored.

Atomic filesystem metadata operations, durable writes, metadata garbage collection
and rare rollback/recovery paths remain synchronous. A byte quantum is not a strict
wall-clock guarantee on slow or failed media. Board stress validation is required.

## Memory budgets and launch

`Platform::memory` defines stable package admissibility. Do not populate it with current free heap: temporary memory pressure must not hide an installed package or trigger rollback. Optional `available_memory` is checked by Runtime at launch only, separately for internal RAM and PSRAM. Failure returns `resource_exhausted`, preserving the catalog and the previous app until ownership transfer. A verified installed update is not rolled back solely because activation runs out of memory; Core attempts the factory clock and records the launch error.

The Lua heap still cannot exceed `declared internal - 16 KiB`. The declaration additionally reserves estimated native manifest/component allocations, `LuaApp::Impl` (including published values), and Lua source at the loading peak. The Lua heap is reduced when necessary; its 16 KiB minimum remains. An older package whose actual native peak exceeds its declaration may now fail launch; package format and ContractRevision remain unchanged. Impl allocation failure is explicit, and failed launch releases the file and resources even before lifecycle.create.

Diagnostics distinguish `runtime_resident_bytes` (fixed Runtime, including ResourceManager and its buffers, already included in the platform baseline), native heap/peak estimates, and Lua used/peak. STL capacity estimates exclude allocator headers, fragmentation, stack, validation/scene bindings and hardware queues. The adapter must measure the full peak and largest free block; estimates do not guarantee total RAM.

## Time checkpoints and repeated validation

`scene()` never writes `epoch`. The Core owner saves it in `tick()` every 60 seconds; `nextWake()` accounts for the checkpoint independently of frame rate. Successful `time_set` persists immediately and schedules the next checkpoint 60 seconds later. Storage failure preserves the running clock in RAM; `epoch_write_errors` and `epoch_checkpoint_disabled` expose the problem. Automatic retries stop after three failures (60 then 120 second delays) until a successful `time_set` or reboot. `epoch_durable_writes` counts successful writes by the current Core.

During successful operation, persisted time trails by less than one checkpoint interval. Powered-off duration is unknown without RTC or synchronization; storage errors invalidate that interval guarantee. Decimal epoch and other persistent formats are unchanged; no migration is required.

Source inspection of revision 3275a64 shows three complete candidate reads during commit: transfer SHA, Core inspect and PackageManager inspect. Both digests now share one body pass; index validation adds its own reads. The large-package test checks the exact byte count. Prepared installation passes the same open file to Runtime. Ordinary `Core::launch` and `package_read` use call-scoped `locate(..., File&, Package&)` to retain the validated open file until handoff/read without a second SHA. Runtime checks the file for changes before handoff. Catalog and permissions are not cached; a later call validates again. Factory lookup and role-activation fallback may still validate other packages repeatedly; there is no global cache.

Adapters for filesystems that prohibit renaming an open file set `Platform::rename_open_files=false`. PackageManager validates staging identity and revision, closes both read handles, renames, then reopens the moved object and checks size/content mtime before publishing the ref. This requires exclusive filesystem ownership and runs entirely on the Core owner; external writers are forbidden. Rename may change ctime, so it is not compared afterward. Package still references the same File; no SHA pass is repeated. Rename/reopen failure leaves the current ref unpublished and recovery retains the old package. ESP LittleFS enables this mode; Windows/POSIX retain the open handle by default. Host async-install tests exercise both path families through separate Core/PackageManager fixtures.
