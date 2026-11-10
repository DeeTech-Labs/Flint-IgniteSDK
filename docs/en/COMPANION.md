# Companion Demand v1

Negotiate capability `256` through the existing `hello`. FLCP v1, existing method
IDs and `ContractRevision` stay unchanged; package compatibility is unaffected.
Ignore unknown optional capabilities. All integers are little-endian.

Requests/replies retain nonzero IDs. Events use `reply=0`, `id=0`, method 25 and
have no Error prefix. An event must never complete a pending request or reset its
timeout. Transports serialize complete frames; events may occur between replies.

| ID | Method | Request payload |
|---|---|---|
| 22 | companion_subscribe | u16 version=1, u8 enabled (0/1) |
| 23 | companion_bind | u16 version=1, u8 kind=1, u64 demandGeneration |
| 24 | companion_sample | u16 version=1, u8 kind=1, u64 demandGeneration, u32 cpu, u32 ram |
| 25 | companion_state | event only |

Kind 1 denotes CPU/RAM. Samples are hundredths of a percent (0..10000).
Event payload: `u16 version=1, u8 kind=1, u64 demandGeneration, u8 state,
u64 consumerGeneration, u32 sampleIntervalMs=2000, u32 ttlMs=10000`.
States: 0 = no active consumer, 1 = permission denied, 2 = demand active.
Subscribe delivers an initial snapshot. Four subscribers maximum; subscribing
again reuses the slot. Changes coalesce into the latest snapshot without an event
queue. Events are limited to one per subscriber per 100 ms except the immediate
initial snapshot. Snapshots fit within 64 bytes.

Subscribe/bind require an authenticated compatible Session with a nonzero ID.
Bindings identify the provider session, data kind and demand generation. Application
changes, suspend/resume, catalog changes and live permission changes invalidate
bindings and samples. Another session cannot replace a live binding; disconnect
or demand changes permit rebinding. Accept at most one sample per 1000 ms, with a
10-second TTL. SDK checks permissions on binding, reception and application reads.

Legacy metrics (16) remains session/consumer-bound for old clients, whose delayed
samples cannot carry a demand generation. New providers exclusively use methods
22–25. A foreign legacy request cannot replace an existing binding.

Core owns all state. Adapters call `takeCompanionEvent` only with TX capacity and
send complete frames. SDK has no FreeRTOS/USB/BLE dependency. Local applications
continue running without a provider.
