# Flint IgniteSDK

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
Failed builds preserve the last successful package. The JSON report gives declared
budgets and runtime compatibility, not measured hardware fit, publisher authenticity,
market admission, or battery-life guarantees.

## Lua application

`logic: {"language": "lua", "path": "main.lua"}` adds one text-source section.
A Lua package has exactly one app/watchface component, capability bit 64, 48..144 KiB
of declared internal memory and 1..100000 Lua instructions per callback. Source is
limited to 16 KiB. Binary chunks are rejected. The current development contract is
0x20261105; regenerate packages using this SDK. There is no previous-format loader.

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

Physical validation on the 4 MiB Flash / 2 MiB PSRAM Spark on 2026-11-04 passed
scene animation, 30 app switches, loop/allocator/forbidden-call failures, excessive
parser nesting and failed-initialization recovery. Game VM peak was 8091 bytes;
minimum observed free main-task stack was 17808 bytes. See the firmware repository's
`docs/validation/spark-lua-2026-11-04.json` for the measured build identity and raw
results. These measurements cover one board and workload, not all applications.

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
C code, parsing, garbage collection or filesystem recovery. Add permission-checked
persistence/device APIs, interactive input, styles and Studio editing separately.
Marketplace signatures and publication remain separate work.
