# Первое Ignite-приложение

[Индекс](README.md) · [English](../en/APP-QUICKSTART.md) · [Lua API](LUA-API.md)

## 1. Подготовьте окружение

Нужны Git, CMake 3.21+, C/C++20 toolchain, Python 3.11+ и Pillow. Lua 5.4.9 уже
включена; системный Lua не нужен. На Windows откройте Developer PowerShell для
Visual Studio либо окружение выбранного MinGW kit. Не смешивайте компиляторы в
одном build. Ниже PowerShell, команды выполняются из корня **IgniteSDK**:

```powershell
python -m venv build/docs-venv
build/docs-venv/Scripts/python -m pip install Pillow
cmake -S . -B build/docs-sdk -DPython3_EXECUTABLE="$PWD/build/docs-venv/Scripts/python.exe"
cmake --build build/docs-sdk --config Debug --parallel 2
ctest --test-dir build/docs-sdk -C Debug --output-on-failure
```

Ожидается `100% tests passed`. Для Linux/macOS используйте `python3`, interpreter
`build/docs-venv/bin/python` и добавьте `-DCMAKE_BUILD_TYPE=Debug`; executables лежат
в `build/docs-sdk/`, а не в `Debug/`. Это переносимые команды, не утверждение о
запуске всех ОС. Pillow требуется и при configure для генерации factory assets.

На Windows Ninja/MinGW замените пути `build/docs-sdk/Debug/ignite-check.exe` и `ignite-run.exe` на `build/docs-sdk/ignite-check.exe` и `build/docs-sdk/ignite-run.exe`: подкаталог Debug относится к Visual Studio.

Для временных CLI-файлов создайте temp/; в PowerShell задайте `$env:TEMP="$PWD/temp"` и `$env:TMP=$env:TEMP`, на Linux/macOS — `mkdir -p temp` и `export TMPDIR="$PWD/temp"`.

## 2. Создайте и проверьте пакет

`init` создаёт декларативный watchface без Lua. Каталог назначения должен отсутствовать.
Для запуска именно Lua начните с сопровождаемого примера `examples/counter`.

```powershell
build/docs-venv/Scripts/python tools/ignite.py init build/my-clock --id demo.clock --publisher demo
build/docs-venv/Scripts/python tools/ignite.py build build/my-clock/project.json build/my-clock.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py check build/my-clock.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py build examples/counter/project.json build/counter.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py run build/counter.ignit --scenario examples/counter/scenario.json --runner build/docs-sdk/Debug/ignite-run.exe --image build/counter.ppm
```

Успех: JSON validator содержит `runtime_compatible:true`, runner возвращает код 0
и создаёт PPM. Это проверка Runtime, а не измерение вместимости платы или подписи
publisher. `run` без окна использует виртуальное monotonic время. Для интерактивного
Preview соберите Flare, включите Simulator и откройте Ignite Studio; реальные USB/BLE
здесь не проверяются. Не запускайте декларативный clock с искусственным `press`:
он не объявляет `input.focused`; используйте counter для ввода.

## 3. Поймите project.json

[counter/project.json](../../examples/counter/project.json) — полный исполняемый
образец. Корень содержит `id/name/publisher/version/components`, а каждый component —
собственные `type`, `budget`, `capabilities`, `permissions`, `logic` и UI при наличии.
`app_api:1`, `sensor_api:1`, `ipc_api:1` также задаются в component. Старые root-поля
отклоняются. `logic.path` и до восьми `logic.includes` читаются compiler относительно
корня проекта; итоговый Lua-текст ограничен 16 КиБ. На устройстве нет `require`.

`permissions` запрашивает доступ; grant выдаёт доверенный Core по явному действию
пользователя. Publisher — metadata, не криптографическое доказательство личности.
48..144 КиБ Lua budget включает native/loader allowances: это не размер доступного
Lua heap. Не повышайте бюджет автоматически при OOM.

## 4. Добавьте датчик и службу

Сначала соберите [sensors](../../examples/sensors/project.json) тем же `build`/`check`.
Для temperature нужны `sensor_api:1`, `app_api:1`, Lua capability 64 и component
declaration `sensors.temperature`; затем отдельный persistent grant в Ignite Hub.
Source 0 означает детерминированный выбор источника для запрошенных каналов. Все
операции читают кеш или ставят запрос; Lua не ждёт I2C/conversion. Не считайте `nil`
или ошибку измерением нуля. [Единицы, callbacks и freshness](SENSORS.md).

Для пары UI + background используйте полный
[thermometer-service](../../examples/thermometer-service/project.json). Он содержит
`main` и `worker`, отдельные VM и общий статически включённый `shared.lua`.

```powershell
build/docs-venv/Scripts/python tools/ignite.py build examples/thermometer-service/project.json build/thermometer.ignit --validator build/docs-sdk/Debug/ignite-check.exe
build/docs-venv/Scripts/python tools/ignite.py run build/thermometer.ignit --scenario examples/thermometer-service/scenario.json --runner build/docs-sdk/Debug/ignite-run.exe --factory build/docs-sdk/generated/factory --image build/thermometer.ppm
```

Сценарий — доверенный test driver: его `permission` шаги **не** API приложения и не
автоматическая выдача прав на устройстве. Он проверяет запуск, сохранение UI-independent
службы, sensor revoke и stop. В Flare Simulator выдайте execution/temperature/humidity
вручную, затем запустите `worker` в Diagnostics. UI показывает последние известные
значения; после закрытия UI служба продолжает сбор. Stop и revoke доступны в Services.
Autostart требует отдельного grant **и** включения preference; install/UI launch его
не включают. История — шесть пар в RAM, flush не чаще минуты. [Полный контракт](BACKGROUND-SERVICES.md).

## 5. Установите на устройство

Подключитесь через Flare, дождитесь совместимого handshake. В Ignite Hub выберите
скомпилированный `.ignit`, дождитесь завершения установки, затем выдайте только нужные
права. Запустите UI; службу запускайте отдельно. На активном Spark без датчиков
пример обязан показывать `not supported`: запрос не включает закомментированные
модули и не меняет TOML. [Пошаговые пользовательские действия](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/ru/USER-GUIDE.md).

Если validator сообщает `incompatible_contract`, пересоберите пакет SDK той же
ревизии, что firmware/factory. При `permission_denied` проверьте declaration нужного
component и подтверждённый grant; при `resource_exhausted` — бюджет и admission.
Не стирайте пользовательские данные для обхода этих ошибок.

Используйте исходники и compiler одной согласованной ревизии SDK. Для примера UI+service пересобирайте пакет из текущего project.json: старый бинарный snapshot не заменяет исправленные исходники.
