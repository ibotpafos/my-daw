# План разработки

## Текущий прогресс

Первый track-editor slice реализован: [сборка, поведение и доказательства](16-implementation.md). B-002/B-003 начаты; B-004 закрыт в части snapshot save/open/recovery. [0.7.0](22-clip-operations-fades.md) закрыл B-008. [0.9.0](24-crash-safe-recording.md) добавил recoverable input. [1.0.0](25-offline-export.md) закрыл B-009. Версии 1.1–1.9 добавили grid, jobs, crossfades, output lifecycle, mixer, comp, loop recording, Tracktion proof и routing. [1.10.0](36-apple-au-master-inserts.md) начал B-012. [1.11.0](37-au-scanner-parameters.md) добавил installed-AU scanner/cache и generic parameters. [1.12.0](38-volume-automation-pdc.md) начал B-013. [1.13.0](39-multi-automation-workflow.md) расширил automation и реализовал B-014 preview/commit. [1.14.0](40-vocal-preparation-workflow.md) реализовал B-015 в code scope. [1.15.0](41-dawproject-export.md) реализовал B-016 в code scope. [1.16.0](43-vst3-master-effects.md) реализовал основной B-017 code scope. [1.17.0](44-automation-write-pdc-plan.md) добавил B-013 Touch/Latch и runtime graph PDC planner. [1.18.0](45-track-bus-inserts-pdc.md) активировал Audio Unit/VST3 chains на дорожках и buses, block graph DSP и фактическую PDC всех routing edges. [1.19.0](46-plugin-automation-tail-export.md) добавил durable plug-in parameter lanes, sample-offset AU/VST3 playback, DAWproject automation и bounded finite VST3 tail export. [1.20.0](47-plugin-touch-audible-playhead.md) добавил Touch/Latch для plug-in controls, lock-free live override и transport по воспринимаемой позиции после graph latency. [1.21.0](48-professional-workspace.md) собрал arrangement, mixer console и прямое редактирование клипов в единое рабочее окно. [1.22.0](49-studio-pro-workspace-meters.md) добавил Studio Pro-inspired dock hierarchy, общий zoom/ruler, inspector, сохранение layout и realtime stereo meters. [1.23.0](50-inspector-browser.md) превратил правый dock в редактируемый channel/clip inspector и встроенный WAV/AU/VST3 browser. [1.24.0](51-pinned-headers-keyboard.md) завершил B-018 code scope закреплёнными track headers, раздельным horizontal timeline scroll и единым keyboard command boundary. [1.25.0](52-ui-qa-hosting-policy.md) исправил реальные layout defects и добавил v15 hosting policy foundation без скрытого downgrade. [1.26.0](53-async-auv3-hosting.md) добавил двухфазный async playback prepare и строгий AUv3 OOP runtime. [1.27.0](54-design-system-workspace-modes.md) подключил reusable design system, функциональные workspace modes и повторный визуальный QA. [1.28.0](55-accessible-mixer-controls.md) сделал custom meters/faders отдельными AX controls и проверил изменение gain через accessibility action. Physical/listening/vendor compatibility gates остаются отдельными; B-012 всё ещё требует vendor matrix и editors.

[1.29.0](56-studio-pro-arrangement-first-layout.md) сделал arrangement главным рабочим пространством с одним верхним toolbar и нижним transport/status bar; Inspector и Mixer теперь появляются по режиму, а pinned track headers имеют ширину 195 pt. Physical/listening/vendor compatibility gates остаются отдельными; B-012 всё ещё требует vendor matrix, editors и VST3 isolation.

[1.30.0](57-studio-pro-mixer-console.md) превратил нижний Mixer в полноценный горизонтальный console bank: узкие track/bus/master strips показывают Inserts, Sends, output, pan, рабочие channel controls, stereo meter/fader, automation и цветной footer. Полные routing/send/insert actions сохранены в раскрываемой панели «Детали канала». Physical/listening/vendor compatibility gates остаются отдельными; B-012 всё ещё требует vendor matrix и editors.

[1.31.0](58-vst3-managed-runtime-isolation.md) добавил managed VST3 runtime isolation: один disposable helper на prepared OOP insert, fixed stereo float32 48 kHz pipeline до 4096 frames, PDC latency, parameter offsets, MDVS/fingerprint validation, volatile runtime status и one-fault delayed dry fallback. Срез проверен только с self-hosted fake helper; реальные vendor VST3, прослушивание, editor proxy и compatibility matrix остаются незакрытыми.

[1.32.0](59-vst3-remote-parameter-editor.md) частично продолжил B-012: isolated VST3 использует one-shot control helper для generic `list`/`snapshot`/`set`, с MDVS+fingerprint revalidation, session metadata cache и одной revision на успешный set. Playback останавливается и готовится заново; UI использует тот же generic dialog с noncontinuous slider. Self-hosted fake-helper test покрывает success, malformed reply, crash, nonzero exit и timeout/reaping. Это не live audio worker и не vendor editor; реальные vendor VST3, compatibility matrix и listening остаются открытыми.

[1.33.0](60-infinite-tail-export-policy.md) завершает policy-срез B-013: WAV export различает finite и infinite declarations и даёт выбрать automatic bounded tail, finite-only либо 2/5/15/30-секундный предел. Политика живёт в локальных export preferences, не меняет проект и не подменяет legacy C ABI. Engine/bridge CTest и arm64 app build прошли; tail dialog ещё требует повторного визуального/VoiceOver smoke после сбоя Accessibility backend. Physical presentation timestamp и listening acceptance по-прежнему отдельные gates.

[1.34.0](61-audio-browser-preview.md) добавляет B-018 browser audition: AVFoundation-плеер открывает только выбранный WAV из явно добавленной папки, не вызывает C bridge и не меняет transport, revision, Undo или draft. Смена выбора, папки, импорт, новый/открытый проект и завершение приложения останавливают preview. Unit-level controller test регистрируется как Apple CTest; ручной слуховой и VoiceOver smoke остаются отдельными gates.

[1.35.0](62-variable-rate-wav-import.md) закрывает ограниченный import compatibility slice: macOS принимает mono/stereo PCM WAV 44,1 / 48 / 88,2 / 96 / 192 kHz и приводит их к каноническим 48 kHz до C++ domain/storage. Использован системный Apple AudioConverter за существующей границей импорта; 48 kHz остаётся direct fast path, проектный формат и realtime callback не меняются. CTest и macOS bundle QA описаны в отдельном документе; listening, physical device matrix и non-Apple resampler остаются отдельными gates.

[1.36.0](63-background-wav-import.md) переносит bounded PCM WAV import с control/main thread в отдельную cancellable job: чтение, проверка, decode и resampling не блокируют editing или transport. Готовый immutable result применяется в один revision-aware commit; если проект изменился, UI не применяет его сам и предлагает явное добавление в актуальную ревизию. Новый/открытый проект и завершение приложения отменяют job до смены session. Это не streaming media engine, не proof качества resampling и не listening/physical gate.

[1.37.0](64-delete-track-undo.md) добавляет базовое управление проектом: выбранная дорожка удаляется полностью одним revision-aware commit, а Undo/Redo возвращают или снова убирают тот же immutable snapshot. Удаление не меняет buses и master chain; после сохранения/открытия остаётся только surviving project state. Это не операция с корзиной, не media garbage collection и не proof слушательского или физического поведения.

[1.38.0](65-track-reorder.md) делает порядок track list редактируемым и durable: пользователь переносит целую дорожку между соседями, а её media, takes, automation и inserts следуют за тем же owner. Одна перестановка — одна revision и один Undo snapshot; SQLite сохраняет новый `tracks.position`. Это не folder/group hierarchy, не rewire routing и не listening/physical gate.

[1.39.0](67-aiff-import.md) закрыл формат-путь IMP-01: AIFF/AIFC импорт тем же фоново-ресемплинг путём; добавлен целевой CTest для DAWproject export; встроенный EQ выведен из скоупа решением владельца (2026-09-15) — эффекты и инструменты через AU/VST3-хостинг. UI-путь AIFF в приложении смонтирован (роутинг по расширению, панели WAV/AIFF); физические гейты (B-010/B-012/B-013) открыты.

[1.40.0](68-midi-foundation.md) закрывает MIDI-фундамент: нотная модель с валидациями и лимитами в domain, SQLite `user_version` 16, версионированный C-ABI с постраничным чтением, доставка событий в renderer-план (loop wrap без висящих нот, carry без потерь), 6-й аргумент `process()` и протокол managed-runtime v2 с frame-accurate эхом в изоляционном тесте, флаг Instrument в сканере VST3 (кэш 2), вход CoreMIDI (ring drop-oldest, running status) и табличный MIDI-редактор в инспекторе. Собственных тембров нет — их дают сторонние инструменты. Открытые гейты: audible-плеер MIDI-only дорожек (instrument-source), живой .vst3-синт, живое MIDI-железо, клики GUI.

[1.41.0](69-tempo-map.md) делает темп и размер частью проекта: точки с обязательным якорем на frame 0, upsert-команды, обратимая пара beats↔frames с точностью ≤1 кадра на краю 2^40, черновик v17 с fallback для ≤16, C-ABI запросов/команд. UI (сетка, snap-to-beat, барная линейка) потребляет карту следующим слоем — пока открыто.

[1.42.0](70-instrument-source.md) закрывает слышимость MIDI: трек без аудио с нотами или инсертами становится голосом рендера (zero-fill вход, цепочка, PDC-хвост, ноль новых аллокаций), длина проекта включает MIDI, гейт 'Import audio or add a MIDI/instrument track', play в приложении для MIDI-only проекта, инструмент-браузер с experimental-метками. Открыто: живой .vst3-синт/AU MusicDevice, запись исполнения в клипы, мониторинг.

[1.43.0](71-metronome.md) — метроном из темпо-карты: клик/акцент в мастер-микшере RT-без аллокаций, побитово идентичный через границы блоков, с доказанным подавлением в экспорте. Слуховой гейт и pre-roll открыты.

[1.44.0](72-midi-recording.md) — запись MIDI в клип на движке: `appendMidiNotes` (клип не растёт, порядок захвата сохраняется) и `MidiRecorder` с LIFO-закрытием и счётчиками мусора. Живой контроллер, арминг и мост-функция открыты.

[1.45.0](73-tempo-ui.md) — приложение потребляет темпо-карту (bit-perfect Swift-зеркало с 25 харнес-парами, revision-aware commit с откатом), beat-grid, тактовая линейка, readout такт.бит.тики; мост несёт VST3 instrument-флаг, браузер отмечает `🎹`. GUI-клики — ручной гейт.

[1.46.0](74-markers.md) — маркеры-локаторы: опциональная дорожка `Marker{frame,name}` с общим `validateName()`, три revision-команды, черновик v18 и legacy-fallback, CTest `marker_model` (36 CHECK). Мост и UI — следующий шаг.

[1.47.0](75-midi-wiring.md) — запись и метроном у пользователя: 10 вызовов MIDI-входа/тейка в ABI (revision-free capture, одна ревизия на коммит), клип-рельативный конвертер с названными v0-допусками, тумблер клика через живой граф, попап MIDI-входа и «Запись с клавиатуры» в инспекторе; сквозной тейк доказан кросс-процессным CoreMIDI-хелпером. Живая клавиатура, слух и мышь — открыто.

[1.48.0](76-clip-track-editing.md) — полный набор операций над клипами и дорожками в движке: десять revision-команд (track mute/solo/color/gain/duplicate, clip color/gain в dB, MIDI color/transpose с клампом питчей/quantize по темпо-карте), черновик v19, `Voice.gain` в renderer (dB→linear в mix-in, 0 dB = бит-совместимо), восемь вызовов и append-поля `color`/`gain_db` в C-ABI (`daw_midi_clip` v2); CTest `clip_track_model` + clip-gain/fade секции + живая rev-секвенция в `pure_c_bridge`.

[1.60.0](77-project-package.md) — проектным архивом `.mydawzip`: упаковка сохранённого черновика с манифестом и атомарное извлечение («Файл → Упаковать проект…» / «Открыть проект из архива…»).

[1.59.0](76-clip-track-editing.md) — панорама клипа: одиннадцатое поле региона, unity-центровый закон как у дорожки, v21, пресеты в меню клипа.

[1.58.0](33-full-duplex-loop-recording.md) — преролл луп-записи: транспорт с кликом ведёт N секунд до punch-in, ведущие кадры выбрасываются из тейка (`skipFrames`), лейбл дубля остаётся на границе лупа.

[1.57.0](25-offline-export.md) — realtime-метр мастера: momentary/short-term LUFS по BS.1770-4 в аудио-пути без аллокаций, строка громкости на мастер-стрипе микшера.

[1.56.0](25-offline-export.md) — BS.1770-4 метр: integrated LUFS и true-peak готового WAV (`daw_measure_wav`), экспортное сообщение показывает цифры.

[1.55.0](76-clip-track-editing.md) — мультиселект клипов в дорожке: Ctrl-клик-группа, групповые delete и nudge на шаг сетки по одной ревизии (`deleteClips`/`nudgeClips`, мост с массивом индексов), плюс закрыт пропуск M/L-хоткеев из 1.52.0.

[1.54.0](63-background-wav-import.md) — drag-and-drop из Finder: WAV/AIFF падает на аудиоленту как take в снапнутой позиции через существующий фоновый импорт (движок и мост не тронуты), один job — одно сообщение.

[1.53.0](25-offline-export.md) — экспорт стемов по дорожкам: solo-синтез снапшота до мастера (гейн в единицу, мастер-цепочка мимо), «NN - имя.wav» с санитизацией, тихие дорожки пропускаются, отмена между файлами, `daw_begin_stem_export` и «Файл → Экспортировать стемы…».

[1.52.0](76-clip-track-editing.md) — состояние воспроизведения клипа: mute вынимает голос из render-плана (пустой план честно отказывает), loop продлевает регион за срез с wrap-чтением и reject разлупа через validate; черновик v20 (колонки muted/looped, гейт по версии), мост `daw_set_clip_muted/looped`, «M»/«L» в меню и хоткеях; попутно — регресс v19: trim и split больше не роняют gain/color.

[1.51.0](76-clip-track-editing.md) — клипы путешествуют: четыре revision-команды copy/move между дорожками (аудио и MIDI), правило общего take-источника для аудио-вставки, одиночный регион не покидает дорожку, пересечения по-прежнему решает validate; мост `daw_*_clip_to_track` ×4 с живой MIDI-секцией в `pure_c_bridge`; UI — контекстное меню (копировать/копировать на дорожку/перенести на дорожку), хоткеи C/V, «Перенести MIDI-клип на дорожку…» в «•••». Физическая мышь и слух — открытые гейты.

[1.50.0](74-markers.md) — маркеры дошли до линейки: C-ABI `daw_marker` v1 (144 байта, count/get/add/rename/remove, expected-revision, живая секция в `pure_c_bridge` включая reject дубля кадра и undo/redo-историю), полоса флажков над тактовой шкалой: клик — переход, двойной клик — добавить (snap к сетке), правый клик — переименовать/удалить, Cmd+Shift+M — маркер в позицию курсора. Мышь и VoiceOver по новой полосе — открытые ручные гейты.

[1.49.0](76-clip-track-editing.md) — те же операции у пользователя: контекстное меню клипа (цвет, громкость dB, дубль/split/удаление), горячие S/D/Delete в фокусе волны, durable-цвет дорожки красит хедер/строку/микшер/волну с fallback на палитру, «Дублировать дорожку» и палитра в «•••», транспонирование/квантование/цвет MIDI-клипа из того же меню, пункты и гейты в меню «Проект». Мышь, клавиатура и слух по новым контролам — открытые ручные гейты.

## Последовательность

Работа идёт законченными вертикальными срезами. Сроки ниже — ориентиры для планирования небольшой команды с C++ audio и macOS опытом; это не обещание календарной даты. Если такой экспертизы нет, сначала обучающие spikes и переоценка.

| Этап | Выход | Предварительный диапазон | Критерий перехода |
|---|---|---|---|
| M0 | Toolchain + audio/UI/storage/host spikes | 2–4 недели команды | Развилки ADR имеют данные |
| M1 / S0 | 8-track import/play/record/edit/save/export | Ещё 4–8 недель | Сквозной proof с физическим входом |
| M2 / S1 | Дубли, comp, buses, automation, AU | Ещё 8–16 недель | Vocal flow + recovery + F1 + AU matrix |
| M3 / S2 | Workflow SDK v0, preview/Undo, один полезный модуль | Ещё 4–8 недель | Модуль добавляется без private API |
| M4 | VST3, DAWproject, beta hardening | Ещё 8–16+ недель | Compatibility, migration и install evidence |

Диапазоны не включают создание полноценной линейки инструментов, модели уровня коммерческого stem splitter и универсальную замену зрелой DAW. Параллельность команды может менять календарь; один разработчик должен планировать более длинный путь. После M0 оценить задачи заново по фактической скорости.

## M0: прототипы решений

| ID | Эксперимент | Time box | Доказательство / stop condition |
|---|---|---|---|
| SP-01 | Core Audio full-duplex → ring → writer | 3–5 рабочих дней | 10 минут записи, устройство отключается без silent success |
| SP-02 | AppKit canvas против Metal peaks | 2–3 дня | Один fixture, pan/zoom/frame/RAM/VoiceOver вывод |
| SP-03 | **Prototype готов:** C bridge job lifetime; audio plan retirement продолжается в B-005 | — | 4-slot saturation, explicit busy, released handles/session shutdown; Debug + ASan/UBSan + TSan |
| SP-04 | SQLite package recovery | 2–3 дня | Kill до/после commit, recovery copy, Save As |
| SP-05 | **Реализация готова:** disposable enumeration и отдельный probe каждого effect | — | Timeout/crash/malformed quarantine реализован; installed third-party matrix ещё не прогонялась |
| SP-06 | **Готов:** pinned Tracktion/JUCE vertical slice | — | Два tracks, WAV import, gain/loop, save/reload, 48 kHz offline render; размер/build/license constraints записаны |

Spikes одноразовые или изолированные; их нельзя автоматически превратить в production код без review. Если SP-01/SP-05 дважды не дают пригодного proof, пересмотреть native engine decision до масштабирования UI.

## Начальный backlog

| ID | Приоритет | Зависит от | Готово, когда |
|---|---|---|---|
| B-001 | P0 | — | Полный Xcode выбран, build manifest фиксирован |
| B-002 | P0 | B-001 | CMake core + app target + C bridge smoke собираются |
| B-003 | P0 | B-002 | Domain model, commands и Undo проходят инварианты |
| B-004 | P0 | B-003 | Project create/save/open и recovery доказаны |
| B-005 | P0 | B-002 | **Prototype частично готов:** adapter + silent callback + lifecycle metrics; physical disconnect ещё нужен |
| B-006 | P0 | B-003, B-005 | WAV read-ahead, transport и output |
| B-007 | P0 | B-004, B-005 | **Prototype готов:** recoverable dry media; physical proof ещё нужен |
| B-008 | P0 | B-006 | Timeline selection/move/trim/split с Undo |
| B-009 | P0 | B-006, B-004 | **Основной code scope готов:** offline WAV согласован с playback, PDC и bounded finite VST3 tail; listening proof ещё нужен |
| B-010 | P1 | B-007, B-008 | **Реализация готова к physical gate:** take lanes, comp, playback loop и AUHAL loop recording; latency/recovery v2/physical proof ещё нужны |
| B-011 | P1 | B-006 | **Native prototype готов:** buses/sends, DAG validation, realtime plan, UI и format v9; Tracktion parity переносится в B-012 |
| B-012 | P1 | B-011, SP-05 | **Частично реализован editor proxy:** AU/VST3 inserts на track/bus/master, scan/cache, parameters/state, missing fallback, routing parity, строгий AUv3 OOP runtime, managed VST3 runtime isolation и isolated VST3 one-shot generic parameter control path; vendor editors и compatibility matrix реальных plug-ins ещё открыты |
| B-013 | P1 | B-012 | **Основной code scope готов:** channel и plug-in Read/Touch/Latch gestures, durable sample-offset AU/VST3 parameter lanes, atomic Undo, graph PDC, latency-compensated audible playhead, finite tail и user policy для infinite tail; hardware presentation timestamp и listening acceptance ещё нужны |
| B-014 | P1 | B-003 | **Prototype готов:** bundled manifest, typed preview и atomic one-revision commit через public C boundary |
| B-015 | P2 | B-014, B-010 | **Code scope готов:** selected Lead/Doubles, peak/RMS suggestions, preview и atomic apply; listening evidence ещё требуется |
| B-016 | P2 | B-009, B-013 | **Code scope готов:** DAWproject 1.0 XML/ZIP, media/AU/VST3 state, routing, channel и plug-in parameter automation, loss report; import matrix ещё не прогонялась |
| B-017 | P2 | B-012 | **Основной code scope готов:** pinned MIT SDK, isolated scan/cache, stereo master processing, parameters/state, latency/tail, dry fallback, managed runtime isolation и DAWproject `.vstpreset`; vendor matrix, editors и sidechain ещё нужны |
| B-018 | P1 | B-008, B-011, B-013 | **Code scope и локальный visual/AX QA готовы:** Studio Pro-inspired arrangement-first workspace с одним top toolbar, bottom transport/status, 195-point pinned track headers и режимным Inspector/Mixer; полноразмерный horizontal console bank с Inserts/Sends/routing summaries, track/bus/master controls, live stereo meters/faders и раскрываемыми подробными channel actions; shared zoom/ruler, synchronized vertical scroll, editable channel/clip inspector, WAV/AU/VST3 browser с read-only WAV audition, persisted dock layout, direct move/trim/fade и единая keyboard command boundary; физический пользовательский workflow, фактическое audition и полный VoiceOver audit ещё нужны |

## Риски

| Риск | Ранний сигнал | Действие |
|---|---|---|
| Собственный engine съедает проект | После M0 нет устойчивой записи | Tracktion comparison, сократить features |
| «Native UI» оказывается дорогим canvas | Плохой frame time/accessibility | AppKit/Metal comparison, меньше custom controls |
| Plugins нарушают deadline | Lookahead/IPC stalls на 128 frames | Ограничить supported matrix, явный monitoring mode |
| Потеря сессии | Recovery даёт dangling refs | Исправить commit order до новых функций |
| SDK слишком общий | Модулю нужен private pointer | Разобрать конкретный use case, расширить typed API |
| ИИ отвлекает от core | Нет S1, но растёт число моделей | Заморозить AI до audio acceptance |
| Усталость maintainers | Нет review/tests/release owner | Небольшой scope и release cadence |
| Неудобный формат миграции | Старые fixtures не открываются | Versioned schema и compatibility suite |

## Организация команды

Ответственности: audio/hosting, macOS UX, domain/storage, testing/release. Один человек может совмещать роли. Контрибьюторы начинают с bounded tasks, а не переписывания ядра. GitHub issues/PR могут быть созданы после появления remote; в этой подготовке ничего не опубликовано.
