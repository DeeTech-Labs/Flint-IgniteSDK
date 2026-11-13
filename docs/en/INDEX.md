# FlinT Ecosystem documentation

[Canonical Russian index](../ru/README.md) · [IgniteSDK](../../README.md).

[Text and layout](UI-LAYOUT.md): optional bounded text blocks; application authors remain responsible for layout.

SDK owns shared API/permission/format documentation; OS owns firmware and hardware,
Flare owns desktop workflows. Cross-repository links are checked against local
checkouts; that does not assert that the new pages have been published on GitHub.

| Audience | Start | Continue | Success check |
| --- | --- | --- | --- |
| Device user | [Daily operations](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/en/USER-GUIDE.md) | [OS recovery](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/en/getting-started/device-use.md) | Compatible connection, installed package, confirmed grants |
| Ignite developer | [First app](APP-QUICKSTART.md) | [Lua API](LUA-API.md), [sensors](SENSORS.md), [services](BACKGROUND-SERVICES.md) | Validator and scenarios exit 0 |
| OS/hardware developer | [Build OS](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/en/getting-started/building.md) | [Add a module](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/en/development/extending.md), [SDK boundaries](PLATFORM-GUIDE.md) | Host/tooling tests and ESP32-S3 build; physical checks separately |
| Flare developer | [Build](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/en/BUILD.md) | [Extend Flare](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/en/DEVELOPMENT.md) | CTest, QML smoke and relevant session tests |
| Contributor | [Contributing](CONTRIBUTING.md) | Select a project and a focused check set | Diff, changelog, RU/EN and reproducible evidence |

Tutorial: [first app](APP-QUICKSTART.md). Task guides: [images/icons](FLIM.md),
[sensor access](SENSORS.md), [UI + service](BACKGROUND-SERVICES.md), [PC Monitor](PC-MONITOR.md).
References: [Lua](LUA-API.md), [generated registry](REGISTRY.md), [tools](SDK.md).
Explanations: [architecture/formats](ARCHITECTURE.md), [Companion Demand](COMPANION.md),
[platform extension](PLATFORM-GUIDE.md), [licensing](LICENSING.md), [FLIM measurements](FLIM-BENCHMARK.md).

Contract `0x20261115` requires FLM3 manifests and FLIM v2; nominal API/package/Companion
version 1 alone is insufficient. Rebuild old packages; incompatible installed data
is retained. Historical validation does not validate current firmware. Host tests
do not measure current, DMA, physical stack, Flash power-loss durability or USB/BLE
latency. `device.future.toml` is inactive and its wiring is not hardware acceptance.
