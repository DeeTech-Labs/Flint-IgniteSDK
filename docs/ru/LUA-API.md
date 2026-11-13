# Lua API: работающие вызовы

[Индекс](README.md) · [English](../en/LUA-API.md) · [Первое приложение](APP-QUICKSTART.md)

Источник поведения — `src/lua_app.cpp`, `src/runtime.cpp`, `src/service_runtime.cpp`
и host bridge Core. Это справочник реализованных вызовов, а не всех имён registry.
Включите `app_api:1` в выбранном component. Для sensors нужен `sensor_api:1`,
для IPC/state — `ipc_api:1`; каждый component объявляет собственные permissions.
Capability API, сконфигурированное оборудование и готовность источника независимы.

## Правила вызова и ошибки

Коды ошибок — integer: 0 означает успех; см. [сгенерированную таблицу](REGISTRY.md).
Неверное количество/тип аргументов, недопустимый timer или превышение sandbox limits
могут завершить callback ошибкой Lua, а не вернуть обычный код. Обрабатывайте
возвращаемый error; `nil` не является показанием нуля. У сервисов нет permission dialog.
Декларация manifest не выдаёт grant. Grants проверяет Core по установленному package,
component и instance; приложение не передаёт доверенную identity.

Sensor operations и state/IPC во время загрузки возвращают `invalid_state`.
Service host operations также заблокированы при load/pause/stop. Foreground может
настроить `schedule` при load, но не должен делать там внешние действия. Для переносимого
кода выполняйте операции в активных callbacks; в service — после `event('start',0)`.

## Обработчики

Lua chunk возвращает table обработчиков. UI обязан содержать `update(dt_ms, pressed)` (пустая функция допустима при schedule0);
`event(kind, value, x, y, pointer, at)` необязателен. `event` получает focus/input/lifecycle события;
`timer` приходит как `event('timer', id)`. При App API focused input поступает
в event, а не как pressed в update. Последние четыре аргумента — скаляры координат/pointer/monotonic timestamp, а не input table. Touch cancellation описана в
[SDK](SDK.md). Не храните handles после suspend/stop/update/перезапуска.

Service: `event('start',0)`, `event('pause',reason)`, `event('resume',0)`,
`event('stop',reason)`, timer, `sensor`, `message`, `state`, `persisted`, `policy`.
`update` появляется только при включённом schedule. [Причины и lifecycle](BACKGROUND-SERVICES.md).
Pause отменяет подписки и timers; resume callback восстанавливает нужный спрос.
Пропущенные периоды не проигрываются неограниченной очередью.

Все callbacks одного tick делят instruction/native budget; это не отдельная квота
для каждого sensor/timer. Input и touch_cancel доставляются раньше sensor events.
Service не получает Scene, focus и UI API. Lua sandbox не открывает стандартные
библиотеки, `require`, `load`, OS/filesystem/network/debug или native pointers.

## Время и чтение системных значений

`ignite.read(name[, key]) -> value, error`. Значение — string/integer либо nil.

| name | Permission | Значение и контекст |
| --- | --- | --- |
| `time.read` | `time.read` | UTC epoch seconds, не clock для deadlines; UI/service |
| `time.monotonic` | `time.monotonic` | monotonic milliseconds; UI/service |
| `system.info` | `system.info` | профиль; UI |
| `system.power.status` | `system.power.status` | key `voltage_mv`, `age_ms`, `measured_at_ms`, `status` при наличии backend; UI |
| `system.locale.read` | `system.locale.read` | строка locale по key; UI |
| `system.theme.read` | `system.theme.read` | строка theme по key; UI |
| `system.settings.read` | `system.settings.read` | string/integer по key; utc_offset_minutes и ble_available — integer; UI |
| `packages.list` | `packages.list` | ограниченный список для UI; для выбора используйте catalog |

Не объявляйте charging/SOC, если backend не предоставляет их. Неизвестное имя —
`not_supported`; неизвестный ключ поддержанного сервиса может быть `source_unavailable`.
Service реализует только время из этой таблицы. Его `ignite.read` не даёт Companion access.

`ignite.schedule(interval_ms) -> error`: 0 выключает update, 1..86400000 задаёт интервал
UI; для service действует model minimum (по умолчанию 100 ms). `ignite.timer(id,
delay_ms, repeat_boolean) -> error`: id 1..8, delay 0 отменяет timer, иначе до 86400000
и не меньше model minimum. Нужен `time.timer.create`. Timer повторно с тем же ID заменяет
предыдущий timer этого instance.

## UI, ресурсы и навигация

| Вызов | Результат | Условия и пределы |
| --- | --- | --- |
| `ignite.value(key, value)` | нет | UI, до 16 ключей; integer i32 или UTF-8 string до 192 bytes; `${app.key}` |
| `ignite.set(node, property, integer)` | обычно нет; `image` возвращает error | UI, node index/named `ui.id`; свойства `x/y/w/h/color/visible/image/catalog`; фон первого узла защищён |
| `ignite.catalog(index)` | name, target, error | UI, `packages.list`, bounded catalog; index начинается с 0 |
| `ignite.action(key, value)` | error | UI, `ui.navigation`; переходы требуют текущего focused пользовательского ввода |
| `ignite.log(text)` | error | UI, UTF-8 до 128 bytes, первые 8 сообщений на запуск |

`image` принимает ID собственного подготовленного FLIM-ресурса, нужен `ui.resources`.
Это не загрузка PNG/URL на устройстве. [Ресурсы, имена узлов и цвет](FLIM.md),
[навигация и input contract](SDK.md). Привилегированные factory actions проверяют
фактическое factory происхождение, не строку package ID. `ignite.services` предназначен
только доверенному factory management, не сторонним приложениям.

## Хранилище

UI: `ignite.storage_get(key) -> string,error` либо `ignite.storage_get(key, default_i32) -> integer,error`, `storage_set(key,value) -> error`,
`storage_delete(key) -> error`. Нужен `storage.private`. Default только i32; записываемый value — i32 либо
UTF-8 string до 192 bytes. Key — 1..32 `[a-z0-9_]`, не путь. До 16 keys/4096-byte файла,
успешные записи не чаще одной в секунду на Core. Это синхронное приватное UI-хранилище;
не используйте его на каждый кадр. CRC не является шифрованием/подписью.
Обновление/rollback того же ID/publisher сохраняет данные; uninstall удаляет app kv.

Service не имеет этих синхронных функций. UI/service совместно используют state API:

| Вызов | Результат | Ограничения |
| --- | --- | --- |
| `ignite.ipc.send(component, topic, value)` | error | `app.ipc`; тот же package generation; component <=64, topic <=32 bytes |
| `ignite.state.read(key)` | value, revision, error | `storage.private`; key <=32, до 16 keys |
| `ignite.state.commit(expected_revision, writes)` | revision, error | атомарный RAM batch 1..8 keys, конфликт — `conflict` (16) |
| `ignite.state.flush()` | ticket, error | bounded async persistence; успех запроса не означает durable сохранение |

Значения IPC/state: boolean, int64, finite double, UTF-8 string <=96 bytes. Нет table/
function/reference между VM. `message(sender_component,topic,value)`, `state(revision)`,
`persisted(ticket,error)`, `policy(value)` доставляются с live authorization. Inbox 8 events;
messages при переполнении отклоняются, state/policy coalesce. Global flush не чаще
model `minFlushMs` (60000 ms по умолчанию); installation имеет приоритет. Отмена instance
удаляет pending flush/results; shared RAM state принадлежит пакету, а не одной VM.
[Persistence и восстановление](BACKGROUND-SERVICES.md).

## Датчики и outputs

| Вызов | Результат | Условия |
| --- | --- | --- |
| `ignite.sensors.catalog(index)` | metadata, error | index 1..16; не запускает сбор |
| `ignite.sensors.read(source, mask, last_known_boolean)` | snapshot, error | все 3 аргумента обязательны, cache only |
| `ignite.sensors.refresh(source, mask, timeout_ms)` | handle, error | async, timeout 1..60000 |
| `ignite.sensors.subscribe(source, mask, interval_ms)` | handle, error | async, source/model bounds |
| `ignite.sensors.unsubscribe(handle)` | error | только собственный handle; отменяет refresh или subscription |
| `ignite.beep(hz, duration_ms)` | error | `audio.play`; service также `audio.background`; нужен buzzer backend |
| `ignite.brightness(percent, duration_ms)` | error | только UI, live session grant `ui.brightness`, временный override |

Sensor callback: `sensor(handle,snapshot,error,drops,refresh_finished)`. Source 0 выбирает
основной источник детерминированно. Mask, SI units, source/sample identity, error/status,
timestamp/age, expiry и motion axes определены в [Sensor reference](SENSORS.md).
До 8 subscriptions и 8 refresh/instance, общий hard максимум 40 каждого. Нельзя
отменять чужой handle. Отзыв прекращает будущую доставку и demand, но уже переданные
приложению значения невозможно удалить из его памяти задним числом.

Beep не является playback/haptics: OS ограничивает 100..10000 Hz, непрерывный burst
5 seconds даже с retunes, интервалы команд и duty cycle. Brightness 5..100%,
1..5000 ms; после override восстанавливается системная политика. Ограничения backend
могут быть строже. Нет оборудования — `not_supported`, не виртуальный результат.

## Рабочие примеры

Полные manifest и Lua, собираемые общим compiler: [counter](../../examples/counter/project.json),
[sensors](../../examples/sensors/project.json), [UI + service](../../examples/thermometer-service/project.json).
Не переносите один Lua-фрагмент без component capabilities/permissions. Quickstart
показывает build/check/run, trusted scenario grants и установку через Flare.
