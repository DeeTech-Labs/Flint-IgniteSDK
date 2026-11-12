[Русский оригинал](../ru/SDK.md)

# Flint IgniteSDK

The container builder rejects packages larger than 1 MiB (including the header and index) and manifests larger than 4096 bytes. Before writing headers, the service registry generator validates unique names and IDs, the ID range 1..65535, and the boolean type of `mutates`.

Standalone SDK and shared portable runtime for FlinT OS and Flint Flare.
Repository: https://github.com/DeeTech-Labs/Flint-IgniteSDK

## Build and install

Requires CMake, a C/C++20 toolchain and Python 3. Pillow is needed for image/font
conversion and integration-test fixtures. Lua 5.4.9 is vendored with its upstream
license and verified archive digest; builds do not download an interpreter.

```sh
cmake -S . -B /tmp/ignite-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/ignite-build --parallel
ctest --test-dir /tmp/ignite-build --output-on-failure
cmake --install /tmp/ignite-build --prefix /tmp/ignite-sdk
/tmp/ignite-sdk/bin/ignite init /tmp/my-clock --id demo.clock --publisher demo
/tmp/ignite-sdk/bin/ignite build /tmp/my-clock/project.json /tmp/my-clock.ignit
/tmp/ignite-sdk/bin/ignite check /tmp/my-clock.ignit
```

On Windows invoke the installed `ignite` script using Python. In a source checkout
pass `--validator /tmp/ignite-build/ignite-check` to `tools/ignite.py build/check`.
CLI reports and diagnostics use UTF-8 independently of the console code page.
Failed builds preserve the last successful package. The JSON report gives declared
budgets and runtime compatibility, not measured hardware fit, publisher authenticity,
market admission, or battery-life guarantees.

## Lua application

`logic: {"language": "lua", "path": "main.lua"}` adds one text-source section.
A Lua package has exactly one app/watchface component, capability bit 64, 48..144 KiB
of declared internal memory and 1..100000 Lua instructions per callback. Source is
limited to 16 KiB. Binary chunks are rejected. The current development contract is
0x20261112; regenerate packages using this SDK. There is no previous-format loader.

The source returns a table with `update(dt_ms, pressed)`. It is called once at load
with zero elapsed time, then explicitly by Runtime or the Core scheduler. Elapsed
time is capped at 100 ms by Core. Input injection requires `input.focused`.
The first API intentionally exposes only:

- `ignite.set(node, property, integer)`: update x/y/w/h on supported own UI nodes.
  Nodes are one-based; the full-screen background is immutable. Bounds match the
  scene codec. Resource references and executable callbacks cannot be replaced.
- `ignite.value(key, integer)`: publish up to 16 values, used as `${app.key}` in text.
  Keys are 1..32 lowercase ASCII letters/digits/underscores; values are signed i32.

The VM has a counting allocator and instruction hook. No standard libraries,
filesystem, OS, debug, module loading, coroutines, dynamic loading or protected-call
helpers are exposed. Failures destroy the state; Core returns to its clock/recovery
path. This is an experimental sandbox, not a complete security audit. Instruction
count is not wall-clock time. Native allocations, source parsing, C stack and GC
latency still require measurement on the device.

```sh
/tmp/ignite-sdk/bin/ignite build examples/flappy-bird/project.json /tmp/game.ignit
/tmp/ignite-sdk/bin/ignite run /tmp/game.ignit --frames 120 --press-every 24 --image /tmp/game.ppm
```

`run` is a deterministic headless simulation with synthetic presses and a final
PPM image; it is not an interactive game window. Flare's simulator uses the same
Core for timed updates. Physical game input is not wired in this stage.
Host builds enable Lua by default; `-DFLINT_ENABLE_LUA=OFF` tests the rejecting
configuration. Spark firmware enables the same source/limits via
`CONFIG_FLINT_LUA_RUNTIME`. The shared configuration limits nested C calls to 40;
firmware reserves a 32 KiB main-task stack. C++ bridge headers include `<climits>`
to preserve the C/C++ integer ABI on ESP-IDF's toolchain.

The Spark profile has physical validation for scene animation, repeated app
switches, execution/allocation limits and factory recovery. Keep raw device
reports outside the repository; record concise validation results in pull requests.
Revalidate on-device when changing the engine, limits or native bindings.

Authenticated Companion method 21 (`diagnostics`) returns integer-valued strings:
VM used/peak bytes, last logic error, and device-provided memory/stack/timing fields.
Timing maxima are cumulative since boot and include transitions/recovery; they do
not represent steady-state FPS or pure interpreter time. `tools/companion.py` is an
explicit USB development client (requires pyserial), not an app-accessible API.


## One source, pinned consumers

OS and Flare mount this repository at `platform/` using Git submodules. Each parent
records an immutable SDK commit. There is no copy/export/sync tool.

```sh
git clone --recurse-submodules <consumer-repository>
git submodule update --init --recursive
```

Implement shared changes in this repository, test and push them, then advance the
submodule pointer in each consumer. Review and commit that pointer with any adapter
changes. Do not follow a moving branch automatically. Matching source commits avoid
implementation drift; development contract/build/capability checks still apply.

Host consumers may use `add_subdirectory(platform)` and `Flint::Platform`, or install
and use `find_package(FlintPlatform 1.0 EXACT CONFIG REQUIRED)`. ESP-IDF compiles the
same source release through its component adapter, not a host static library.
Board configuration, transport and display adapters remain in OS; Qt UI stays in
Flare. Factory packages/examples currently belong to this shared contract release.

## Remaining work

Recheck resource, latency and failure recovery measurements for other boards or
new native bindings. Instruction quotas do not establish a wall-clock deadline for
C code, parsing, garbage collection or filesystem recovery. Private state, input
delivery and bounded services are implemented in Application API 1 below. Physical
input/power adapters, styles and Studio editing require separate integration.
Marketplace signatures and publication remain separate work.

## Application API 1: input, state and services

`"app_api": 1` adds mandatory capability `128` (`CapAppApi`) to a Lua project's requirements. Lua still requires bit `64`; `capabilities: 65` plus `app_api: 1` therefore compiles to `193`. Older runtimes reject the missing capability. Current revision `0x20261112` requires FLM2/FLIM v2 and rebuilding all packages and consumers. No legacy decoder is provided; see [FLIM](FLIM.md). C++ ABI compatibility is not preserved.

New functions require `CapAppApi`. Invalid argument counts/types, ranges and UTF-8 terminate the handler with a diagnostic. Service/access failures return numeric `flint::Error` codes: 0=ok, 4=not_supported, 5=permission_denied, 7=source_unavailable, 9=quota_exceeded; see `include/flint/contracts.h` for the complete enumeration. No standard libraries or unrestricted `require`, `debug`, `io`, `os` are exposed.

### Events and lifecycle

The returned application table must still contain `update(dt_ms, pressed)`. An optional `event(kind, value)` handles `press`, `release`, `hold`, `rotate`, `focus_lost`, `focus_gained`, `timer`, `suspend`, `resume`. Values are signed i32; timer values are IDs 1..8, suspend/resume use 0. Callbacks run synchronously in the Core loop. There is no destroy callback: persist important state in advance. Events do not execute in the background.

The OS adapter calls `Core::postInput({kind, value, runtime.generation()})`. An active Lua app and an empty-scope `input.focused` declaration are required. The queue holds 16 events; overflow returns quota_exceeded and mismatched generations return stale_handle. Input after focus_lost is discarded until focus_gained. Old-run events never cross a restart; suspend/stop clear the queue. Legacy packages receive presses through `update(0, true)`. OS owns physical input recognition/debounce; Flare owns QML presentation.

### Scheduling and timers

- `ignite.schedule(interval_ms)` returns an error code. Values 1..86400000 set the update interval; 0 disables periodic updates. The initial `update(0, false)` still runs. Default interval is 33 ms; scheduler dt is capped at 100 ms.
- `ignite.timer(id, delay_ms, repeating)` returns an error code. Eight slots, IDs 1..8, delay 1..86400000 ms, boolean third argument. Reusing an ID replaces it; delay=0 cancels it. Delivery is `event('timer', id)`.
- Both require empty-scope `time.timer.create`. Timers belong to the current run, are not delivered during suspension, are cancelled on resume, and are destroyed on stop/restart. The resume event can rearm them. Missed repeating periods coalesce to one delivery per tick, with no catch-up loop.

`Core::nextWake()` returns an absolute monotonic millisecond deadline accounting for input, timers, updates, minute changes and metric expiry; UINT64_MAX means no deadline. Adapters must call tick by that deadline and wake on external events. All callbacks in one Runtime tick share the declared instruction allowance, in addition to queue/timer bounds. At most 32 service calls are allowed per handler/Runtime tick; excess calls return quota_exceeded. This is not a wall-clock guarantee: native calls, filesystem operations, parsing and GC require hardware measurements.

### Services and private state

`ignite.read(service [, key])` returns `value, error`, or nil/error. Implemented names only:

- `time.read`: Unix seconds, source_unavailable before clock synchronization.
- `time.monotonic`: monotonic milliseconds.
- `system.info`: key profile (also the default) or build_id.
- `system.power.status`: passes the key to the read-only `Platform::power` adapter; absent adapter means not_supported. Device profiles define supported fields; SDK supplies no fake measurements.
- `system.locale.read`, `system.theme.read`, `system.settings.read`: string by key from active locale/theme/system settings; missing key means source_unavailable.

Each requires its same-named empty-scope declaration. Runtime checks capability, active/initializing state and the canonical permission class before calling Core. Only these automatic services are exposed; persistent/session/action/restricted services are not dispatched through this bridge. Unknown names return not_supported. Registry membership does not implement a service. App-supplied principals/grants are never accepted.

- `ignite.storage_get(key)` returns string/error or nil/error.
- `ignite.storage_get(key, default_integer)` parses saved i32 text; missing key or implementation returns the default and corresponding error. Invalid numeric contents return nil/invalid_argument; corrupted storage is not hidden by defaults.
- `ignite.storage_set(key, string_or_integer)` and `ignite.storage_delete(key)` return an error code. Integers are i32; values are stored as UTF-8 or decimal text.

These require empty-scope storage.private. Keys: 1..32 `[a-z0-9_]` characters; strings: at most 192 UTF-8 bytes, no NUL. Each package has at most 16 keys and a 4096-byte file. Core derives the namespace from the active manifest. `app-<package-id>.kv` stores a version, package ID, claimed publisher, pairs and CRC-32; publisher claims and CRC do not authenticate identity. Existing writeAtomic replaces the file; failed writes retain the previous file. At most one successful write per second per Core instance, including application switches; process restart resets the throttle. Apps should coalesce changes and handle quota_exceeded. Same-ID/publisher updates and rollback retain data; uninstall deletes it. Apps migrate their own schemas. This is not encrypted/secure storage, backup or shared filesystem access. Physical power-loss durability depends on the filesystem adapter and has not been verified here.

### UI and diagnostics

`ignite.value(key, value)` additionally accepts UTF-8 strings up to 192 bytes. There are at most 16 published keys, with existing aggregate text/scene limits. `${app.key}` only reads app-published data; strings are not reinterpreted as templates.

`ignite.set(node, property, integer)` adds color (RGB565, 0..65535), visible (0/1), and image for image nodes. The first/background node stays immutable. image returns an error code and accepts only a package-owned FLIM section after ui.resources, format, dimension and length checks; it updates node dimensions from the image. Other successful set operations return no values. Source JSON nodes may have unique IDs matching `[a-z][a-z0-9_]*`, at most 32 characters. The compiler creates a lexical `ui` table, e.g. `ignite.set(ui.indicator, 'visible', 0)`. Names count toward the 16 KiB Lua source limit; the binary scene format and node numbering are unchanged.

`ignite.log(text)` accepts up to 128 UTF-8 bytes and returns an error code. The first eight records per run are retained; further writes return quota_exceeded. Apps gain no stdout/filesystem access. `Runtime::diagnostic()` retains error, load/update/event phase, Lua message with source line, used/peak memory, consumed instructions and logs after VM teardown. Core retains the bounded last-failure report across recovery and exposes it through authenticated Companion diagnostics. Messages are limited to 160 bytes, logs to 8×128; no native pointers or debug interface are exposed.

### Reproducible scenarios

```sh
python3 tools/ignite.py build examples/counter/project.json /tmp/counter.ignit --validator /tmp/ignite-build/ignite-check
python3 tools/ignite.py run /tmp/counter.ignit --scenario examples/counter/scenario.json --runner /tmp/ignite-build/ignite-run --image /tmp/counter.ppm
```

run uses shared Core and a virtual monotonic clock. JSON contains exactly steps (1..4096 entries, file at most 256 KiB). Operations: advance with ms 0..86400000; input with kind and optional value/expect (expected enqueue error); suspend, resume, restart (new application run), reboot (new Core with retained storage); expect_text, expect_state, expect_error (Core's last logic error); permission with permission/scope/enabled via the trusted Companion API. Revocation applies to implemented grant permissions, not automatic permissions. Unknown fields/operations and wrong types are rejected. The first failed assertion exits with a nonzero status and step number. --storage retains state across CLI runs; --factory supplies factory packages for recovery tests. Otherwise storage is temporary. Scenarios do not verify display/input drivers, physical BLE, Flash durability, device RAM/stack, latency or battery life.

## Memory budgets and launch

`Platform::memory` defines stable package admissibility. Do not populate it with current free heap: temporary memory pressure must not hide an installed package or trigger rollback. Optional `available_memory` is checked by Runtime at launch only, separately for internal RAM and PSRAM. Failure returns `resource_exhausted`, preserving the catalog and the previous app until ownership transfer. A verified installed update is not rolled back solely because activation runs out of memory; Core attempts the factory clock and records the launch error.

The Lua heap still cannot exceed `declared internal - 16 KiB`. The declaration additionally reserves estimated native manifest/component allocations, `LuaApp::Impl` (including published values), and Lua source at the loading peak. The Lua heap is reduced when necessary; its 16 KiB minimum remains. An older package whose actual native peak exceeds its declaration may now fail launch; package format and ContractRevision remain unchanged. Impl allocation failure is explicit, and failed launch releases the file and resources even before lifecycle.create.

Diagnostics distinguish `runtime_resident_bytes` (fixed Runtime, including ResourceManager and its buffers, already included in the platform baseline), native heap/peak estimates, and Lua used/peak. STL capacity estimates exclude allocator headers, fragmentation, stack, validation/scene bindings and hardware queues. The adapter must measure the full peak and largest free block; estimates do not guarantee total RAM.

## Touch and interactive system screens

Touch packages declare `app_api: 1`, `CapTouchEvents=2048` and `CapInteractiveUi=4096` alongside Lua/Declarative/AppApi (6337 combined). These are Runtime API capabilities, not a claim that the device has a physical touch sensor. Input requires `input.focused`, navigation `ui.navigation`, and catalogue access `packages.list`.

`InputEvent` retains its leading `kind/value/generation` fields and adds logical `x/y`, `pointer` (0..15) and monotonic-ms `at`. New kinds are `touch_down`, `touch_move`, `touch_up`, `touch_cancel`. Coordinates are bounded to −320..640 for movement outside the viewport; stale generations are rejected. Consecutive moves for one pointer are coalesced, without replacing down/up/cancel. `event(kind, value, x, y, pointer, at)` avoids allocating a Lua table for each sample; older handlers ignore the extra arguments. Adapters use the Core clock and wake its owner after queuing input. No physical touch driver is added.

- `ignite.read('packages.list', 'count')` returns the tile count and error; `revision` detects catalogue changes.
- `ignite.catalog(index)` returns `name, target, error`, with zero-based indices matching catalogue icons. Names fit grid metrics; targets are validated `package/component` strings.
- `ignite.action('role', 'clock'|'launcher'|'settings')` and `ignite.action('launch', target)` return an error and queue navigation after Lua returns. A focused release/up callback is required; load/update/timer calls return `user_interaction_required`. Deferred actions are generation-bound.
- `ignite.action('system', command)` additionally requires a factory package with the active Settings role. Commands are `ble_toggle`, `offset_up`, `offset_down`, `language`, `motion`. Claiming Settings metadata does not grant a user package this authority.

Host compilation supports `logic.includes`: at most eight relative UTF-8 Lua sources within the project root, concatenated before `logic.path`. The original 16 KiB source bound includes generated UI IDs. Runtime gains neither `require` nor filesystem access. Base Lua screens declare 48 KiB internal memory and 4096 instructions; platform limits are unchanged.

Persistent `reduce_motion` accepts `0/1` through standard `settings_get/settings_set`. It disables automatic inertia and introductory movement while preserving direct finger tracking. Flare Preview and physical controllers need corresponding event adapters; SDK host tests inject events directly through Core.

## Application icons

See [FLIM.md](FLIM.md) for FLIM v2, FLM2 manifest resources, theme selection, converter commands and bounded memory. Example: `examples/icons/project.json`. `ignite.catalog(index)` returns `name, target, error`; the placeholder symbol is removed. `catalog_icon` nodes support `ignite.set(node, "catalog", index)`. All consumers and packages require revision `0x20261112`.

Factory Clock and Settings provide authored color INDEX8 icons and separate A4 masks. Source PNGs live in `system/assets/`; reproduce their geometry with `python tools/build_system_icons.py`, then rebuild packages using `python tools/build_system.py`. These run on a Pillow-equipped host, never in firmware. Read-only numeric `utc_offset_minutes` under `system.settings.read` provides a typed UTC value; the original string `utc_offset` remains available.

## Generation without source-tree writes

Pillow is required for factory package generation during CMake configure, including builds with tests disabled.

CMake automatically runs `cmake/FlintGenerate.cmake`. Outputs live in `<build>/generated/include/flint/`, `<build>/generated/factory/` and `<build>/generated/examples/<name>/sample.ignit`. The library searches generated headers before source headers, and SDK installation exports the generated versions. Tests receive `FLINT_FACTORY_DIR` and `FLINT_EXAMPLES_DIR` and do not rely on checked-in binary packages.

Direct `generate_registry.py` defaults to `build/generated/include`; `build_system.py` defaults to `build/generated/factory` and `build/generated/examples`. Independent builds must select their own output directories:

```sh
python3 -B tools/generate_registry.py --output build/generated/include
python3 -B tools/build_system.py --output build/generated/factory --examples-output build/generated/examples
# Firmware: compile only factory packages.
python3 -B tools/build_system.py --output build/firmware-factory --skip-examples
```

Unchanged outputs retain their timestamps. Changed files publish through individual atomic replacements; an input compilation failure leaves previous packages untouched. The entire package set is not an atomic transaction: consumers must wait for successful generator completion. One build owns each output directory; do not point it at source `factory/`, `examples/` or `include/`.

`build_system_icons.py --output <dir>` and `build_builtin_fonts.py --font-dir <fonts> --output <dir>` also default to `build/generated/`. Updating maintained PNG/font sources requires a separate reviewed copy, never an implicit build side effect. Existing source factory/sample packages and headers remain snapshots for older consumers; new CMake builds neither refresh them nor use them instead of current generated artifacts.

Current touch/UI contracts use `ContractRevision=0x20261112` with nominal Application API 1. Consume the matching SDK tree as a whole: headers, `core_ui.cpp`, `core_touch.cpp`, assets and system packages. Equal nominal API versions do not make older revisions compatible. Flare still reading source `factory/` and `examples/` requires a separate resource integration migration; the portable `Flint::Platform` target already uses the new scheme.
