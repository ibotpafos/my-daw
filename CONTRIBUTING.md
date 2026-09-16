# Участие в разработке My DAW

My DAW находится на стадии аудиопрототипа. Изменения принимаются законченными
вертикальными срезами: domain model, audio/runtime boundary, C bridge, storage
и macOS UI должны оставаться согласованными. Перед общим механизмом сначала
рассмотрите существующий код, системный SDK и поддерживаемые библиотеки —
подробности в [AGENTS.md](AGENTS.md).

## Подготовка

Для приложения требуются macOS 14+, Apple Silicon, Xcode Command Line Tools,
CMake 3.28+ и Python 3.14 для документации/проверок структуры. Для ядра на Ubuntu
также нужны SQLite development headers, pkg-config и libsamplerate 0.2.2+:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake libsqlite3-dev pkg-config libsamplerate0-dev
```

Версия libsamplerate разрешается системным package manager и проверяется CMake;
не выдавайте upstream SHA из manifest за побитовый lock пакета дистрибутива.
Обычная сборка не скачивает SDK. VST3 включается отдельно после явного bootstrap
из `scripts/bootstrap-vst3-sdk.sh`; Tracktion/JUCE остаются spike-only.

```sh
cmake --preset debug
cmake --build --preset debug --parallel 2
ctest --preset debug
cmake --preset sanitizers
cmake --build --preset sanitizers --parallel 2
ctest --preset sanitizers
```

На Mac единая команда сборки и запуска:

```sh
./script/build_and_run.sh
./script/build_and_run.sh --verify
```

Кнопка Run использует эту же команду. Она запрашивает штатный выход только
основного bundle данного checkout; отмена выхода не обходится принудительным
завершением. `scripts/run-macos.sh` — совместимый псевдоним. Для одной сборки без
запуска используйте `./scripts/build-macos.sh`.

Для документации и структуры:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-docs.txt
.venv/bin/python -m pip check
.venv/bin/python scripts/check_docs.py --require-schemas
.venv/bin/python -m unittest discover -s tests -p test_project_layout.py -v
.venv/bin/python scripts/e2e_coverage.py --strict
```

Описание структуры и артефактов — в [руководстве сопровождения](docs/82-project-maintenance.md),
платформенной конвертации — в [документации ресемплера](docs/83-portable-resampling.md),
остальные ограничения — в [документации разработки](docs/12-development.md).

## Ветки и изменения

- Создавайте ветку от актуального `main`; используйте префикс `codex/` или краткое имя задачи.
- Один commit должен описывать одно законченное изменение. Формат: `type(scope): действие`.
- Не включайте в Git сборки, пользовательские проекты, аудиофайлы, recovery-файлы, ключи подписи или `.env`.
- В real-time audio callback не добавляйте locks, allocation, file I/O или destruction, включая вызовы стороннего кода.
- Изменения формата проекта требуют backward/negative fixtures и явного описания миграции.

При добавлении Swift-файла обновляйте `apps/macos/sources.txt`. Стиль редактора
задаёт `.editorconfig`, C++ форматирует clang-format 18 с `.clang-format`.
Массовое форматирование оформляйте отдельно от изменения поведения; первый
такой commit записан в `.git-blame-ignore-revs`.

## Проверка

Минимум перед review: Debug CTest, релевантный sanitizer, обязательная проверка
схем/документации и сборка arm64 app. ASan/UBSan preset останавливает тест при
undefined behavior, а не оставляет предупреждение в успешном логе. TSan —
отдельный preset и отдельный прогон; ASan не подменяет проверку гонок.

GitHub Actions сохраняет JUnit/CTest-логи и macOS development `.app` с checksum
и build manifest. Этот архив имеет ad-hoc подпись, но не нотарифицирован.
Сборка не доказывает запуск GUI или корректное прослушивание; аппаратные и
vendor-plugin проверки отмечаются только после фактического прогона.

Лицензия проекта ещё не принята. До появления корневого `LICENSE` внешние
contributions следует предварительно согласовать с maintainer; предложение
описано в [open-source policy](docs/13-open-source.md).
