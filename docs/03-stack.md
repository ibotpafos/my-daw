# Технологический стек

## Выбор

Базовая архитектура: **Swift + AppKit/SwiftUI → C API → C++20 → Core Audio HAL**. Хранилище и command model живут вне realtime-контура. macOS deployment target 14.0 и arm64 — проектное решение, которое проверяется сборкой и тестами на этой системе, а не выводится из версии компьютера разработчика.

Современность стека определяется управляемыми зависимостями и пригодностью для задач. Новый framework не должен автоматически становиться обязательным для записи.

| Слой | Выбор и применение | Ограничение |
|---|---|---|
| App shell | Swift 6 language mode, AppKit windows/menu/responder, SwiftUI settings | Swift concurrency только вне realtime |
| Timeline | AppKit custom NSView; viewport virtualization и peak pyramid | Metal вводится после spike; accessibility отдельно |
| GPU | MetalKit для крупных волн и визуализаций при доказанной пользе | Не переносим весь DSP на GPU |
| Engine | C++20, собственная domain/C ABI граница; Tracktion adapter для готовых engine services | Нельзя позволять backend types стать публичной моделью продукта |
| Audio I/O | Core Audio HAL через C++/Objective-C++ adapter | Full-duplex одно устройство сначала |
| DSP | Небольшие собственные узлы + Accelerate/vDSP | Оптимизация только после correctness и profiling |
| AU | AudioToolbox adapters | Системная isolation проверяется для каждого пути |
| VST3 | Официальный SDK, позднее | Реализовать hosting lifecycle, UI и state, не только process |
| Session model | C++20 control thread, immutable snapshots | UI не является второй authoritative моделью |
| Storage | SQLite C API, локальный package + media | DB ни разу не вызывается из callback |
| Bridge | C ABI с opaque handles и POD | Не экспортировать STL/Swift types и исключения |
| Background tasks | Swift services / ограниченные workers | CPU/RAM budgets и отмена |
| ML | Core ML как baseline-кандидат; Core AI отдельный availability-gated adapter | Выбор модели и backend после benchmark |
| macOS actions | App Intents для Shortcuts; Siri через отдельную проверку | Не обещать parity с iOS |
| Build | CMake ≥3.28 для C++; Xcode project для app; SPM для Swift packages | Одна точка подключения core, без двойной компиляции |
| Test | CTest + небольшой assertion runner сначала; XCTest/Swift Testing для app | Catch2 только при росте потребности |
| Diagnostics | Instruments, os_signpost вне callback, RT counters | Никакого форматирования логов в callback |

Apple документирует Core Audio callback и правила Audio Workgroups; Accelerate предоставляет DSP-примитивы.[^1] Swift поддерживает C++ interop, но набор поддержанных конструкций развивается, поэтому для публичной границы выбран более узкий C API.[^2]

## Сравнение направлений

Оценки ниже — инженерное суждение для этого проекта, не независимый benchmark.

| Кандидат | Сильная сторона | Цена для проекта | Решение |
|---|---|---|---|
| Native Swift + собственное C++ ядро | Точное управление Mac UX, процессами и форматом | Больше работы над sequencing, PDC и hosting | Reference backend для уже готовых slices и parity tests |
| JUCE + Tracktion Engine | Готовые playback graph, render, recording, plugin и MIDI building blocks | Адаптация доменной модели/UX, отдельные лицензии | Выбран для следующего integration spike за нашей C ABI границей |
| JUCE без Tracktion | Кроссплатформенные I/O, UI и hosting abstractions | Sequencer всё ещё собственный; нативные детали требуют адаптации | Вариант при переходе к Windows как равному приоритету |
| Форк Ardour | Существующий большой DAW-код и GPL-экосистема | Стоимость понимания и преобразования чужой архитектуры/UI | Исследовательский ориентир, не база по умолчанию |
| Rust + CPAL + native shell | Memory safety для большой части кода | CPAL — I/O библиотека, не DAW engine; FFI и hosts всё равно нужны | Возврат к вопросу при команде с Rust audio опытом |
| Electron/Tauri + native engine | Быстрая разработка веб-панелей | Ещё один runtime и мост для высокочастотного canvas/input | Можно для вспомогательного веб-сайта, не основной app shell |
| Swift + AVAudioEngine | Быстрый ограниченный audio proof | Не готовая session/comp/PDC архитектура DAW | Допустим изолированный эксперимент, не вторая production engine |

Tracktion — JUCE-модуль, требует C++20 и лицензируется отдельно от JUCE.[^3] JUCE имеет открытый AGPLv3 и коммерческий варианты; применение одной «open source» метки не решает выбор лицензий.[^4] CPAL предоставляет низкоуровневое кроссплатформенное аудио I/O.[^5]

## Решение после Tracktion proof

SP-06 15 сентября 2026 года собрал pinned Tracktion/JUCE и доказал headless цепочку: два audio tracks, 48 kHz WAV import, gain, loop state, сохранение edit, повторное открытие и offline render 48 000 frames. Исходник proof занимает 219 строк вместе с CMake; чистый Release configure занял 23,47 с, build — 35,66 с суммарно после одной исправленной compile iteration, executable — 22 824 536 bytes. Это достаточно, чтобы не писать собственные equivalents graph/hosting/PDC до сравнения адаптера.

Нативный Swift/AppKit UI, собственный project format, commands/Undo и C ABI сохраняются. Tracktion не подключён к app target и не меняет пользовательские проекты. Следующий backend slice должен выразить существующую сессию через adapter и сравнить routing/AU hosting с native reference. Promotion блокируют несовпадение семантики Undo/recovery, лишние записи за C ABI и отсутствие выбранной GPL/AGPL или commercial стратегии. Подробности и воспроизводимая команда находятся в [proof](34-tracktion-engine-spike.md) и [ADR-012](adr/012-tracktion-adapter.md).

## Версии и воспроизводимость

На машине подготовки: arm64, macOS 26.6.2, Apple Swift 6.3.3, Apple Clang 21.0.0, CMake 4.4.3, Python 3.14.7. Активны Command Line Tools. `xcodebuild -version` не работает с текущим developer directory. Это наблюдение окружения, не поддерживаемая матрица приложения.

Для первой app-сборки выбрать стабильный полный Xcode, записать version/build/SDK в build manifest, проверить deployment target 14.0. До выбора такого Xcode не выдумывать номер toolchain lock. CMake minimum и language standard в будущем manifest задаются явно.

Третьи зависимости подключать по release tag **и полному commit SHA**, с SHA-256 архива и сохранённым license text. Tracktion/JUCE сейчас существуют только в ignored spike checkout; их exact revisions и source archive hashes записаны в [`dependencies.lock.json`](../dependencies.lock.json), license texts сохранены в [`third_party/notices`](../third_party/notices). `bundledInApp: false` остаётся обязательным до отдельного promotion review.

## Библиотеки по необходимости

Signalsmith Stretch — MIT-кандидат для offline time-stretch; качество на русскоязычном вокале проверяется отдельно.[^6] Форматная конверсия WAV/AIFF сначала через системные API. Не добавлять FFmpeg, libsndfile, MLX, ONNX Runtime, Wasmtime и Python runtime внутрь приложения «на будущее». Для каждого нового runtime нужен реальный сценарий и профиль ресурсов.

Готовая библиотека предпочтительна, когда она закрывает сложный стандартный слой: plugin SDK/hosting, time-stretch, resampling, MIDI, файловый codec или проверенный playback graph. Подключение проходит через наш adapter за C ABI и получает четыре обязательных доказательства: закреплённую версию и hash, совместимую лицензию, parity-тест с текущей семантикой проекта и измерение CPU/RAM/latency. Формат проекта, command/Undo model, realtime contract и основной macOS UX остаются нашими, чтобы смена backend не ломала пользовательские сессии.

Ближайший reuse-порядок: сначала сравнить Tracktion adapter для graph/render/hosting; затем провести отдельные spikes Signalsmith Stretch для offline stretch и libsamplerate/r8brain-кандидатов для sample-rate conversion. Добавлять целый framework ради одной небольшой функции нельзя: системный API или узкий MIT/BSD-компонент имеет меньшую поверхность обновлений и лицензирования.

Core AI уже представлен Apple как on-device framework; он включён в исследование, но минимальная ОС и поддержанные модели должны быть проверены по конкретному SDK до интеграции.[^7] Core ML остаётся кандидатом для более широкого deployment target; источники не доказывают ускорение любой модели на ANE.

[^1]: Apple: [AudioDeviceIOProc](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc), [Audio Workgroups](https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups), [vDSP audio example](https://developer.apple.com/documentation/accelerate/creating-an-audio-unit-extension-using-the-vdsp-library).
[^2]: Swift.org, [Mixing Swift and C++](https://www.swift.org/documentation/cxx-interop/).
[^3]: Tracktion, [Engine repository](https://github.com/Tracktion/tracktion_engine).
[^4]: JUCE, [license](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md).
[^5]: RustAudio, [CPAL](https://github.com/RustAudio/cpal).
[^6]: Signalsmith Audio, [signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch).
[^7]: Apple, [Core AI](https://developer.apple.com/core-ai/).
