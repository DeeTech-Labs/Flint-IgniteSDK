# Extending the shared platform

[Index](INDEX.md) · [Canonical Russian](../ru/PLATFORM-GUIDE.md) · [Contributing](CONTRIBUTING.md)

SDK owns Core/Runtime/ServiceRuntime, authorization, Broker, package transactions,
sandbox, codecs/compiler/generation. OS owns drivers, bus, scheduling/recovery/TOML.
Flare owns consent presentation, transport orchestration, Studio/Preview and virtual data.
Do not duplicate implementations. Registry presence is not backend availability.

## Execution and ownership

Core serially executes foreground and service VMs on its owner loop. Core-issued HostBinding/
InstanceKey identifies every caller; service identity never comes from foreground runtime_.
Installed package identity is validated; publisher text is not a signature. One Broker cache
combines system/foreground/service demand and filters channels. A bounded backend mailbox
exchanges intents/snapshots with OS hardware worker; all I2C clients including touch/RTC
belong to that worker. No Lua/lifecycle calls from worker, no I2C/Flash/Lua under exchange locks.
Simulation reuses Core/Simulation with controlled monotonic time. Remote Preview uses device
scenes without executing a second remote-data Lua copy.

## Add a source or channel

An additional physical implementation of an existing channel belongs in OS backend/TOML,
not a new app API. Normalize units/axes at the boundary; preserve generation/sample identity,
freshness and deterministic source choice per [sensor contract](SENSORS.md).
A new logical channel is a coordinated shared change: sensors.h, spec/sensors.json,
generate_registry.py, sensors.cpp, Lua typed conversion, sensor_simulation.h, permission
spec/authorizer, compiler validation, protocol/Companion and both consumers. Update revision,
bounds and sensor_broker/core/tooling plus Flare sessions. No hot-path JSON data serialization.
[OS driver guide](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/en/development/extending.md).

## Add an operation/permission/method

Inspect existing Lua/app_api/Runtime/ServiceRuntime/Core implementations first. Define owner,
permission, contexts, result/error, quotas and cancellation before interface. Permissions and
methods belong in canonical spec JSON, Method/codecs change together; regenerate headers.
Core checks component declarations/live grants and actual factory provenance. A VM cannot
create trusted identity/action tokens/dialogs. Async delivery is instance/generation-bound;
stop/revoke cancels pending work. Bound queues and coalesce repeated updates. Charge native
API/memory/stack and prohibit inactive work. Native service calls must be bounded/nonblocking;
Lua hooks cannot interrupt C. Test malformed input, denial/revoke before delivery, old handles,
switch/pause/crash/update, flood/quotas and transports. [Formats](ARCHITECTURE.md),
[registry](REGISTRY.md), [service wire](BACKGROUND-SERVICES.md).

## Scheduler, data and integration

core_background.cpp/state header/service_runtime.cpp own slots, admission, round-robin and
one fixed service pool. Foreground/input run first with separate existing budgets. Reserve
a full quantum before consuming events; exhausted global tokens defer work, individual
overruns fault only that instance. Idle services do not poll. Pause/stop reject new work before
cancellation; failing shutdown callbacks cannot retain VM resources. No automatic retries of
faulted instances in the current Core. Count native wrapper/manifest/source/inbox/shared/flush
memory, not only Lua heap; reserve foreground and OS headroom. [Details](BACKGROUND-SERVICES.md).

storage.cpp validates container/index/hash/manifest; PreparedInstall advances bounded work.
Core stops services/revokes grants before replace/remove; updates cannot grant new permissions.
Disconnect after durable publication does not establish cancellation: reread catalog.
Retain incompatible installed objects; ABI freedom is not permission to erase data/grants.
App kv/shared formats differ; CRC is not authentication. Mocks cannot prove physical durability.

Change owning SDK first; run Lua/no-Lua/compiler/examples/bounds tests. Update ContractRevision
and changed format; nominal envelope/framing versions only when those change. Test consumers
with explicit FLINT_PLATFORM_DIR, then after an authorized commit pin its exact SHA in both
consumers and rebuild normally. A sibling checkout never silently updates submodules. Do not
copy SDK documentation into consumers; use owning-project links and update pins deliberately
alongside validated integration.

## Checks

[Contributing](CONTRIBUTING.md) defines minimum checks. Core/Runtime: app_api, sensor_core,
background_core, service_isolation. Contract/compiler: contracts, sdk_workflow,
registry_validation, package_interop. Storage/lifetime: async_install, generation_isolation.
`ctest -N` lists actual configured names. Host/ESP32 builds do not prove IRQ latency,
stack watermarks, PSRAM/DMA behavior, power or power-cut recovery.
