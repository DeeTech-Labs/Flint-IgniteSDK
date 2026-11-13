# Contributing to IgniteSDK

[Canonical Russian](../../CONTRIBUTING.md) · [First application](APP-QUICKSTART.md) · [Index](INDEX.md)

For a small change start with its page/source, not the whole architecture. Report a minimal
project/Lua, revision, OS/toolchain, command, expected/actual result in
[Issues](https://github.com/DeeTech-Labs/Flint-IgniteSDK/issues); questions in
[Discussions](https://github.com/DeeTech-Labs/Flint-IgniteSDK/discussions). Never publish
pairing keys, secrets, private storage/grants or security exploits with user data.

## Setup and checks

Git, CMake3.21+, C/C++20, Python3.11+, Pillow in selected CMake interpreter before configure.
Lua is bundled. [Platform commands](APP-QUICKSTART.md).

```sh
python3 -m pip install Pillow
cmake -S . -B build/contrib -DCMAKE_BUILD_TYPE=Debug
cmake --build build/contrib --config Debug --parallel 2
ctest --test-dir build/contrib -C Debug --output-on-failure
python3 -B tools/check_docs.py
git diff --check
```

Visual Studio executables are under Debug/; Ninja directly under build/contrib. Never mix
kits in one build. Builds/scratch belong only to owning build/ and temp/; an independently
built submodule uses its own platform/build/. No build/root scratch artifacts.

Docs: links/anchors plus changed commands/scenarios, report unavailable platforms. Compiler/
registry: contracts, registry_validation, sdk_workflow, package_interop, generation_isolation,
all examples. Lua/permissions/lifecycle: full Lua tests, sensor_core/background_core/
service_isolation and no-Lua build. Storage: async_install/core_integration, write failures,
disconnect/generations; power-cut separately. Rendering: decoder/resources/icons/touch tests,
captures and firmware build. Shared contracts: OS host/tooling/firmware plus Flare tests and
deliberate pins. `ctest ... -N` lists configured names.

```sh
cmake -S . -B build/no-lua -DFLINT_ENABLE_LUA=OFF
cmake --build build/no-lua --config Debug --parallel 2
ctest --test-dir build/no-lua -C Debug --output-on-failure
```

`check_docs.py --examples --runner <ignite-run> --validator <ignite-check>` compiles examples/
runs maintained scenarios under temp/. `--ecosystem <parent>` checks all three local checkouts,
not GitHub publication or remote website availability.

## Code and review

include/flint: public interfaces; src: portable execution; spec: registry truth; tools:
compiler/generators; system: factory source; examples/tests: reproducible behavior.
[Extension guide](PLATFORM-GUIDE.md). Build-local generation does not mutate source snapshots.
No handwritten generated registries, duplicated OS authorization or task per service. Keep
hardware in OS, Qt in Flare. Require RAII, bounded resources, owner/generation/cancellation,
validated input, live permission checks at operation/delivery, no hardware during validation.
ABI freedom does not permit data loss or weakened recovery.

Preserve others' changes; inspect git/submodule status. Use focused docs/fix/feat branches and
English Conventional Commits. No commit/push without authorization. PR describes problem,
result, compatibility/security/migration, commands actually executed and limits. Update
Unreleased changelog, RU/EN and links; do not alter licensing without permission.
Owning SDK changes precede coordinated exact consumer pins; never silently pick a sibling or
copy SDK sources. No legacy adapters for superseded development contracts.

Separate host tests, firmware build and hardware runs. Give board/electrical revision, firmware
SHA and measurement conditions. Without hardware explicitly mark heap/PSRAM/DMA, stack,
latency, BLE/USB, power, Flash durability and sensor recovery unverified.
