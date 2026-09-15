# Первый рабочий срез — 0.1.0

> Исторический статус 0.1.0. Текущее поведение и изменение формата описаны в [аудиосрезе 0.2.0](17-audio-slice.md).

> Текущий срез 1.37.0: [полное удаление дорожки с Undo](64-delete-track-undo.md). Формат SQLite не менялся: save/open сохраняет surviving tracks, а Undo восстанавливает дорожку в памяти до следующего Save.

Дата проверки: 14 сентября 2026. Это запускаемое macOS-приложение для редактирования дорожек. Воспроизведения, записи, waveform, микширования аудиосигнала и plugin hosting пока нет. Fader меняет сохранённый параметр модели; он ещё не управляет звуком.

## Что работает

- Создание нового черновика и добавление до 256 дорожек.
- Unicode-названия, проверка корректности UTF-8 и лимита 120 Unicode scalars.
- Уровень от −120 до +24 dB; одна команда при отпускании ползунка.
- Undo/Redo с историей до 128 изменений. Новое изменение после Undo отбрасывает ветку Redo. ID дорожек не переиспользуются при Undo.
- Одна модель C++ и одна монотонная revision для изменений внутри открытой сессии; устаревшая команда отклоняется.
- Swift/AppKit вызывает тот же C bridge, который проверяется headless-тестами.
- Сохранение, Save As и открытие SQLite-черновика. Открытие сбрасывает Undo/Redo; при ошибке текущая модель сохраняется.
- Запрос сохранения при закрытии/новом проекте/открытии другого файла с несохранёнными изменениями.

## Сборка и запуск

Из корня репозитория на Apple Silicon Mac:

```sh
./scripts/build-macos.sh
open 'build/My DAW.app'
```

Скрипт использует установленный `xcrun swiftc`, SDK, CMake и системную SQLite, не загружает runtime-зависимости и не меняет `xcode-select`. В этой среде приложение успешно собрано с Command Line Tools без полного Xcode. Это уточняет прежнее предположение о блокирующей необходимости полного Xcode: для данного AppKit-среза достаточно CLT. Xcode app target, debugger setup, signing/notarization pipeline остаются будущей работой.

Target: arm64 macOS 14.0; реально проверен текущий Mac с macOS 26.6.2. Работа на macOS 14 и отдельных M1/M4 пока не проверена. Бинарник подписывается локальной ad-hoc подписью; он не нотарифицирован и не предназначен для публичного распространения.

`build/build-manifest.json` фиксирует версии toolchain/SDK и время сборки. C++ core собирается CMake в Debug; приложение компилируется в Swift 6 language mode.

## Проверки

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

Локально прошли два executable test targets в Debug и ASan/UBSan: domain/storage/bridge и отдельный чистый C consumer заголовка. Покрыты revision conflict, недопустимый gain/NaN, неизвестные ID, пустой и некорректный UTF-8, ветвление Undo/Redo, непереиспользование ID, лимиты дорожек/истории, SQLite roundtrip и замена snapshot, ошибка пути сохранения, отклонение испорченных данных без замены текущего проекта, ABI size и null arguments.

В реальном окне проверены добавление дорожки, переименование с фиксацией при уходе из поля, Undo/Redo, сохранение через NSSavePanel, перезапуск и открытие через NSOpenPanel. Сохранённый уровень −34.6 dB восстановился. Исправлено расположение строк вверху scroll view. Это ручной smoke-check; полного UI automation suite, VoiceOver-аудита и physical audio testing нет.

Добавлен [CI workflow](../.github/workflows/core.yml) для headless sanitizers на Linux/macOS и сборки приложения на macOS. Workflow ещё не запускался на GitHub; локальный проход не доказывает CI.

## Временный формат `.mydawdraft`

Это отдельный экспериментальный SQLite snapshot, **не** формат пакета `.mydaw` из спецификации. Он не принимает аудиопроекты и не обещает совместимость с будущими версиями. Файлы игнорируются Git.

- `PRAGMA application_id = 1296323159`, `user_version = 1`.
- `metadata(singleton, revision, next_id)` — одна строка.
- `tracks(position, id, name, gain)` — упорядоченный список.
- Undo/Redo, аудио, плагины, command log и UI selection не сохраняются.
- Запись создаёт временную базу рядом с файлом, выполняет SQLite transaction с FULL synchronous, закрывает базу и атомарно заменяет snapshot через POSIX rename.
- Ошибка до rename сохраняет прежний файл. Это не доказательство восстановления после отключения питания: directory fsync, аварийные kill-тесты и recovery journal ещё не реализованы.
- Reader открывает файл read-only, проверяет тип/версию, quick_check, данные и лимит размера 16 MiB; полная модель валидируется до публикации.
- Нет блокировки между несколькими экземплярами приложения или обнаружения внешней перезаписи. Пока открывать файл на запись только в одном экземпляре.

## Осознанные границы прототипа

C bridge v1 в [daw.h](../engine/bridge/daw.h) — экспериментальный внутренний интерфейс. Он не реализует JSON batch, capability permissions, projectId/commandId/idempotency из будущего workflow SDK. Названия копируются в caller-owned fixed buffer, POD содержит `struct_size`, исключения остаются внутри C++.

Все вызовы serial на main thread, включая ограниченные сохранения черновиков. Нет фонового storage worker, realtime callback или live preview. Изменения сначала находятся в памяти и становятся saved только после явного Save. До аудио/больших проектов нужно вынести controller/storage с main thread и реализовать durable command lifecycle согласно архитектуре. Текущий bridge запрещено вызывать из audio callback.

UI пока на русском, без готового RU/EN localization catalog. Нет удаления дорожки, точного числового ввода gain, drag reorder, автоматического восстановления последнего файла и Finder document association. Закрытие с несохранённой сессией спрашивает пользователя; silent autosave не заявлен.

## Следующий срез

1. Подключить Core Audio device adapter: выбор output, callback с тишиной, явные start/stop и telemetry.
2. Ввести audio time/prepared immutable render state и безопасное завершение callbacks.
3. Импорт WAV, read-ahead и воспроизведение одного клипа с этим gain; измерить underruns и фактическую latency.
4. Затем перенос на пакет `.mydaw`, storage worker/recovery и только после них запись входа.

B-002 выполнен частично: core/C bridge и AppKit bundle есть, Xcode project нет. B-003: базовые track commands/Undo готовы. B-004: snapshot save/open есть, полноценные package/recovery ещё не готовы. SP-03 plan retirement, SP-01 audio и остальные M0 spikes не закрыты.
