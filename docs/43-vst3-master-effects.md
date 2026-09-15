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

## Isolation и оставшиеся границы

[Версия 1.31.0](58-vst3-managed-runtime-isolation.md) добавила отдельный runtime helper для выбранного `OutOfProcess` VST3 insert. Он не заменяет in-process path: выбранная policy сохраняется в проекте, а фактический runtime публикуется отдельно. Helper обслуживает один prepared insert через fixed 4096-frame stereo/48 kHz pipeline; эта дополнительная latency входит в PDC. После missing, late или malformed ответа host один раз сообщает failure, затем продолжает deterministic delayed dry path. Перед запуском helper проверяет сохранённый MDVS envelope и SHA-256 executable fingerprint module.

В [1.32.0](59-vst3-remote-parameter-editor.md) generic parameter editor стал доступен и для isolated VST3. Это не remote vendor UI и не live audio helper: отдельный one-shot control helper выполняет bounded synchronous `list`, `snapshot` и `set`, повторно проверяет MDVS envelope и module fingerprint. Session metadata cache удерживает metadata между control calls; успешный set сохраняет state одной revision, останавливает playback и требует reprepare graph. Storage остаётся format v15 без миграции. Parameter automation по-прежнему идёт audio-protocol sample-offset events; macOS slider в isolated dialog noncontinuous, чтобы один drag не запускал helper на каждом промежуточном значении. Isolated AU по-прежнему не получает remote generic editor.

Этот control path может задержать dialog на время bounded synchronous вызова. Self-hosted fake-helper test покрывает list/snapshot/set, malformed payload, crash, nonzero exit и timeout с reaping; полный debug CTest проходит. Vendor UI, реальные vendor VST3, instruments/MIDI, sidechain, compatibility matrix и listening acceptance остаются открытыми. [Версия 1.19.0](46-plugin-automation-tail-export.md) подключила sample-offset parameter automation и bounded finite VST3 tail export; отдельная user policy для infinite tail остаётся частью hardening.

Официальные материалы: [VST3 SDK 3.8.1 build 84](https://github.com/steinbergmedia/vst3sdk/releases/tag/v3.8.1_build_84), [VST3 processing lifecycle](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html).
