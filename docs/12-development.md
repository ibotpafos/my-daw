# Начало разработки

## Состояние репозитория

Сейчас есть CMake core, экспериментальный C bridge, Swift/AppKit bundle, Undo/Redo и SQLite-черновики. Добавлен CI workflow, ещё не запущенный удалённо. Добавлены ограниченные WAV/RAM playback и Core Audio output: [аудиосрез 0.2.0](17-audio-slice.md). Xcode project и запись пока не реализованы. Подробности и исполняемые команды: [первый рабочий срез](16-implementation.md). API-ключи, облако и платные библиотеки не нужны.

## Окружение

Проверить `python3 scripts/doctor.py`. Наблюдение 2026-09-14: Swift/Clang/CMake доступны, активен `/Library/Developer/CommandLineTools`; `xcodebuild` требует полного Xcode. Это ограничение активного toolchain; поиск `/Applications/Xcode*.app` не обнаружил app по стандартной маске. Другие каталоги не инвентаризировались.

Для текущего AppKit-прототипа проверена сборка через CLT (`scripts/build-macos.sh`). Для будущей работы с Xcode app target установить/выбрать полный стабильный Xcode, один раз запустить его и завершить настройку SDK. Записать версию в manifest первого implementation PR. Не менять системный `xcode-select` скрыто; для проекта можно задавать `DEVELOPER_DIR` в локальном build script после выбора реального пути.

## Планируемая структура кода

```text
apps/macos/               # Xcode project, Swift UI, entitlements
engine/domain/            # проект и инварианты
engine/commands/          # dispatcher, transactions, history
engine/audio/             # render plan, nodes, buffers
engine/platform/macos/    # HAL, AU, ObjC++
engine/storage/           # SQLite, package, recovery
engine/bridge/            # C header, opaque handles
helpers/plugin-scanner/   # disposable process
modules/                  # first-party workflows
tests/                    # domain/DSP/storage/integration
benchmarks/               # fixtures and reports
```

Это целевая структура, директории с пустыми «реализациями» специально не считаются готовой функциональностью.

## Первый implementation slice

Реализован базовый вариант (см. ограничения выше): `daw_core` static library (C++20) и headless executable, который создаёт session, добавляет track, применяет gain command, делает Undo и сериализует/открывает проект. Затем Swift app вызывает тот же C bridge, показывает список дорожек и кнопки добавления/Undo. Только после этого подключить Core Audio device adapter и импорт WAV.

Критерий: один authoritative revision и один command implementation, используемые app и headless tests. Не начинать с фиктивного таймлайна, который потом требует замены всей модели.

## Конвенции

Swift identifiers/C++/schema keys на английском; продуктовые строки локализуются RU/EN, документация первоначально на русском. Формат времени и единицы измерения задаются типами. В C++ использовать RAII вне RT, span/explicit lengths, signed frame types. Каждый публичный вызов указывает thread affinity и ownership.

PR описывает изменение поведения, tests и пределы доказательств. Engine change требует focused DSP/concurrency tests, UI change — визуальной проверки затронутых состояний. Нельзя добавлять плагины/аудио/секреты в git. Большие fixtures генерируются или хранятся отдельно с разрешением.

## Будущая сборка и CI

Добавлены `CMakeLists.txt`, `CMakePresets.json` (debug/release/sanitizers) и `scripts/build-macos.sh`. Debug и sanitizers проверены локально; release preset предусмотрен, но отдельная release qualification ещё не выполнена.

CI: Linux headless domain/core, macOS compile/link + app tests. Physical audio performance — отдельная машина, не shared CI runner. Dependency downloads фиксируются SHA; no moving branches. CI secrets не требуются для pull-request тестов; notarization отдельный release job.

## Проверка документации

`check_docs.py` выполняет базовую проверку стандартной библиотекой Python. Проверяет локальные Markdown-ссылки, JSON parsing, применяет минимальную SQL schema к временной in-memory DB и её ключевые ограничения. Если установлен `jsonschema`, проверяет schemas и примеры полностью; иначе явно пишет SKIP и не выдаёт это за полную schema validation. Ключ `--require-schemas` превращает отсутствие зависимости в ошибку.

Для полной проверки и HTML установлены documentation-only dependencies в локальную `.venv`, версии перечислены в [requirements-docs.txt](../requirements-docs.txt). Это snapshot среды подготовки, не audio runtime lock; wheel availability на других версиях Python проверить отдельно. Генератор `scripts/build_handbook.py` собирает offline HTML из Markdown. После редактирования спецификации пересобрать справочник.

Не считать успешную проверку спецификации доказательством работающего audio engine.
