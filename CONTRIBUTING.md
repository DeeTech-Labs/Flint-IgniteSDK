# Вклад в IgniteSDK

[English](docs/en/CONTRIBUTING.md) · [Первое приложение](docs/ru/APP-QUICKSTART.md) · [Индекс](docs/ru/README.md)

## Первый небольшой вклад

Для изменения текста/примера начните с соответствующей страницы и исходника, а не со
всей архитектуры. Для ошибки приложите минимальный project.json/Lua, SDK revision,
ОС/toolchain, команду, ожидаемый и фактический результат. Не прикладывайте grants,
личные данные, pairing keys, полные пользовательские storage или секреты.
Вопросы — [Discussions](https://github.com/DeeTech-Labs/Flint-IgniteSDK/discussions),
воспроизводимые дефекты — [Issues](https://github.com/DeeTech-Labs/Flint-IgniteSDK/issues).
Security-проблемы не публикуйте с exploit/private data: используйте контакт владельцев проекта.

## Окружение и базовые проверки

Git, CMake 3.21+, C/C++20, Python 3.11+ и Pillow в CMake interpreter **до configure**.
Lua включена в исходники. [Windows и single-config команды](docs/ru/APP-QUICKSTART.md).

```sh
python3 -m pip install Pillow
cmake -S . -B build/contrib -DCMAKE_BUILD_TYPE=Debug
cmake --build build/contrib --config Debug --parallel 2
ctest --test-dir build/contrib -C Debug --output-on-failure
python3 -B tools/check_docs.py
git diff --check
```

Для Visual Studio executable в `build/contrib/Debug/`; Ninja в `build/contrib/`.
Используйте один compiler kit/build directory. Все build/temporary артефакты только
в собственных `build/` и `temp/`; не используйте /tmp или корень для scratch-файлов.
Отдельно собираемый checkout submodule хранит их внутри своего `platform/build/`.

## Какие проверки выбрать

| Изменение | Дополнительно к diff/docs checks |
| --- | --- |
| Только документация | link/anchor check; запустить изменённые команды/сценарии, отметить недоступные ОС/плату |
| Manifest/compiler/registry | contracts, registry_validation, sdk_workflow, package_interop, generation_isolation; пересобрать все примеры |
| Lua/API/lifecycle/permissions | полный Lua CTest, sensor_core/background_core/service_isolation; no-Lua build |
| Storage/update | async_install/core_integration, отказ записи/disconnect/generation; power-cut отдельно |
| Rendering/resources | flim_decoder/resource_rendering/app_icons/touch_navigation/launcher_icons; captures и firmware build |
| Shared contract | OS build + tooling/host, Flare CTest и намеренный SDK pin; физические проверки отдельно |

Реальные имена tests: `ctest --test-dir build/contrib -C Debug -N`. Lua-disabled:

```sh
cmake -S . -B build/no-lua -DFLINT_ENABLE_LUA=OFF
cmake --build build/no-lua --config Debug --parallel 2
ctest --test-dir build/no-lua -C Debug --output-on-failure
```

`check_docs.py --examples --runner <ignite-run> --validator <ignite-check>` собирает
актуальные примеры и исполняет сопровождаемые сценарии в `temp/`. Для всех трёх
checkout используйте `--ecosystem <родительский каталог>`; это локальная проверка
межрепозиторных ссылок, не проверка публикации GitHub или доступности сторонних сайтов.

## Где менять код

`include/flint/` — contracts/interfaces; `src/` — переносимое исполнение; `spec/` —
канонические registries; `tools/` — compiler/generators; `system/` — factory source;
`examples/`, `tests/` — рабочие проекты и проверки. [Как добавить API/источник](docs/ru/PLATFORM-GUIDE.md).
Generation идёт в build-local paths через CMake, не меняет source snapshots автоматически.
Не редактируйте generated registry вручную и не обходите validation ради компиляции.

OS drivers/GPIO и Qt/QML принадлежат потребителям. Core владеет Lua, grants и Broker;
не добавляйте авторизацию OS или task на service. Нужны RAII, явные owner/generation/
cancellation, bounded queues/allocations, обработка недоверенных входов. Проверяйте
permission при операции и async delivery; запретите hardware effects при package validation.
Изменение ABI/формата не разрешает необъявленную потерю данных или ослабление recovery.

## Ветки, SDK и PR

Проверьте git status/submodule до действий; не откатывайте чужие изменения. Ветка
`docs/...`, `fix/...`, `feat/...`; commit по-английски Conventional Commits, например
`docs(sdk): explain service permissions`. Не делайте commit/push без согласованного запроса.
PR описывает проблему, итоговое поведение, migration/security impact, выполненные команды
и ограничения. Добавьте Unreleased entry в CHANGELOG для заметного изменения, RU/EN и
рабочие ссылки. Не меняйте licensing terms без разрешения; [области лицензий](docs/ru/LICENSING.md).

Общие изменения сначала owning SDK. После проверки и разрешённого commit потребители
закрепляют exact SHA; не выбирайте молча sibling checkout и не копируйте SDK sources.
Contract changes требуют согласованного compiler/Runtime/consumer обновления, а не adapters.

В отчёте разделяйте host tests, ESP32 build и физическое выполнение. Указывайте плату,
электрическую ревизию, firmware SHA и условия измерений. Если платы нет, так и напишите:
heap/PSRAM/DMA, stack, latency, BLE/USB, питание, Flash power-cut и sensor recovery не проверены.
