# Фоновые службы и изоляция компонентов

[English](../en/BACKGROUND-SERVICES.md) · [SDK](SDK.md) · [Датчики](SENSORS.md)

## Контракт и миграция

Текущая ревизия разработки — `0x20261115`. Manifest имеет magic `FLM3` (третья версия manifest); номинальные версии OS API, Face SDK, Ignite package envelope и Companion остаются 1. Заголовок контейнера, индекс и framing Companion не меняются. Проверяется точное совпадение ContractRevision: старые пакеты и peers отклоняются. Пересоберите все собственные factory/examples/apps и обновите SDK consumers совместно. Несовместимость не является повреждением: установленные objects/references, private storage и grants не удаляются автоматически. Старые grants/autostart не расширяются на новую package generation; после обновления требуется соответствующее согласие.

В `project.json` поля `logic`, `budget`, `permissions`, `capabilities`, `app_api`, `sensor_api`, `ipc_api` находятся в каждом элементе `components`, а не в корне. Каждое исполняемое component имеет явный `logicSection` в manifest. Корень хранит identity/version, components, resources и icons. Package union permissions служит перечислению согласий; runtime проверяет декларацию именно вызывающего component. Package grant возможен, но не заменяет отсутствующую component declaration. Неявного наследования и fallback к старому manifest нет.

Один package может содержать UI и не более одной службы `type: "service"`. Пример: [thermometer-service](../../examples/thermometer-service/project.json). Capability `16384` обозначает background execution, `32768` — component IPC; compiler добавляет их из service/`ipc_api: 1`. `sensor_api: 1` добавляет `8192` и требует Application API/Lua.

## Владение и lifecycle

Единственный Core исполняет foreground Runtime и отдельные `ServiceRuntime` в том же owner loop. У службы своя Lua VM, validated package identity, component budget и Core-issued instance identity. Файл package закрывается после проверки/чтения исходника до запуска VM; loader peak учитывается при admission. Нет второй задачи FreeRTOS, второго Core, Scene/framebuffer, focused input или rendering API службы. Lua не выбирает principal/instance и не получает указатель на чужую VM.

Служба не получает `ignite.set/value/log`, UI navigation, synchronous private storage, brightness или `ignite.services`. Доступны разрешённые чтения времени, schedule/timer, sensors, IPC/shared state. Buzzer требует `audio.play` и `audio.background`, проверяемые Core; это не фоновое воспроизведение аудиофайлов. Неподдерживаемые операции возвращают `not_supported`. Во время загрузки privileged calls запрещены.

После admission первая активная диспетчеризация вызывает `event("start", 0)`. Далее возможны `event("pause", reason)`, `event("resume", 0)`, `event("stop", reason)`, timer events и sensor/IPC callbacks. При pause/stop новые операции уже запрещены; даже ошибка lifecycle callback освобождает handles/resources и переводит службу в faulted. Resume не воспроизводит накопленные timer ticks: callback должен восстановить нужные timers/subscriptions. Сбой одной службы не останавливает foreground или соседнюю службу.

Закрытие UI и переключение на другой package не останавливают службу. Pause/stop/revoke, update и uninstall отменяют только ресурсы соответствующего instance/package. Старые handles и сообщения не переходят новому instance. Execute revoke действует и на paused instance. Ошибка durable revoke возвращает `io_error`, оставляя доступ запрещённым в RAM; UI не должен подтверждать сохранение. После reboot старый durable grant может вернуться, если запись не состоялась.

`app.background.execute` — persistent consent запуска. `app.background.autostart` — отдельное persistent consent автозапуска. Нужны декларация, действующие grants и явная generation-bound preference; одна декларация или installation ничего не запускает. Crash снимает autostart preference, служба не попадает в автоматический restart loop. Low-energy policy приостанавливает службы; после восстановления возобновляются только подходящие instances с действующим execute grant.

## Общий ограниченный pool служб

Model/host `Platform.background` задаёт maximum count, RAM pool, instruction/native/wall budgets, quantum и минимальные интервалы. SDK hard maximum — 4 службы; model может разрешить меньше, включая 0. Pool учитывает infrastructure, reservations VM, shared state и pending flush; admission также проверяет доступную память. Отказ `resource_exhausted` не уничтожает уже работающий UI или другую службу. Резерв foreground сохраняется отдельно.

Все службы делят **фиксированный** instruction/native/wall pool на monotonic window. Добавление VM не увеличивает этот pool. Ready services обходятся по кругу, не более одного callback batch за Core pass. Quantum резервируется до извлечения событий; неиспользованная часть возвращается. Недостаток общего budget откладывает исполнение до replenishment, не создаёт busy loop и не считается fault службы. Все callbacks одного dispatch делят allowance.

Foreground исполняется раньше служб и сохраняет отдельный существующий instruction/native budget; общего единого foreground+service wall pool нет. Собственное превышение service allowance, deadline или native-call limit завершает виновный instance. Wall clock проверяется в Lua hook и вокруг native calls. Заблокировавшийся C callback невозможно прервать этим механизмом: его превышение обнаруживается после возврата. Parser, allocator, GC, VFS/Flash и реальные задержки требуют отдельного измерения. Quotas не являются hard realtime или гарантией автономности.

## Типизированные IPC и shared state

Обмен разрешён только между живыми components одного установленного package generation. Target component — identifier до 64 ASCII bytes; topic/state key — до 32. Значения: boolean, signed integer, finite number, UTF-8 text до 96 bytes. Lua tables/functions/userdata, pointers и arbitrary actor identity не передаются между VM.

```lua
ignite.ipc.send('worker', 'refresh', true) -- error
local value, revision, error = ignite.state.read('temperature')
local next_revision, error = ignite.state.commit(revision, {status=0})
local ticket, error = ignite.state.flush()
```

`message(sender_component, topic, value)`, `state(revision)`, `persisted(ticket, error)` и `policy(value)` вызываются защищённо и расходуют тот же dispatch budget. Для messages требуется `app.ipc`, для state — `storage.private`. Inbox ограничен 8 events; overflow даёт `quota_exceeded`, state/policy notifications coalesce. Metadata управления содержит rejected/coalesced counters. Stop/restart отменяет сообщения старого sender.

Shared state содержит до 16 keys. Commit принимает 1..8 изменений: проверяет весь batch, capacity и ожидаемую revision до любой модификации, затем применяет всё атомарно в RAM. Конфликт revision — `conflict` (16); недопустимый batch не применяется частично. Чтение/commit/messages не пишут Flash.

`flush()` ставит bounded asynchronous snapshot в очередь и возвращает ticket; это не подтверждение записи. Итог приходит в `persisted`. Один global flush обслуживается за раз; между попытками — минимум model `minFlushMs`, по умолчанию 60000 ms, для всех packages вместе. Installation имеет приоритет перед flush. Состояние сохраняется атомарно с integrity check; это не доказательство устойчивости при отключении питания на конкретной Flash.

Пример thermometer-service хранит latest temperature/humidity/status и ring из шести пар readings: 12 history keys плюс latest/status/history_next дают максимум 16 keys. Каждый sample обновляет шесть keys одним atomic commit. RAM history ограничена; flush запрашивается не чаще 60 секунд. Нет записи на каждый sample и нет неограниченного журнала.

## Управление и проверка

Authenticated Companion `services` (30) возвращает paged metadata/policy, без raw readings. `service_control` (31) предоставляет explicit start/stop/pause/resume/autostart/revoke. Factory management использует `ignite.services.list(index)` и `control(action, package, component)`; list использует index 0..255, actions 0=start, 1=stop, 2=pause, 3=resume, 4=autostart on, 5=off, 6=revoke execute. Control разрешён только из trusted focused input с `system.diagnostics`. Обычный package не получает эту привилегию из имени.

### Companion wire 30/31

Framing v1, little endian; каждый reply начинается с `Error:u16`. Строки — UTF-8 с длиной `u16`, до 64 bytes.

`services` (30) request: `offset:u16`, `limit:u8` (1..8); `offset + limit` не больше 65536. Reply после Error: `count:u8`, `more:u8`, `maxServices:u8`, `memoryBytes:u32`, `instructionsPerWindow:u32`, `nativeCallsPerWindow:u32`, `windowMs:u32`, `lowEnergy:u8`. Далее `count` rows:

- package/component/label: три text64;
- state/reason: два u8; error:u16;
- instance_id/generation: два u64;
- used/peak/reserved: три u32;
- callbacks/instructions/native_calls/max_callback_us/wakeups/event_rejected/event_coalesced: семь u64;
- execute_granted/autostart_granted/autostart: три boolean u8 (0 или 1).

`service_control` (31) request: `action:u8`, package:text64, component:text64. Actions 0..6 соответствуют start/stop/pause/resume/autostart on/off/revoke execute. Успешный ack после Error пуст. Ответ не содержит raw readings или Lua state; polling metadata не запускает службу.

Host integration tests проверяют isolation, ownership, CAS, budgets/fairness, pause faults, revoke, autostart, lifecycle, async flush и update/uninstall. Проверка двух служб в pool 131072 на host относится к admission/accounting этого профиля. Фактические ESP32 RAM/PSRAM, stack watermark, latency, wakeups, Flash durability и питание остаются аппаратной проверкой.

Host MinGW 15.1 x64 (`sizeof`, байты): Core 30776, Runtime 10984, ServiceRuntime 1720, Broker 14200, Simulation 4176, IpcRequest 1592, IpcEvent 280; native Lua state wrapper — 4384. Это размеры host ABI, не измерение ESP32 heap/stack. До multi-owner Broker занимал 9280 байт в том же host ABI; увеличение — 4920 байт.

При сбое записи crash preference latch остаётся запрещённым в RAM; старое durable autostart preference может сохраниться после reboot. Автоматических повторов в текущем запуске Core нет.
