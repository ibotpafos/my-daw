# 82. Структура проекта и воспроизводимые проверки

## Границы

`CMakeLists.txt` объявляет ядро, общие требования C++20 и зависимости. Версия
читается из корневого `VERSION`; изменение этого файла вызывает повторную
конфигурацию. Платформенные реализации и SDK находятся в
[`cmake/DawPlatform.cmake`](../cmake/DawPlatform.cmake), регистрация тестов — в
[`cmake/DawTests.cmake`](../cmake/DawTests.cmake).

Нейтральные типы VST3-каталога вынесены в
[`engine/plugins/vst3_catalog.hpp`](../engine/plugins/vst3_catalog.hpp).
Мост может читать эти данные на Linux без импорта macOS runtime. Сканирование
и загрузка плагинов остаются в платформенном слое; новый сканер не написан.

Из AppKit entry point вынесены существующие типы и реализации:
[`ProjectTime.swift`](../apps/macos/ProjectTime.swift) и
[`TimelineRulerView.swift`](../apps/macos/TimelineRulerView.swift). Это перенос
кода, а не смена алгоритма, проекта, UI framework или аудиодвижка. `DraftApp`
пока остаётся крупным координатором; дальнейшие извлечения требуют отдельных
проверок. Список Swift-файлов для сборки задан явно в
[`sources.txt`](../apps/macos/sources.txt); тест проверяет его полноту и уникальность.

## Одна команда запуска

```sh
./script/build_and_run.sh
./script/build_and_run.sh --verify
./script/build_and_run.sh --logs
./script/build_and_run.sh --telemetry
./script/build_and_run.sh --debug
```

Кнопка Run и старый `scripts/run-macos.sh` используют этот entry point.
Перед сборкой системный `NSRunningApplication.terminate()` запрашивает обычное
завершение только основного bundle данного checkout. Отмена/ожидание сохранения
не обходится принудительным сигналом: сборка отменяется, если приложение остаётся
открытым. Другие checkout и слоты Next/Candidate не завершаются по общему имени.
`--kill` сохранён как совместимый псевдоним штатного запроса выхода, не SIGKILL.
Проверка `--verify` сопоставляет точный bundle path, а не любое приложение с тем же
именем. Она доказывает наличие процесса, но не правильность GUI или аудио.

## Стиль и регрессии

`.editorconfig` задаёт кодировку, окончания строк и отступы. `.clang-format`
фиксирует настройки clang-format 18. Три ранее сжатых файла domain/bridge/storage
переформатированы отдельно; commit указан в `.git-blame-ignore-revs`.
CI проверяет форматирование именно мигрированных файлов, без массового скрытого
переформатирования остального кода. Скрипты запуска проверяет ShellCheck.

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
python3 -m unittest discover -s tests -p test_project_layout.py -v
```

CTest пишет JUnit XML и отказывает при отсутствии тестов. Presets явно разделяют
Debug, Release, ASan/UBSan и TSan; обычный CI выполняет ASan/UBSan, но не выдаёт это
за выполненный TSan-прогон.

Для обязательной проверки схем:

```sh
python3 -m venv build/docs-venv
build/docs-venv/bin/python -m pip install -r requirements-docs.txt
build/docs-venv/bin/python -m pip check
build/docs-venv/bin/python scripts/check_docs.py --require-schemas
```

В CI используется Python 3.14 и та же последовательность на обеих платформах.
Отсутствие `jsonschema` больше не считается успешной проверкой с SKIP.
Тесты `ProjectTime` выполняются отдельно через Swift 6 без окна и устройства:
проверяются смены темпа, обратное преобразование кадров, границы тактов и подписи.

## Артефакты и ограничения

Core CI сохраняет JUnit/CTest-логи и архив macOS arm64 development `.app` с SHA-256
и build manifest. Архив создаётся `ditto`, чтобы сохранить bundle и разрешения.
GitHub Actions закреплены полными commit SHA и работают с `contents: read`, без
сохранения credentials в checkout. Автозапись исходников в постоянных CI jobs
не используется.

Это ad-hoc development build, не нотарифицированный релиз. Стандартная конфигурация
не скачивает VST3 SDK и не запускает Tracktion spike. Для них нужен отдельный
SDK-enabled прогон. Физический MIDI, устройства ввода/вывода, прослушивание,
сохранение при реальном GUI-завершении и vendor compatibility остаются ручными
проверками. Результаты конкретных запусков фиксируются в PR с commit SHA, а не
объявляются пройденными только по наличию workflow.
