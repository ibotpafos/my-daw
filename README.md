# My DAW

Нативная музыкальная станция для Mac: запись, монтаж и сведение в едином интерфейсе, расширяемом через общую систему действий. My DAW — рабочее имя, не выбранный публичный бренд.

**Состояние: аудио+MIDI прототип 1.69.0.** Режимы Сведение и Мастеринг используют Studio Pro-inspired нижнюю консоль: узкие вертикальные track/bus/master strips, верхние Inserts/Sends summaries, routing, pan, channel controls, длинные stereo meters/faders, automation state и цветные подписи каналов. Консоль занимает половину рабочего окна, прокручивается по горизонтали и не вытесняет arrangement; полные routing/send/insert actions открываются кнопкой «Детали канала». Создание и Запись по-прежнему отдают почти всё окно arrangement. Дорожку можно удалить или переставить одним revision-aware действием из arrangement или mixer; Undo возвращает её содержимое либо прежний порядок, включая медиа, дубли, automation и inserts. Импорт и импорт дубля принимают mono/stereo PCM WAV с частотой 44,1 / 48 / 88,2 / 96 / 192 кГц, читают и конвертируют их к внутренним 48 кГц в фоне, а затем добавляют одним revision-aware действием. Во время импорта можно редактировать и слушать проект; отмена, состояние и готовый результат видны в status surface. Импорт принимает также AIFF/AIFC (PCM 16/24/32-bit и 32-bit float) в тех же границах частот, с ресемплингом в 48 kHz в фоне; UI-путь AIFF смонтирован (роутинг по расширению). MIDI-фундамент: нотная модель и команды в domain, черновик v16, версионированный C-ABI со страничным чтением, доставка событий в renderer-план и VST3 managed-runtime (протокол v2), вход CoreMIDI и табличный MIDI-редактор в инспекторе; тембры дают сторонние AU/VST3 — собственных плагинов нет; instrument-треки слышимы (трек без аудио с нотами или инсертами — голос рендера), темп и размер — карта проекта (черновик v17, beats↔frames), play для MIDI-only проектов в приложении. Приложение читает темпо-карту (beat-grid, тактовая линейка, readout такт.бит.тики), метроном синтезируется в мастере из карты с подавлением в экспорте, запись MIDI-исполнения закрыта движком (arm/feed/stop + append-команда), VST3-инструменты помечаются `🎹` из флага сканера. Запись MIDI и метроном дошли до пользователя (выбор MIDI-входа, арминг тейка в инспекторе, тумблер клика; сквозной тейк проверен кросс-процессным CoreMIDI-хелпером), маркеры-локаторы живут в домене и схеме v18, а с 1.50.0 — и на линейке: полоса флажков с переходом по клику, добавлением по двойному клику (snap к сетке) и меню переименования/удаления. Редактор клипов и дорожек (схема v19): цвет и громкость в dB у клипа, цвет/дубликат дорожки, mute/solo/gain, цвет/транспонирование/квантование MIDI-клипа — из контекстного меню волны, хоткеев S/D/Delete и меню «•••»; с 1.51.0 клипы копируются и переносятся между дорожками (хоткеи C/V, подменю целей, MIDI-клип едет целиком с lane и цветом), с 1.52.0 у клипа есть состояние воспроизведения — mute и loop (хоткеи M/L; залупленный регион переживает свой срез, чтение оборачивается); с 1.53.0 «Файл → Экспортировать стемы…» пишет по WAV на слышимую дорожку до мастера; WAV/AIFF можно перетаскивать из Finder прямо на ленту — импорт take по месту отпускания тем же фоновым путём; с 1.55.0 клипы берутся в группу Ctrl-кликом (удаление и сдвиг на шаг сетки — одной ревизией); готовый WAV-экспорт меряется по BS.1770-4 и показывает LUFS/true-peak в статусе (1.56.0), а мастер-стрип микшера — живые momentary/short-term LUFS (1.57.0); луп-запись получила преролл с кликом перед punch-in (1.58.0), а клип — собственную панораму с unity-центровым законом (1.59.0); проект упаковывается в один архив `.mydawzip` и открывается из него (1.60.0), а MIDI-клипы теперь создаются и удаляются прямо из пиано-ролла (1.61.0); маркер-локаторы умеют «цикл отсюда до следующего» (1.62.0), а экспорт стемов ограничивается выбранной дорожкой (1.63.0); во время луп-записи вход можно слышать напрямую кнопкой MON (1.64.0), а диалог стемов выбирает дорожки галочками (1.65.0); дорожки группируются в шины прямо из шапки (1.66.0); вооружение дорожки автоматически включает мониторинг входа (1.67.0); шины удаляются из микшера кнопкой «✕» с подтверждением (1.68.0); Solo exclusive (Option+клик) и快捷键 Space/S/M/A (1.69.0). Открыто: живая клавиатура и мышь по новым контролам (включая полосу маркеров и редактор клипов), слуховая приёмка клика и живого синтеза, AU MusicDevice, pre-roll. Browser умеет локально прослушать выбранный WAV через AVFoundation: это отдельный короткий audition, не трогающий DAW transport, Undo, revision и проект. AUv3 использует обязательный системный out-of-process path, а VST3 может работать через managed helper: отдельный процесс на подготовленный OOP insert, fixed 4096-frame stereo/48 kHz pipeline и явный runtime badge. WAV export предоставляет явную политику infinite component: автоматический безопасный предел, исключение infinite component или ограничение 2/5/15/30 секунд при сохранении known finite decay. Реальная совместимость vendor plug-ins, живое MIDI-железо, listening acceptance и hardware presentation latency ещё не подтверждены.

Собрать и запустить на Apple Silicon Mac:

```sh
./script/build_and_run.sh
```

[Открыть единый HTML-справочник](docs/index.html) — все разделы, навигация, таблицы и ссылки; работает без сети. Markdown-файлы ниже — редактируемые исходники.

## С чего начать

1. [Концепция и границы продукта](docs/01-product.md) — какой опыт создаём и для кого.
2. [Выбранный стек](docs/03-stack.md) — на чём строим, альтернативы и причины выбора.
3. [Архитектура](docs/04-architecture.md) — как связаны интерфейс, проект и звук.
4. [Первая версия](docs/02-mvp.md) — требования с критериями готовности.
5. [План реализации](docs/11-roadmap.md) и [первый цикл разработки](docs/12-development.md).

Правила подготовки веток, проверок и pull request описаны в [CONTRIBUTING.md](CONTRIBUTING.md). Лицензия пока не выбрана; текущее предложение и dependency policy находятся в [open-source policy](docs/13-open-source.md).

## Целевое решение в одном экране

| Область | Базовый выбор |
|---|---|
| Платформа | macOS 14+, arm64; физическая проверка M1 и M4 |
| Интерфейс | Swift 6 language mode, AppKit; SwiftUI для настроек и небольших панелей |
| Таймлайн | Собственный AppKit canvas; Metal/MetalKit после сравнительного прототипа |
| Аудиоядро | Собственная domain/C ABI граница; Tracktion Engine/JUCE кандидат для graph, hosting и render services; Core Audio path остаётся reference до parity |
| Граница языков | Небольшой C API, Objective-C++ только в платформенном адаптере |
| Проект | Папка-пакет, SQLite, неизменяемые медиа, транзакции и восстановление |
| Эффекты | Встроенные сначала; AU на первой пользовательской альфе; VST3 следующим этапом |
| Расширения | Декларативные workflow-модули и типизированные команды; отдельный процесс для внешнего кода |
| ИИ | Необязательные фоновые задачи; план изменений → прослушивание → применение |
| Инструменты | Xcode для macOS app; CMake/CTest для ядра; Python только для утилит |
| Open source | Рекомендация: MPL-2.0 для приложения, Apache-2.0 для самостоятельного SDK; ещё не применена |

Это выбранное направление для прототипов, а не обещание универсальной DAW. Проверенный Tracktion proof изменил следующий шаг: routing и AU-hosting сначала сравниваются через backend adapter, при этом нативный интерфейс, формат проекта и command model остаются нашими. Условия миграции зафиксированы в [ADR-012](docs/adr/012-tracktion-adapter.md).

## Карта документации

| Документ | Содержание |
|---|---|
| [65 Track reorder](docs/65-track-reorder.md) | Reorder дорожек, точный owner subtree, Undo и SQLite round-trip |
| [66 Alpha acceptance](docs/66-alpha-acceptance.md) | Матрица приёмки S1, физические гейты и команды проверки |
| [67 AIFF import](docs/67-aiff-import.md) | Импорт AIFF/AIFC (PCM 16/24/32-bit, float32) с фоновым ресемплингом в 48 kHz |
| [64 Delete track with Undo](docs/64-delete-track-undo.md) | Полное revision-aware удаление дорожки, сохранение/открытие и восстановление Undo |
| [62 Variable-rate WAV import](docs/62-variable-rate-wav-import.md) | Пять PCM WAV частот, конвертация через Apple AudioConverter, границы и QA |
| [63 Background WAV import](docs/63-background-wav-import.md) | Фоновое чтение/конвертация, optimistic apply, отмена и UI state |
| [61 Audio browser preview](docs/61-audio-browser-preview.md) | Локальное AVFoundation-предпрослушивание WAV без изменения проекта и его границы QA |
| [60 Infinite-tail export policy](docs/60-infinite-tail-export-policy.md) | Явная user policy WAV-tail, versioned export options, локальное preference и AX workflow |
| [59 VST3 remote parameter editor](docs/59-vst3-remote-parameter-editor.md) | One-shot control helper, generic editor для isolated VST3, revision/reprepare и границы проверки |
| [58 VST3 managed runtime isolation](docs/58-vst3-managed-runtime-isolation.md) | Per-insert helper, fixed delayed IPC pipeline, runtime ABI/UI, dry fallback и границы проверки |
| [57 Studio Pro mixer console](docs/57-studio-pro-mixer-console.md) | Полноразмерная нижняя консоль, узкие channel strips, routing summaries, details toggle и visual/AX QA |
| [56 Arrangement-first Studio Pro layout](docs/56-studio-pro-arrangement-first-layout.md) | Один top toolbar, compact bottom transport, режимные Inspector/Mixer и visual/AX QA |
| [55 Accessible mixer controls](docs/55-accessible-mixer-controls.md) | AX roles/labels/value, keyboard и VoiceOver increment/decrement для channel strips |
| [54 Design system and workspace modes](docs/54-design-system-workspace-modes.md) | Semantic UI kit, functional workspace modes, compact lanes/mixer и результаты визуального QA |
| [53 Async AUv3 hosting](docs/53-async-auv3-hosting.md) | Bounded render-graph prepare, strict system OOP verification, cancel/stale transport и UI state |
| [52 UI QA and hosting policy](docs/52-ui-qa-hosting-policy.md) | Реальный layout QA, format v15 и явная per-insert hosting policy |
| [49 Studio Pro workspace and live meters](docs/49-studio-pro-workspace-meters.md) | Общий zoom/ruler, правый inspector, сохранение dock layout и realtime channel meters |
| [50 Inspector and browser](docs/50-inspector-browser.md) | Редактируемый channel/clip inspector и встроенный WAV/AU/VST3 browser |
| [51 Pinned headers and keyboard routing](docs/51-pinned-headers-keyboard.md) | Фиксированные track headers, синхронный vertical scroll и безопасные глобальные команды |
| [48 Professional workspace](docs/48-professional-workspace.md) | Arrangement/mixer split, channel strips, прямой clip move/trim/fade и компактный transport/status workflow |
| [47 Plug-in Touch/Latch and audible playhead](docs/47-plugin-touch-audible-playhead.md) | Continuous plug-in controls, one-revision gestures, live override и transport после graph latency |
| [46 Plug-in automation and tail export](docs/46-plugin-automation-tail-export.md) | Format v14, sample-offset AU/VST3 parameter curves, generic AUTO UI, DAWproject lanes и finite VST3 tail |
| [45 Track/bus inserts and graph PDC](docs/45-track-bus-inserts-pdc.md) | Сохраняемые channel chains, block graph DSP, фактическая PDC, общий UI/C ABI и DAWproject devices |
| [44 Automation write and PDC plan](docs/44-automation-write-pdc-plan.md) | Read/Touch/Latch, atomic gesture Undo и runtime graph delay planner |
| [43 VST3 master effects](docs/43-vst3-master-effects.md) | Catalog UI, master processing, parameters/state, latency/tail, dry fallback и DAWproject preset |
| [42 VST3 foundation](docs/42-vst3-foundation.md) | Pinned SDK, isolated scanner/quarantine/cache и format-neutral state envelope |
| [41 DAWproject export](docs/41-dawproject-export.md) | DAWproject 1.0 XML/ZIP, embedded media/AU state, routing/automation и loss report |
| [40 Vocal preparation workflow](docs/40-vocal-preparation-workflow.md) | Выбор Lead/Doubles, deterministic peak/RMS, gain proposal, preview и atomic apply |
| [39 Multi-target automation and workflow](docs/39-multi-automation-workflow.md) | Format v12, track/bus/master lanes, cache invalidation и manifest → preview → atomic commit |
| [38 Volume automation and PDC](docs/38-volume-automation-pdc.md) | Format v11, volume curves, post/pre-send semantics, master-chain latency reporting и compensated export |
| [37 AU scanner and parameters](docs/37-au-scanner-parameters.md) | Изолированный installed-AU scan, quarantine, generic parameters, missing-plugin fallback и Tracktion routing parity |
| [36 Apple AU master inserts](docs/36-apple-au-master-inserts.md) | Approved-каталог, state, latency, dry fallback, playback/export и format v10 |
| [35 Routing, buses and sends](docs/35-routing-buses-sends.md) | Main outputs, pre/post sends, DAG, realtime plan, UI и format v9 |
| [34 Tracktion Engine spike](docs/34-tracktion-engine-spike.md) | Exact dependency lock, edit/import/save/render proof и решение об adapter layer |
| [33 Full-duplex loop recording](docs/33-full-duplex-loop-recording.md) | Единый AUHAL callback, несколько проходов и atomic take commit |
| [32 Loop playback and DAW shell](docs/32-loop-playback-shell.md) | Sample-exact loop transport и более плотная DAW-компоновка |
| [31 Take lanes and comp](docs/31-take-lanes-comp.md) | Отдельные дубли, comp-регионы, armed recording, Undo и format v8 |
| [30 Mixer](docs/30-mixer.md) | Volume, stereo balance, mute, solo, master, smoothing и format v7 |
| [29 Output lifecycle](docs/29-output-lifecycle.md) | Состояния Core Audio, поколения запуска, ошибки callback и silent hardware smoke |
| [28 Crossfades](docs/28-crossfades.md) | Смежные клипы, source handles, linear envelopes, Undo и format v6 |
| [27 Bounded background jobs](docs/27-bounded-background-jobs.md) | Общий worker budget, busy, lifetime и ThreadSanitizer proof |
| [26 Ranges and musical grid](docs/26-ranges-grid-layout.md) | Рабочие зоны UI, диапазонный экспорт, BPM и деления сетки |
| [25 Offline WAV export](docs/25-offline-export.md) | PCM24/float32, общий renderer, фон, прогресс, отмена и atomic publish |
| [24 Crash-safe recording](docs/24-crash-safe-recording.md) | SPSC ring, PCM checkpoints, восстановление после process crash |
| [23 Dry recording](docs/23-dry-recording.md) | Core Audio mono input, Record/Stop, RT-границы и ограничения проверки |
| [22 Clip operations](docs/22-clip-operations-fades.md) | Copy/Delete, snap, fades, формат v5 и проверка |
| [21 Split clips](docs/21-split-clips.md) | Split, несколько регионов, формат v4 и результаты проверки |
| [20 Background storage](docs/20-background-storage.md) | Фоновая запись, резервные копии и crash-тесты |
| [19 Clip editing](docs/19-clip-editing.md) | Move/trim/Undo, формат v3 и результаты проверки |
| [18 Waveform slice](docs/18-waveform-slice.md) | Waveform preview, общий курсор, seek и клавиатура |
| [17 Audio slice](docs/17-audio-slice.md) | Импорт WAV, Core Audio playback, embedded media и проверенные ограничения |
| [16 Implementation](docs/16-implementation.md) | Рабочий прототип, сборка, проверенные сценарии и временный формат |
| [01 Product](docs/01-product.md) | Видение, аудитория, дизайн, идеи и исследование пользовательских проблем |
| [02 MVP](docs/02-mvp.md) | Сценарии, требования, исключения, состояния ошибок |
| [03 Stack](docs/03-stack.md) | Матрица решений, версии, зависимости, ограничения |
| [04 Architecture](docs/04-architecture.md) | Процессы, потоки, жизненный цикл команды, границы модулей |
| [05 Audio engine](docs/05-audio-engine.md) | Время, буферы, граф, мониторинг, PDC, запись и экспорт |
| [06 Project format](docs/06-project-format.md) | Сохранение, транзакции, восстановление, переносимость |
| [07 Extensions](docs/07-extensions.md) | Workflow SDK, команды, совместимость, разрешения |
| [08 Plug-in hosting](docs/08-plugin-hosting.md) | AU/VST3, сканер, состояние, падения, совместимость |
| [09 AI and macOS](docs/09-ai-and-macos.md) | ИИ, локальность, модели, жесты, Shortcuts и Siri |
| [10 Quality](docs/10-quality.md) | Производительность, DSP, реальные устройства, crash-тесты |
| [11 Roadmap](docs/11-roadmap.md) | Этапы, backlog, зависимости, оценки, риски |
| [12 Development](docs/12-development.md) | Подготовка окружения, будущая структура исходников, первый slice |
| [13 Open source](docs/13-open-source.md) | Лицензии, управление, распространение, устойчивость проекта |
| [14 Research](docs/14-research.md) | Конкуренты, источники, ограничения исследования |
| [15 DAW Atlas](docs/15-daw-atlas.md) | 50 DAW и смежных музыкальных сред, сильные стороны и подтверждённые стеки |
| [ADR](docs/adr/README.md) | Реестр архитектурных решений и условий пересмотра |
| [Контракты](specs/README.md) | JSON Schema, SQL и согласованные примеры |
| [Источники](docs/sources.md) | Первичные источники и применимость |

## Проверка подготовленных материалов

Из корня репозитория:

```sh
python3 scripts/check_docs.py
python3 scripts/doctor.py
```

Первая команда проверяет локальные ссылки и примеры контрактов. Вторая читает состояние инструментов разработки и ничего не устанавливает. Приложение собирается и запускается через `./script/build_and_run.sh`; Run action в Codex вызывает тот же entrypoint. Низкоуровневая bundle-сборка остаётся в `./scripts/build-macos.sh`, ядро — через CMake presets. Swift Package не создавался, поэтому `swift build` здесь не используется.

Полная проверка JSON Schema и пересборка HTML:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements-docs.txt
.venv/bin/python scripts/check_docs.py --require-schemas
.venv/bin/python scripts/build_handbook.py
```

Python-пакеты используются только для документации. Готовый HTML не требует Python или этих зависимостей для чтения.

Исследование актуализировано 14 сентября 2026 года. Версии библиотек нужно фиксировать при фактическом подключении; `latest`, `master` и `develop` не являются lock-файлом.
