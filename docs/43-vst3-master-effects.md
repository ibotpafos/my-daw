# VST3 master effects

Версия 1.16.0 завершает основной code scope B-017. My DAW использует pinned VST3 SDK 3.8.1 build 84: discovery выполняется одноразовыми helper-процессами, а выбранный audio effect работает в общей serial master chain вместе с Audio Unit.

## Catalog и добавление эффекта

macOS UI запускает асинхронное сканирование установленных `.vst3`. Отдельный процесс перечисляет modules, затем новый процесс загружает каждый module и probes каждый audio-effect class. Timeout, crash и malformed output попадают в quarantine. Кэш принимает запись повторно только при совпадении path, class FUID, vendor, version и SHA-256 executable fingerprint.

В master section показаны отдельный VST3 popup, scan status, число доступных и quarantined entries. При добавлении host открывает выбранный class, получает исходные component/controller state и сохраняет insert одной revision. Не найденный после открытия проекта эффект остаётся в цепи как `MISSING`; рендер продолжает dry path.

## Audio lifecycle и параметры

Runtime поддерживает первый основной contract: один stereo input, один stereo output, float32, 48 kHz и блоки до 4096 frames. Host выполняет initialize, state restore, stereo bus negotiation, activation, setupProcessing и setProcessing; teardown идёт в обратном порядке. Renderer выбирает AU или VST3 для каждого master insert и суммирует заявленную latency.

Generic parameter panel читает `IEditController` и показывает нормализованные значения. Изменение передаётся controller и processor через `IParameterChanges` на sample offset 0, затем host сохраняет новые component/controller blobs. Latency и конечный tail читаются у активированного processor. Ошибка загрузки или обработки не обрывает transport: входной блок восстанавливается как dry, а счётчик plug-in errors растёт.

## Project и interchange

Этот срез использовал SQLite format v12. [Версия 1.18.0](45-track-bus-inserts-pdc.md) перенесла master, track и bus chains в общую format v13 таблицу. Audio Unit state ограничен 1 MiB на insert; VST3 использует строгий внутренний `MDVS v1` envelope до 16 MiB с FUID, module metadata/fingerprint и раздельными state blobs. Общий лимит plug-in state проекта — 16 MiB.

DAWproject не получает внутренний MDVS. Экспорт создаёт `Vst3Plugin` с canonical FUID и `plugins/master-<id>.vstpreset` в официальной chunk layout: `Comp`, optional `Cont` и `List`.

## Оставшиеся границы

Текущий runtime загружает выбранный VST3 в процесс приложения. Следующие hardening-срезы: runtime isolation, vendor editors, instruments/MIDI, sidechain и compatibility matrix реальных plugins. [Версия 1.19.0](46-plugin-automation-tail-export.md) подключила sample-offset parameter automation и bounded finite VST3 tail export; отдельная user policy для infinite tail остаётся частью hardening.

В этом срезе выполнена compile/build проверка. QA, запуск приложения, прослушивание и сторонние VST3 не выполнялись по текущему режиму разработки.

Официальные материалы: [VST3 SDK 3.8.1 build 84](https://github.com/steinbergmedia/vst3sdk/releases/tag/v3.8.1_build_84), [VST3 processing lifecycle](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html).
