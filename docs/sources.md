# Реестр первичных источников

Дата обращения к Tracktion/JUCE обновлена 2026-09-15; остальные источники проверены 2026-09-14. Для живых API/repository/product pages дата публикации часто не задана; она не подменяется датой поиска. Архивные документы и alpha/beta объявления отмечены. Источники подтверждают возможности/API/лицензионные тексты, а не качество будущей реализации My DAW.

## Платформа и реализация

| № | Издатель / источник | Что использовано | Ограничение |
|---|---|---|---|
| 1 | Apple, [AudioDeviceIOProc](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc) | Callback Core Audio I/O | API page, implementation spike обязателен |
| 2 | Apple, [Understanding Audio Workgroups](https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups) | Deadline и auxiliary realtime threads | Не обоснование переноса background jobs в RT |
| 3 | Apple, [Incorporating Audio Effects and Instruments](https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments) | AU instantiate, AUv3 process behavior | Различать iOS/macOS и AUv2/AUv3 |
| 4 | Apple, [AUAudioUnit](https://developer.apple.com/documentation/audiotoolbox/auaudiounit) | Hosting APIs и version support | Конкретные режимы проверить физически |
| 5 | Apple, [Debugging Out-of-Process Audio Units](https://developer.apple.com/documentation/audiotoolbox/debugging-out-of-process-audio-units-on-apple-silicon) | Системный hosting service как архитектурный факт | Инструкции изменения SIP не используются |
| 6 | Apple, [vDSP audio unit example](https://developer.apple.com/documentation/accelerate/creating-an-audio-unit-extension-using-the-vdsp-library) | DSP primitives | Не готовый DAW engine |
| 7 | Swift.org, [Mixing Swift and C++](https://www.swift.org/documentation/cxx-interop/) | Возможности и эволюция interop | C ABI — наше проектное решение |
| 8 | Apple, [Handling Trackpad Events](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/EventOverview/HandlingTouchEvents/HandlingTouchEvents.html) | Gesture handling, альтернативные controls | Архив; проверить SDK availability |
| 9 | Apple, [AppKit Event Handling](https://developer.apple.com/documentation/appkit/event-handling) | Нативные input methods | Не «доступ ко всем системным жестам» |
| 10 | Apple, [MTKView](https://developer.apple.com/documentation/metalkit/mtkview) | Кандидат Metal view | Страница динамическая; конкретная интеграция не проверена |
| 11 | Apple, [Core ML](https://developer.apple.com/documentation/coreml) | Локальные модели и compute hardware | Не гарантия ANE для каждой модели |
| 12 | Apple, [Core AI](https://developer.apple.com/core-ai/) | Новый model runtime/tooling | Minimum deployment availability ещё проверить |
| 13 | Apple, [Meet Core AI](https://developer.apple.com/videos/play/wwdc2026/324/), WWDC26 | Model deployment и profiling | Не обязательная зависимость S0 |
| 14 | Apple, [App Intents](https://developer.apple.com/documentation/appintents) | Структурированные app actions | Siri execution зависит от системы |
| 15 | Apple, [App Shortcuts HIG](https://developer.apple.com/design/human-interface-guidelines/app-shortcuts) | Отличие macOS от iOS shortcuts | Не обещать platform parity |
| 16 | Apple, [Notarizing macOS software](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution) | Signing, hardened runtime | Выпуск не выполнялся |

## Engines, форматы и лицензии

| № | Издатель / источник | Что использовано |
|---|---|---|
| 17 | Tracktion, [Engine](https://github.com/Tracktion/tracktion_engine), [features](https://github.com/Tracktion/tracktion_engine/blob/develop/FEATURES.md), [LICENSE](https://github.com/Tracktion/tracktion_engine/blob/develop/LICENSE.md) | C++20/JUCE module без UI; playback/render/record/MIDI/plugin building blocks; отдельное GPL/commercial лицензирование |
| 18 | JUCE, [repository](https://github.com/juce-framework/JUCE), [LICENSE](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md) | Framework, CMake, AGPL/commercial |
| 19 | Steinberg, [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) | Current MIT licensing, SDK scope |
| 20 | Steinberg, [Processing FAQ](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html) | RT restrictions и latency changes |
| 21 | Free Audio, [CLAP](https://github.com/free-audio/clap) | Открытый plugin API, MIT |
| 22 | Celemony, [ARA SDK](https://github.com/Celemony/ARA_SDK) | Random-access extension и Apache-2.0 |
| 23 | Signalsmith Audio, [Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) | MIT time/pitch library candidate |
| 24 | RustAudio, [CPAL](https://github.com/RustAudio/cpal) | I/O альтернатива, не complete DAW |
| 25 | SQLite, [WAL](https://sqlite.org/wal.html) | Sidecars, single writer, durability и FS ограничения |
| 26 | SQLite, [Backup API](https://sqlite.org/backup.html) | Snapshot backup живой базы |
| 27 | SQLite, [Copyright](https://www.sqlite.org/copyright.html) | Public-domain statement |
| 28 | Mozilla, [MPL FAQ](https://www.mozilla.org/en-US/MPL/2.0/FAQ/), обновление 2024-01-30 | File-level copyleft как основа предложения |
| 29 | EBU, [R128](https://tech.ebu.ch/publications/r128) | Методика loudness, не универсальная streaming target |
| 30 | Bitwig, [DAWproject](https://github.com/bitwig/dawproject) | XML/ZIP interchange, не native format |

## Продукты и открытый код

Полный предметный реестр — [50 строк атласа](15-daw-atlas.md): в каждой указаны официальный издатель/репозиторий и точные страницы. Дополнительные архитектурные опоры:

31. Cockos, [REAPER Extensions SDK](https://www.reaper.fm/sdk/plugin/plugin.php) и [ReaScript](https://www.reaper.fm/sdk/reascript/reascript.php), живые SDK pages.
32. Reason Studios, [Rack Extension developer guide 4.1.0](https://developer.reasonstudios.com/documentation/rack-extension-sdk/4.1.0/rack-extension-dev-guide), versioned documentation.
33. Bitwig, [Modern Foundations](https://www.bitwig.com/modern-foundations/) и [hosting manual](https://www.bitwig.com/userguide/latest/vst_plug-in_handling_and_options/), product architecture и режимы hosting.
34. Ableton, [Comping](https://www.ableton.com/en/manual/comping/), Live 12 manual.
35. Ableton, [Introducing Extensions SDK](https://www.ableton.com/en/blog/introducing-extensions-sdk/), 2026-06-02, public beta announcement.
36. Embarcadero/Image-Line, [Delphi case study](https://www.embarcadero.com/case-study/image-line-software-case-study), историческое свидетельство; не current inventory FL Studio.
37. Zrythm, [v2 alpha announcement](https://forum.zrythm.org/t/zrythm-v2-0-0-alpha-1-released/596), 2026-06-01; новый стек, незавершённая alpha.
38. Paul Davis / Ardour, [Mixbus relationship](https://discourse.ardour.org/t/harrison-mixbus32c/109165), 2023-09-11; разработчик объясняет lineage и mixer differences.
39. Fender, [Fender Studio Pro](https://intl.fender.com/products/fender-studio-pro), проверено 2026-09-15; официальный product page и screenshots использованы только как визуальный ориентир arrangement/mixer/browser hierarchy.

## Неустановленные факты

Полные current language/framework inventories закрытых DAW, абсолютные rankings производительности, prevalence пользовательских жалоб, готовность перейти на новый продукт, точная стоимость команды и качество моделей на вокале не установлены. Их нельзя заполнять предположениями ради полной таблицы. Следующая проверка — реальные сравнительные workflows и M0 spikes.
