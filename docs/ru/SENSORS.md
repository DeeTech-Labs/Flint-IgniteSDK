[English](../en/SENSORS.md)

# Датчики и системная периферия

Контракт разработки `0x20261115`; номинальные версии package/API/Companion остаются 1: framing, header и index контейнера не менялись, manifest теперь FLM3 с per-component logic/budgets/declarations. Изменились permissions и появились новые методы. Старые пакеты/peers отклоняются по ContractRevision. Пересоберите все свои пакеты. Не удаляйте partitions, settings, private storage или grants для обновления. Существующий grants.bin сохраняет формат; ambient_light больше не automatic и требует явного persistent grant. Publisher — metadata, а не доказательство подписи.

Канонический каталог логических каналов: `spec/sensors.json`; генератор выпускает `flint/sensor_registry.h`. `temperature` — Celsius, `pressure` — Pa, `humidity` — %, `ambient_light` — lux, `acceleration` — m/s², `angular_velocity` — rad/s. Имена микросхем, I2C и GPIO не являются API приложений. Source.id стабилен в пределах модели. Автовыбор source=0: наименьший priority, затем source_id; неисправный основной источник не вызывает скрытого переключения на другой. Маска нескольких каналов выбирает один источник, поддерживающий всю маску.

Core владеет единственным Broker/cache и authorizer. Backend обменивается bounded desired demand и coherent samples; его методы не выполняют I2C, ADC, Lua, Flash и не ждут конверсии. OS worker владеет аппаратным расписанием. Worker transport buffers не публичный cache. Backend должен переживать Core, будить owner loop при результате и уважать Demand.notBefore, physical min interval и shutdown после последнего потребителя. Один physical sample обслуживает все подписки; медленные подписки и delivery coalesce. Периоды и policy задаёт OS, не приложение. Foreground и [фоновые службы](BACKGROUND-SERVICES.md) имеют отдельные owners; shared Broker объединяет demand и не дублирует физическую выборку.

## Lua

В `project.json`, внутри нужного `components[]`: `"app_api":1, "sensor_api":1`, Lua capability64 и memory/instruction budgets по обычным правилам. sensor_api добавляет capability8192; cap означает поддержку API, не присутствие оборудования. Во время загрузки/валидации все sensor operations возвращают invalid_state. Вызывайте их из активных update/input/sensor callbacks.

```lua
local handle = nil
ignite.schedule(1000)
return {
  update=function(dt)
    if dt>0 and not handle then
      local e
      handle,e=ignite.sensors.subscribe(0,1,5000) -- temperature mask=1
      ignite.value('error',e)
    end
  end,
  sensor=function(id,s,error,drops,finished)
    if error==0 then ignite.value('temperature',''..s.values.temperature.x) end
  end
}
```

- `catalog(index)` — 1..16, `(metadata,error)`; конец/нет источника — not_supported. Metadata не включает readings и не требует grants.
- `read(source_id, channel_mask, last_known)` — `(snapshot,error)`, только cache, без I/O. 0 source выбирает основной.
- `refresh(source_id,mask,timeout_ms)` — `(handle,error)`, async одна новая конверсия, timeout1..60000. Уже находившийся в transport sample не завершает новый refresh.
- `subscribe(source_id,mask,interval_ms)` — `(handle,error)`, интервал в пределах Source. До 8 subscriptions и 8 refresh на owner; до 40 каждого вида суммарно для foreground и четырёх служб.
- `unsubscribe(handle)` — error; отменяет subscription или pending refresh. После смены runtime/consumer generation старые handles дают stale_handle.
- Callback `sensor(handle,snapshot,error,drops,refresh_finished)` реально доставляется после input и до timer/update в tick. Все foreground callbacks tick делят instruction budget и32 native API calls; службы используют общий model pool и собственный dispatch allowance. Медленный callback получает последний sample, не неограниченную очередь.

Mask bits0..5 соответствуют порядку каналов выше. Snapshot: source_id,generation,sample_id,timestamp_ms,age_ms,status,reason,channels,simulation,last_known,values. Scalars используют x, motion — x/y/z; каждый канал содержит unit. Timestamp — monotonic completion/read observation, не гарантированное время физической выборки. Чтение cache не меняет его. Status0 absent,1 pending,2 valid,3 stale,4 failed,5 initializing. Saturation/невалидные данные: failed с invalid_argument. Нули не заменяют отсутствие значения. При pending/error/stale обычное чтение не содержит values. last_known=true может вернуть values вместе с исходным unavailable error/status/age; проверяйте last_known. Reset/recovery source generation инвалидирует старое last-known.

Доступ проверяется при read, создании refresh/subscription и delivery, через общий authorize по установленному principal. Persistent permissions: sensors.motion, sensors.ambient_light, sensors.temperature, sensors.pressure, sensors.humidity. Declaration не grant. Маска BME280 фильтрует данные и не разрешает соседние каналы. Отзыв удаляет demand/неотправленные callbacks; уже доставленные значения отозвать невозможно. Suspend/stop/crash/update/uninstall отменяют demand. Ошибка durable revoke возвращает io_error и блокирует текущий доступ в RAM; после reboot старый durable grant может вернуться, поэтому UI не подтверждает сохранение при ошибке.

Нет сконфигурированного источника — not_supported. Сконфигурирован, но не работает/ещё нет данных — source_unavailable плюс status. Поддерживаемая операция без declaration/grant — permission_denied. Наличие Source в catalog не означает готовность. device.future.toml и комментарии не включают модули.

## Системные функции

`ignite.beep(frequency_hz,duration_ms)` требует audio.play; это только bounded buzzer, не audio playback/haptics. `ignite.brightness(percent,duration_ms)` требует текущего session ui.brightness grant и даёт override5..100% на1..5000ms. Неподдерживаемый output — not_supported. В inactive loading outputs запрещены. OS ограничивает beep100..10000Hz, до5s непрерывно даже при retune,100ms между командами, quiet не короче предыдущего burst; mute останавливает звук.

Authenticated settings_get/set: brightness_mode=manual/automatic, brightness_manual5..100, brightness_fallback5..100, audio_muted0/1. Read-only brightness_supported/mute_supported отражают backend. Автояркость создаёт system demand ALS1Hz, EMA1/4 на новом sample, hysteresis3п.п., максимум5п.п./s. Ошибка ALS ведёт к fallback50% по умолчанию. Override прекращается при expiry, смене consumer, suspend или revoke, затем возвращается системная политика. Это начальная policy без измеренной фотометрической калибровки.

RTC использует валидный UTC cache и monotonic extrapolation. time_set сначала durably обновляет Core epoch fallback, затем запрашивает RTC write/readback. Success time_set — Core принял время, не подтверждение RTC. time_status(epoch:u64,state:u8,error:u16) сообщает0 no RTC/Core-only,1 pending,2 RTC confirmed,3 partial RTC failure. Пока pending следующий time_set запрещён. Deadline5s; частичный отказ не откатывает уже принятое Core время. При настроенном RTC периодические epoch Flash checkpoints отключены; trusted time_set пишет fallback один раз. Sensor deadlines/grant expiry всегда monotonic.

## Companion wire и диагностика

Общий framing v1/LE/CRC32; сначала обычный Error:u16. sensor_catalog(27) empty request возвращает count:u8, затем source_id:u16,mask:u8,min/default/max/stale:u32,priority/simulation/temperature_kind/coordinates:u8,status:u8,reason:u16,generation/sample_id/at/age:u64. Значений в catalog нет.

sensor_diagnostic(28): action:u8=0 stop;1 start с source:u16,mask:u8,interval_ms:u32,lease_ms:u32;2 read. Одна явная session lease до30000ms; аппаратная аутентификация сама её не включает. Interval>=200ms и source.minInterval, максимум5Hz. Lease истекает monotonic, disconnect/закрытие Flare panel прекращают demand; reconnect не возобновляет её. Start/stop replies empty. Read: source:u16,channels:u8,generation/sample_id/at/age:u64,status:u8,reason:u16,drops:u32,simulation:u8; по каждому установленному bit три IEEE754 f64 LE. channels0 при отсутствии свежего sample, без doubles. Чтение чаще interval — quota_exceeded. Это ограниченный pull с coalescing/backpressure, не raw ODR stream по BLE. Runtime logs через diagnostics доступны только владельцу активной diagnostic lease; обычные counters не содержат sensor readings.

## Проверка и дальнейшая работа

Host tests sensor_broker и sensor_core используют Simulation и управляемое monotonic время. Simulation явно помечает данные, поддерживает отсутствие/pending/stale/failed/initializing, reset, finite values и counters. Это не проверка реального I2C, jitter или питания. Remote Preview продолжает исполнение устройства; вторую Lua VM для remote данных не создаёт.

Валидация на железе отдельная: actual internal/PSRAM и worker stack watermark; wakes/I2C/ADC counts и durations; sample gaps,jitter,missed deadlines,age,drops; touch/render/USB/BLE/install под нагрузкой Flash; питание idle/display-on/sampling; отключение ALS и обязательная попытка shutdown; RTC oscillator-stop/partial writes/restart; invalid calibration/saturation/timeouts; power hold/BOOT/reset/recovery. FPS, hard realtime и автономность не гарантированы.

Backlog: VEML autorange/коррекция; IMU mounting/calibration/FIFO; проверенный wake-on-motion/raise gesture; local history с отдельной access policy/Flash limits; RTC alarms/low-power wake; обоснованная battery charge model. Наличие IMU не означает шагомер или медицинские алгоритмы.

Motion catalog содержит acceleration_range_m_s2,angular_velocity_range_rad_s,mounting_verified. OS QMI8658: ±19.6133m/s²,±8.936rad/s; фиксированные sensor-aligned device axes, identity mounting. Совпадение осей с геометрией корпуса ещё не проверено: mounting_verified=false. UI rotation оси не меняет. Не называйте unverified mount откалиброванным.

Несовместимые установленные objects и refs сохраняются при старте: смена ContractRevision не считается повреждением storage и не запускает автоматический rollback/удаление объекта. Явный launch возвращает incompatible_contract; список исполнимых пакетов скрывает несовместимые записи до пересборки/переустановки. Private state и durable grants не удаляются.

[Фоновые службы](BACKGROUND-SERVICES.md) используют тот же Broker/cache с отдельными instance-bound handles. Ошибка или смена UI не отменяет demand живой службы. Package permissions union не заменяет component declaration.
