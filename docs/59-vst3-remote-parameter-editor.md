# VST3 remote parameter editor

Версия 1.32.0 добавляет control-path для generic parameter editor выбранного `OutOfProcess` VST3 insert. Это продолжение B-012, а не завершение VST3 hosting: vendor editor и реальная compatibility matrix остаются отдельными задачами.

## Назначение и граница процесса

Audio runtime helper продолжает обслуживать только fixed delayed audio pipeline подготовленного insert. Новый helper не участвует в audio callback и не остаётся live worker: для одного bounded synchronous control вызова создаётся отдельный **one-shot control helper**, который выполняет `list`, `snapshot` либо `set` и завершается.

`list` возвращает parameter metadata, `snapshot` возвращает сохранённые значения, а `set` применяет одно normalized значение в диапазоне 0...1. Каждый новый helper повторно проверяет `MDVS` envelope и executable fingerprint VST3 module. Session metadata cache заполняется одним валидированным `list` и действует до следующей project revision; `set` запускает новый helper и независимо повторяет проверку module.

Реализация использует официальный Steinberg VST3 SDK и системные macOS POSIX process/shared-memory primitives. Дополнительная универсальная IPC-библиотека здесь не добавлена: протокол фиксирован, ограничен по размеру и отделён от realtime callback.

## Контракт состояния

Generic C ABI остаётся точкой входа для параметров track, bus и master insert; UI не получает отдельный private API для VST3. При isolated VST3 bridge направляет generic list/snapshot/set в control helper. Успешный set сохраняет новый VST3 state и создаёт ровно одну project revision. Затем playback останавливается, а следующий Play выполняет controlled graph reprepare.

SQLite format остаётся v15: новая таблица и миграция не требуются. Durable plug-in parameter automation не превращается в череду control calls. Она остаётся audio-protocol path с sample-offset parameter events внутри подготовленного graph.

## macOS UI

Кнопка «Параметры VST3…» доступна только для isolated VST3. Перед чтением списка параметров приложение вызывает `daw_stop`, затем показывает прежний generic dialog. Его sliders для isolated режима noncontinuous: drag фиксирует одно значение после отпускания, поэтому helper не запускается для каждого промежуточного события. Tooltip, accessibility label и help сообщают, что VST3 обслуживается отдельным helper и изменение вызывает controlled reprepare.

Isolated AU остаётся без remote parameter editor. Для него control блокируется с явной подсказкой переключить hosting policy в «В процессе».

## Ограничения и verification scope

Control path синхронный и bounded, но может задержать открытие dialog либо применение значения. Он не является remote vendor UI и не подтверждает безопасную работу произвольного vendor plugin.

Debug build и полный CTest проходят. Self-hosted fake-helper test подтверждает list/snapshot/set, строгий разбор parameter payload, отклонение malformed reply и completion с ненулевым exit, а также bounded crash/timeout handling с reaping. Bridge regression отдельно подтверждает, что isolated AU остаётся заблокирован без изменения project revision.

Открытые проверки:

- реальный VST3 vendor list/snapshot/set на Apple Silicon;
- реальное сохранение и повторное открытие после isolated vendor set;
- fingerprint mismatch и stale metadata на реальном module;
- матрица buffer size/sample rate, realtime/offline export и parameter automation;
- vendor UI, physical audio path и listening acceptance.

Связанные документы: [managed runtime isolation](58-vst3-managed-runtime-isolation.md), [plugin hosting](08-plugin-hosting.md), [VST3 master effects](43-vst3-master-effects.md) и [roadmap](11-roadmap.md).
