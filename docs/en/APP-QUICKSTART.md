# Your first Ignite application

[Index](INDEX.md) · [Russian](../ru/APP-QUICKSTART.md) · [Lua reference](LUA-API.md)

## 1. Tools

Install Git, CMake 3.21+, a C/C++20 compiler and Python 3.11+/Pillow. Lua 5.4.9 is
bundled. On Windows use a Visual Studio developer shell or one consistent MinGW kit.
Run these PowerShell commands from the **IgniteSDK** root:

```powershell
python -m venv build/docs-venv
build/docs-venv/Scripts/python -m pip install Pillow
cmake -S . -B build/docs-sdk -DPython3_EXECUTABLE="$PWD/build/docs-venv/Scripts/python.exe"
cmake --build build/docs-sdk --config Debug --parallel 2
ctest --test-dir build/docs-sdk -C Debug --output-on-failure
```

Expect `100% tests passed`. Linux/macOS: use `python3`, the interpreter at
`build/docs-venv/bin/python`, add `-DCMAKE_BUILD_TYPE=Debug`, and use executables in
`build/docs-sdk/` instead of `Debug/`. These adaptations are instructions, not a claim
of execution on every OS. Pillow is needed at configure time for factory assets too.

On Windows Ninja/MinGW replace `build/docs-sdk/Debug/ignite-check.exe` and `ignite-run.exe` with `build/docs-sdk/ignite-check.exe` and `build/docs-sdk/ignite-run.exe`; the Debug directory is Visual Studio-specific.

Create temp/ for CLI scratch; PowerShell: `$env:TEMP="$PWD/temp"; $env:TMP=$env:TEMP`; Linux/macOS: `mkdir -p temp; export TMPDIR="$PWD/temp"`.

## 2. Build and validate

`init` creates a declarative watchface without Lua and refuses an existing destination.
Use the maintained counter example for Lua/input:

```powershell
build/docs-venv/Scripts/python tools/ignite.py init build/my-clock --id demo.clock --publisher demo
build/docs-venv/Scripts/python tools/ignite.py build build/my-clock/project.json build/my-clock.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py check build/my-clock.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py build examples/counter/project.json build/counter.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py run build/counter.ignit --scenario examples/counter/scenario.json --runner build/docs-sdk/Debug/ignite-run.exe --image build/counter.ppm
```

Success means exit 0, validator `runtime_compatible:true`, and a PPM image. This does
not validate board capacity or publisher signatures. `run` uses virtual monotonic
time without a window. For interactive Preview, build Flare, start Simulator and
open Ignite Studio. Physical USB/BLE is separate. Do not feed synthetic presses to
the declarative clock: it does not declare `input.focused`.

## 3. Project layout

[counter/project.json](../../examples/counter/project.json) is the complete buildable
example. Root fields are `id/name/publisher/version/components`, resources and icons.
Each component owns `type/budget/capabilities/permissions/logic` and UI if applicable.
`app_api/sensor_api/ipc_api` belong inside a component; old root fields are rejected.
Compiler reads `logic.path` and up to eight `logic.includes` within the project root,
combining at most 16 KiB of Lua. Runtime has no `require`.

Declarations request access; a trusted user action creates a Core grant. Publisher
metadata is not proof of authenticity. The 48..144 KiB Lua budget includes native
and loader allowances, not just Lua heap. Do not silently raise quotas after OOM.

## 4. Sensors and a service

Build/check [sensors](../../examples/sensors/project.json) first. Temperature needs
`sensor_api:1`, `app_api:1`, Lua capability 64 and the component declaration
`sensors.temperature`, then an explicit persistent grant in Ignite Hub. Source 0
selects a deterministic source for the requested channels. Calls read a cache or
queue work; Lua never waits for I2C/conversion. Absence/errors are not zero readings.
See [units, callbacks and freshness](SENSORS.md).

[thermometer-service](../../examples/thermometer-service/project.json) contains UI
`main` and service `worker` in separate VMs, with statically included `shared.lua`:

```powershell
build/docs-venv/Scripts/python tools/ignite.py build examples/thermometer-service/project.json build/thermometer.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py run build/thermometer.ignit --scenario examples/thermometer-service/scenario.json --runner build/docs-sdk/Debug/ignite-run.exe --factory build/docs-sdk/generated/factory --image build/thermometer.ppm
```

Scenario `permission` steps are a **trusted test driver**, not an application grant
API or automatic device consent. The scenario covers start, UI-independent service
execution, sensor revoke and stop. In Flare Simulator explicitly grant execution,
temperature and humidity, then start `worker` in Diagnostics. Closing UI leaves the
collector running; Services provides stop/revoke. Autostart needs a separate grant
and preference. The example retains six paired readings in RAM and flushes at most
once per minute. See the [service contract](BACKGROUND-SERVICES.md).

## 5. Install

Connect Flare, wait for a compatible handshake, select the `.ignit` in Ignite Hub,
wait for installation completion and explicitly grant required access. Launch UI
and service separately. Active Spark without configured sensors returns `not supported`;
requests never enable commented modules. [Device operations](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/en/USER-GUIDE.md).
For `incompatible_contract` rebuild with matching SDK/firmware/factory. For permission
errors check the calling component declaration and confirmed grant. For
`resource_exhausted` check admission/budget; do not erase user data to bypass errors.

Use source/compiler from one coordinated SDK revision. Rebuild the UI+service package from the current project.json; an old binary snapshot does not substitute for corrected sources.
