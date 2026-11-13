# Background services and component isolation

[Russian specification](../ru/BACKGROUND-SERVICES.md) · [SDK](SDK.md) · [Sensors](SENSORS.md)

## Contract and migration

Development revision is `0x20261115`. The manifest magic is `FLM3` (manifest version 3); nominal OS API, Face SDK, Ignite package envelope and Companion versions remain 1. Container headers, index records and Companion framing are unchanged. Exact ContractRevision matching rejects older packages/peers. Rebuild all owned factory/examples/apps and update SDK consumers together. Incompatibility is not corruption: installed objects/references, private storage and grants are not automatically deleted. Existing grants/autostart are not expanded to a new package generation; the corresponding consent is needed after update.

In `project.json`, `logic`, `budget`, `permissions`, `capabilities`, `app_api`, `sensor_api` and `ipc_api` belong to each `components` entry. Executable components have explicit manifest `logicSection` references. Root fields hold identity/version, components, resources and icons. The derived package permission union supports consent enumeration; runtime checks the calling component's own declarations. Package grants may be shared, but cannot replace a missing component declaration. There is no implicit inheritance or legacy manifest fallback.

A package can contain UI and at most one `type: "service"` component. See [thermometer-service](../../examples/thermometer-service/project.json). Capability `16384` identifies background execution and `32768` component IPC; the compiler adds them for service/`ipc_api: 1`. `sensor_api: 1` adds `8192` and requires Application API/Lua.

## Ownership and lifecycle

One Core executes foreground Runtime and separate ServiceRuntime instances on its existing owner loop. Each service owns a Lua VM, validated package identity, component budget and Core-issued instance identity. The package file is closed after validation/source loading and before VM execution; admission charges the loader peak. There is no second FreeRTOS task/Core, service Scene/framebuffer, focused input or rendering API. Lua cannot select its principal/instance or access another VM pointer.

Services do not receive `ignite.set/value/log`, UI navigation, synchronous private storage, brightness or `ignite.services`. Allowed interfaces cover time reads, schedule/timer, sensors, IPC/shared state. Buzzer needs Core-checked `audio.play` and `audio.background`; this is not background audio-file playback. Unsupported operations return `not_supported`. Privileged operations are disabled during loading.

After admission, the first active dispatch invokes `event("start", 0)`. Lifecycle callbacks include `event("pause", reason)`, `event("resume", 0)` and `event("stop", reason)`, alongside timers and sensor/IPC callbacks. New operations are already disabled during pause/stop; lifecycle callback failure still releases handles/resources and faults the service. Resume does not replay missed timer ticks: callbacks restore required timers/subscriptions. One service fault does not stop foreground or another service.

Closing UI or switching packages leaves services running. Pause/stop/revoke, update and uninstall cancel the affected instance/package resources only. Handles and messages do not transfer to a new instance. Execute revocation also releases paused instances. Failed durable revocation returns `io_error` and denies access in RAM; UI must not claim it was saved. The previous durable grant may return after reboot if persistence failed.

`app.background.execute` is persistent launch consent. `app.background.autostart` is separate persistent autostart consent. Declarations, current grants and an explicit generation-bound preference are all required; declaration/installation alone starts nothing. A crash disables the autostart preference, preventing automatic restart loops. Low-energy policy pauses services and resumes only eligible instances with a valid execute grant.

## One bounded service pool

Model/host `Platform.background` configures maximum count, RAM pool, instruction/native/wall budgets, quantum and minimum intervals. SDK hard maximum is 4; models can allow fewer or zero. Accounting includes infrastructure, VM reservations, shared state and pending flush; admission also checks available memory. `resource_exhausted` preserves an already running UI/other service. Foreground reserve remains separate.

All services share a **fixed** instruction/native/wall pool per monotonic window. Adding VMs does not multiply it. Ready instances rotate, at most one callback batch per Core pass. A quantum is reserved before consuming events, and unused tokens are refunded. Insufficient global tokens defer execution until replenishment, without busy polling or faulting an innocent service. All callbacks of one dispatch share its allowance.

Foreground runs before services and retains its separate existing instruction/native budget; there is no combined foreground+service wall pool. Exceeding a service's own allowance/deadline/native-call limit faults that instance. Lua hooks and native call boundaries check elapsed time. A blocked C callback cannot be preempted by this mechanism: overrun is detected after it returns. Parser, allocator, GC, VFS/Flash and actual latency need measurement. Quotas do not establish hard realtime or battery-life guarantees.

## Typed IPC and shared state

Communication is limited to live components of the same installed package generation. Target component identifiers are at most 64 ASCII bytes; topics/state keys at most 32. Values are boolean, signed integer, finite number or UTF-8 text up to 96 bytes. Lua tables/functions/userdata, pointers and arbitrary caller identities never cross VMs.

```lua
ignite.ipc.send('worker', 'refresh', true) -- error
local value, revision, error = ignite.state.read('temperature')
local next_revision, error = ignite.state.commit(revision, {status=0})
local ticket, error = ignite.state.flush()
```

Protected callbacks are `message(sender_component, topic, value)`, `state(revision)`, `persisted(ticket, error)` and `policy(value)`, sharing the dispatch budget. Messages require `app.ipc`; state requires `storage.private`. An inbox holds 8 events; overflow returns `quota_exceeded`; state/policy notifications coalesce. Management metadata reports rejected/coalesced counts. Stop/restart removes messages from the old sender.

Shared state has at most 16 keys. Commit accepts 1..8 changes and validates the whole batch, capacity and expected revision before any mutation, then commits atomically in RAM. Revision conflicts return `conflict` (16); invalid batches do not partially apply. Reads, commits and messages do not write Flash.

`flush()` queues a bounded asynchronous snapshot and returns a ticket, not durable acknowledgement; `persisted` reports completion. One global flush is serviced at a time, with at least model `minFlushMs` between attempts, default 60000 ms across all packages. Installation takes priority. State uses atomic replacement/integrity checking; this alone does not verify physical power-loss durability.

The thermometer-service example stores latest temperature/humidity/status and six paired readings: 12 history keys plus latest/status/history_next fill at most 16 keys. Each sample atomically changes six keys. History is bounded in RAM and flush is requested no more often than once per 60 seconds. There is no per-sample Flash write or unbounded log.

## Management and validation

Authenticated Companion `services` (30) returns paged policy/metadata without raw readings. `service_control` (31) performs explicit start/stop/pause/resume/autostart/revoke. Factory management uses `ignite.services.list(index)` and `control(action, package, component)`; list uses index 0..255; actions are 0=start, 1=stop, 2=pause, 3=resume, 4=autostart on, 5=off, 6=revoke execute. Controls require trusted focused input and `system.diagnostics`. An ordinary package cannot gain this privilege by its name.

### Companion wire 30/31

Framing remains v1/little endian; every reply starts with `Error:u16`. Text is UTF-8 with a u16 byte length, at most 64 bytes.

`services` (30) request: `offset:u16`, `limit:u8` (1..8), with `offset + limit <= 65536`. After Error, the reply contains `count:u8`, `more:u8`, `maxServices:u8`, `memoryBytes:u32`, `instructionsPerWindow:u32`, `nativeCallsPerWindow:u32`, `windowMs:u32`, `lowEnergy:u8`, followed by `count` rows:

- package/component/label: three text64 strings;
- state/reason: two u8 values; error:u16;
- instance_id/generation: two u64 values;
- used/peak/reserved: three u32 values;
- callbacks/instructions/native_calls/max_callback_us/wakeups/event_rejected/event_coalesced: seven u64 values;
- execute_granted/autostart_granted/autostart: three boolean u8 values (0 or 1).

`service_control` (31) request: `action:u8`, package:text64, component:text64. Actions 0..6 mean start/stop/pause/resume/autostart on/off/revoke execute. A successful acknowledgement has no payload after Error. Replies contain neither raw readings nor Lua state; metadata polling does not start a service.

Host integration tests cover isolation, ownership, CAS, budgets/fairness, pause faults, revocation, autostart, lifecycle, async flush and update/uninstall. Two-service admission in a 131072-byte host pool validates that profile's accounting. Actual ESP32 RAM/PSRAM, stack watermarks, latency, wakeups, Flash durability and power remain hardware checks.

MinGW 15.1 x64 host `sizeof` in bytes: Core 30776, Runtime 10984, ServiceRuntime 1720, Broker 14200, Simulation 4176, IpcRequest 1592, IpcEvent 280; native Lua state wrapper 4384. These are host ABI sizes, not measured ESP32 heap/stack. Before multi-owner support Broker used 9280 bytes under this ABI, a 4920-byte increase.

If persisting the crash preference latch fails, RAM remains disabled but an old durable autostart preference may survive reboot. The current Core session never retries a faulted service automatically.
