# Lua API: implemented calls

[Index](INDEX.md) · [Canonical Russian](../ru/LUA-API.md) · [First application](APP-QUICKSTART.md)

Implementation owners are `src/lua_app.cpp`, `src/runtime.cpp`, `src/service_runtime.cpp`
and Core host bridges. This is implemented API, not every registry entry. Set component
`app_api:1`; sensors require `sensor_api:1`, IPC/state `ipc_api:1`. Each component declares
its own permissions; declarations do not grant access. Core selects installed identity.

## Context and errors

Integer error 0 means success; [generated codes](REGISTRY.md). Invalid argument types/counts,
timer ranges or sandbox exhaustion can raise a Lua callback fault instead of a normal error.
Handle returned errors; nil is not a measurement of zero. Services cannot prompt for grants.
Sensors and IPC/state reject loading-phase calls with `invalid_state`; service host calls
also reject load/pause/stop. Foreground schedule during load is supported; perform external
work from active callbacks for portable behavior. Service work starts at `event('start',0)`.

UI chunks must return `update(dt_ms,pressed)` (an empty function is valid with schedule0);
optional `event(kind,value,x,y,pointer,at)` receives scalar coordinates/pointer/monotonic timestamp, not an input table. With App API,
focused input uses event. Timer delivery is `event('timer',id)`; [SDK](SDK.md) defines touch
fields/cancellation. Service events are start/pause/resume/stop, timer, sensor and IPC;
update runs only with schedule. Resume must recreate timers/subscriptions. Missed periods
do not replay indefinitely. [Lifecycle and reason values](BACKGROUND-SERVICES.md).
All callbacks in one tick share instruction/native budgets; input precedes sensor callbacks.
No service Scene/focus/UI APIs. Standard Lua libraries, require/load, OS/filesystem/network/
debug/native pointers are not exposed by the sandbox.

## Reads and timers

`ignite.read(name[,key]) -> string_or_integer_or_nil,error`:

| name / required permission | Result/context |
| --- | --- |
| `time.read` | UTC epoch seconds; UI/service, not a deadline clock |
| `time.monotonic` | monotonic milliseconds; UI/service |
| `system.info` | profile; UI |
| `system.power.status` | backend keys voltage_mv, age_ms, measured_at_ms, status; UI |
| `system.locale.read`, `system.theme.read` | string selected by key; UI |
| `system.settings.read` | string/integer; utc_offset_minutes and ble_available are integer; UI |
| `packages.list` | bounded UI list; use catalog for selection |

The required permission has the same name. Unsupported names/backends return `not_supported`,
missing supported keys may return `source_unavailable`. Do not infer charging/SOC. Service
read supports only time from this table; it does not expose Companion data.

`ignite.schedule(interval_ms) -> error`: 0 disables update, otherwise 1..86400000 for UI,
subject to service model minimum (default 100 ms). `ignite.timer(id,delay_ms,repeat_boolean)
-> error`: IDs 1..8, delay 0 cancels, otherwise up to 86400000 and model minimum. Reusing
an ID replaces that instance's timer. Both require `time.timer.create`.

## UI and private storage

| Call | Result | Requirements/limits |
| --- | --- | --- |
| `ignite.value(key,value)` | none | UI; 16 keys; i32 or UTF-8 string <=192 bytes; `${app.key}` |
| `ignite.set(node,property,integer)` | normally none; image returns error | UI index/named ui.id; x/y/w/h/color/visible/image/catalog; first background protected |
| `ignite.catalog(index)` | name,target,error | packages.list; zero-based, bounded |
| `ignite.action(key,value)` | error | UI navigation; current trusted focused user input |
| `ignite.log(text)` | error | UI, first 8 messages, UTF-8 <=128 bytes |
| `ignite.storage_get(key)` / `storage_get(key,default_i32)` | string,error / integer,error | storage.private; numeric default only |
| `ignite.storage_set(key,value)`, `storage_delete(key)` | error | storage.private; i32 or string <=192 bytes |

Image IDs reference owned prepared FLIM resources with `ui.resources`; no device PNG/URL
loading. [Resources](FLIM.md), [input/navigation](SDK.md). Factory actions require actual
factory provenance; `ignite.services` is not third-party management API. Private UI storage
keys are 1..32 `[a-z0-9_]`, not paths: 16 keys/4096-byte file, one successful write per second
globally per Core. Batch writes; not encrypted storage. Update/rollback of the same ID/
publisher retain app kv; uninstall removes it. Services do not get synchronous kv functions.

## Shared state and IPC

| Call | Result | Requirement/limit |
| --- | --- | --- |
| `ignite.ipc.send(component,topic,value)` | error | app.ipc; same package generation; target <=64, topic <=32 bytes |
| `ignite.state.read(key)` | value,revision,error | storage.private; key <=32, 16 keys |
| `ignite.state.commit(expected_revision,writes)` | revision,error | atomic RAM batch 1..8; conflict code16 |
| `ignite.state.flush()` | ticket,error | async; request success is not durable acknowledgement |

Values are boolean, int64, finite double, UTF-8 string <=96 bytes, never Lua references.
Callbacks: `message(sender_component,topic,value)`, `state(revision)`, `persisted(ticket,error)`,
`policy(value)`. Authorization is checked at delivery. Inbox 8; full messages rejected,
state/policy coalesce. Global flush minimum defaults to 60000 ms; install takes priority.
Instance cancellation removes pending flush/results; shared RAM belongs to the package.
[Persistence](BACKGROUND-SERVICES.md).

## Sensors and outputs

| Call | Result | Requirements |
| --- | --- | --- |
| `ignite.sensors.catalog(index)` | metadata,error | 1..16, no sampling |
| `ignite.sensors.read(source,mask,last_known_boolean)` | snapshot,error | exactly three arguments; cache only |
| `ignite.sensors.refresh(source,mask,timeout_ms)` | handle,error | async; 1..60000 ms |
| `ignite.sensors.subscribe(source,mask,interval_ms)` | handle,error | async; source/model bounds |
| `ignite.sensors.unsubscribe(handle)` | error | own refresh/subscription only |
| `ignite.beep(hz,duration_ms)` | error | audio.play; service also audio.background; actual buzzer |
| `ignite.brightness(percent,duration_ms)` | error | UI only; live ui.brightness session grant |

Callback: `sensor(handle,snapshot,error,drops,refresh_finished)`. Source0 selects deterministically.
[Sensor reference](SENSORS.md) owns masks, SI units, identity, status, timestamps, age and axes.
8 subscriptions/8 refresh per instance, hard global 40 each. Revoke stops future delivery,
not values already handed to the application. No hardware means not_supported, never fake values.
OS beep limits 100..10000 Hz, 5-second continuous burst including retunes, command spacing/duty
cycle; this is not playback/haptics. Brightness 5..100%, 1..5000 ms restores system policy.
Backend may be stricter.

Complete compiled examples: [counter](../../examples/counter/project.json),
[sensors](../../examples/sensors/project.json), [UI + service](../../examples/thermometer-service/project.json).
Include their component declarations; copying Lua alone is insufficient. Quickstart covers
build/check/run, trusted scenario grants and Flare installation.
