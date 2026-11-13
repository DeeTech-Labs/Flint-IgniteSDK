# Документация FlinT Ecosystem

Русский — канонический язык. [English](../en/INDEX.md) · [IgniteSDK](../../README.md).

[Текст и вёрстка](UI-LAYOUT.md): добровольные текстовые блоки и ответственность автора за расположение элементов.

Этот индекс принадлежит SDK: общие API, permissions и форматы описываются здесь;
прошивка и desktop UI — в своих проектах. Межрепозиторные ссылки проверяются по
локальным checkout; это не подтверждение публикации этих страниц на GitHub.

## Выберите маршрут

| Аудитория | Начать | Затем | Проверить результат |
| --- | --- | --- | --- |
| Пользователь устройства | [Настройка и ежедневные операции](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/ru/USER-GUIDE.md) | [Recovery OS](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/ru/getting-started/device-use.md) | Устройство совместимо, пакет установлен, состояние прав подтверждено |
| Разработчик Ignite | [Первое приложение](APP-QUICKSTART.md) | [Lua API](LUA-API.md), [датчики](SENSORS.md), [службы](BACKGROUND-SERVICES.md) | Validator и сценарий завершаются с кодом 0 |
| Разработчик OS/модулей | [Сборка OS](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/ru/getting-started/building.md) | [Добавление модуля](https://github.com/DeeTech-Labs/FlinT-OS/blob/main/docs/ru/development/extending.md), [границы SDK](PLATFORM-GUIDE.md) | Host/tooling tests и ESP32-S3 build; физические проверки отдельно |
| Разработчик Flare | [Сборка](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/ru/BUILD.md) | [Добавление функции](https://github.com/DeeTech-Labs/Flint-Flare/blob/main/docs/ru/DEVELOPMENT.md) | CTest, QML smoke и нужные session tests |
| Контрибьютор | [CONTRIBUTING](../../CONTRIBUTING.md) | Выберите проект и минимальный набор проверок | Diff, changelog, RU/EN, воспроизведение и честный отчёт |

## Учебные маршруты

[Первое приложение](APP-QUICKSTART.md) ведёт от инструментов до пакета, simulator и устройства.
Пользовательские действия не требуют чтения C++ или бинарного протокола.

## Решение задач

- [Ресурсы и иконки](FLIM.md): подготовка PNG/SVG/шрифтов на компьютере.
- [Датчики](SENSORS.md): subscriptions, freshness, диагностика и отсутствие оборудования.
- [UI + service](BACKGROUND-SERVICES.md): отдельные VM, IPC, сохранение и autostart.
- [PC Monitor](PC-MONITOR.md): отдельная политика данных компьютера.

## Справочники и объяснения

- [Lua API](LUA-API.md), [сгенерированный registry](REGISTRY.md): наличие имени не означает backend.
- [SDK и инструменты](SDK.md), [архитектура и форматы](ARCHITECTURE.md), [Companion Demand](COMPANION.md).
- [Как расширять платформу](PLATFORM-GUIDE.md): owning files, lifecycle, доверие и проверки.
- [Лицензии](LICENSING.md), [измерения FLIM](FLIM-BENCHMARK.md): результаты привязаны к описанной версии.

## Уровни подтверждения

Текущий контракт — `0x20261115`, manifest FLM3, FLIM v2. Старые пакеты нужно пересобрать;
nominal API/package/Companion version 1 не гарантирует совместимость. Установленные
несовместимые данные не стираются автоматически. Исторические validation reports
не подтверждают новую прошивку. Host tests не измеряют ток, DMA, аппаратный stack,
Flash power-loss durability или задержки USB/BLE. `device.future.toml` неактивен;
его GPIO и необязательные датчики не являются готовой проверенной конфигурацией.
