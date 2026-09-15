# Участие в разработке My DAW

My DAW пока находится на стадии локального аудиопрототипа. Изменения принимаются законченными вертикальными срезами: domain model, audio/runtime boundary, C bridge, storage и macOS UI должны оставаться согласованными.

## Подготовка

Требуются macOS 14+, Apple Silicon, Xcode Command Line Tools, CMake и Python 3. Полная подготовка и архитектурные ограничения описаны в [документации разработки](docs/12-development.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 8
ctest --test-dir build --output-on-failure
./scripts/build-macos.sh
```

Для документации:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-docs.txt
.venv/bin/python scripts/build_handbook.py
.venv/bin/python scripts/check_docs.py --require-schemas
```

## Ветки и изменения

- Создавайте ветку от актуального `main`; для локальной разработки используется префикс `codex/` или краткое имя задачи.
- Один commit должен описывать одно законченное изменение. Формат заголовка: `type(scope): действие`, например `feat(project): add track folders`.
- Не включайте в Git сборки, пользовательские проекты, аудиофайлы, recovery-файлы, ключи подписи или `.env`.
- Изменения real-time кода не должны добавлять locks, allocation, file I/O или destruction в audio callback.
- Изменения формата проекта требуют backward/negative fixtures и явного описания миграции.

## Проверка

Минимум перед review: Debug CTest, релевантный sanitizer, проверка документации и сборка arm64 app. UI-изменения проверяются в новом тестовом черновике. Hardware, listening и vendor-plugin compatibility указываются как проверенные только после фактического физического прогона.

Лицензия проекта ещё не принята. До появления корневого `LICENSE` внешние contributions следует предварительно согласовать с maintainer; предложение по лицензированию описано в [open-source policy](docs/13-open-source.md).
