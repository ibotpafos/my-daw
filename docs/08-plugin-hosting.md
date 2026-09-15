# Хостинг аудиоплагинов

## Очередность

S0 использует собственные DSP nodes. S1 поддерживает ограниченную проверенную матрицу arm64 AU effects. VST3 добавляется после AU lifecycle и восстановления. CLAP, ARA-host и инструменты — следующие независимые проекты. AAX не входит в план хоста.

Текущий VST3 SDK опубликован под MIT; старое описание «GPL или Steinberg proprietary» неприменимо к нынешнему snapshot.[^1] CLAP — открытый API с MIT license.[^2] ARA SDK имеет Apache-2.0, но ARA — дополнительный random-access integration protocol, а не замена обычному audio processing.[^3]

## Обязательный lifecycle

Discovery → scan → instantiate → configure buses/sample rate/max frames → prepare → activate → process → deactivate → state capture → destroy. Обработать отказ на каждом шаге. UI editor открывается/закрывается отдельно; отсутствие editor не означает отсутствие processor.

Catalog хранит format/component ID, vendor, version, architecture, file fingerprint, scan result и причину quarantine. Scan проходит в отдельном disposable process с timeout, а не при запуске на main thread. Quarantine можно пересканировать явно после обновления.

## Isolation

Apple описывает out-of-process hosting AU и возможность управления режимом; покрытие зависит от AU version и выбранного API.[^4] Не переносить утверждение про AUv3 автоматически на все AUv2. Точный путь измерить на target OS, плагине и architecture. Не включать Rosetta/Intel bridge в baseline.

Для первого собственного helper-пути допустим фиксированный pipeline в один блок: host отправляет block N и получает готовый output на следующий цикл. Это добавляет минимум один блок и должно входить в PDC. Никакого неблокирующего «волшебного IPC с нулевой задержкой».

Выбранный режим хранится у каждого insert: `InProcess` или `OutOfProcess`. Он сохраняется для track, bus и master chains; проекты форматов до v15 открываются с историческим значением `InProcess`.

В [1.26.0](53-async-auv3-hosting.md) AUv3 получил строгий системный OOP path. Подготовка graph выполняется в bounded background job, а созданный instance принимается только когда `kAudioUnitProperty_LoadedOutOfProcess` подтверждает фактическое размещение. Generic parameter editor для такого insert выключен до появления remote state proxy. Это не расширяет capability AUv2 или VST3.

Если системный AU host обслуживает render иначе, его timing и failure behavior измеряются отдельно. Синхронный render API стороннего host может нарушить deadline при зависшем plugin; watchdog после факта не превращает его в гарантированно bounded execution.

Для live monitoring baseline допускает встроенные эффекты и только проверенный low-latency AU path. Режим защиты и добавленная задержка видны. Не объявлять все сторонние плагины безопасными для записи.

## Падение и зависание

Worker перестал выдавать output: для affected node — заранее определённая тишина с коротким ramp, статус error, сохранённый checkpoint state. Автоматический bypass может резко изменить громкость/спектр, поэтому политика восстановления задаётся явно. Пользователь может bypass/reload. Запись сухого входа должна продолжаться, если process topology позволяет; это проверяется fault injection.

Crash scanner не добавляет plugin в рабочую матрицу. Crash main app при in-process plugin остаётся риском; такой путь может существовать только как явно ограниченный internal development mode, не как доказанная isolation.

## State, automation и compensation

State snapshot получать на разрешённом API thread и безопасной фазе. Не все plugins допускают state capture одновременно с render; для альфы допустима сохранённая последняя безопасная версия плюс индикатор pending. Сохранение должно сообщать, если актуальное состояние не удалось получить.

Stable parameter IDs отделены от индексов UI. Automation read/write сопоставляет их после reopen. Dynamic latency/bus changes проходят control reconfiguration. В VST3 нужно учитывать process context и `getLatencySamples`, а не только аудиобуферы.[^5]

## Матрица приёмки

Набор: системный AU effect, простой открытый AU, коммерческий EQ, compressor с lookahead, reverb с хвостом, plugin с sidechain, plugin с resizing editor. Коммерческие плагины тестируются только на установленных/доступных лицензиях; их имена и версии фиксируются в отчёте.

Для каждого: scan/timeout, mono/stereo layouts, 44.1/48 kHz, 64/128/256 frames, open/close editor 50 раз, parameter automation, save/reopen, missing plugin, latency change, offline/realtime export, crash/hang, suspend/resume. Поддержка считается по строкам матрицы, не одним флагом «AU есть».

[^1]: Steinberg, [VST3 SDK](https://github.com/steinbergmedia/vst3sdk).
[^2]: Free Audio, [CLAP](https://github.com/free-audio/clap).
[^3]: Celemony, [ARA SDK](https://github.com/Celemony/ARA_SDK).
[^4]: Apple, [Incorporating Audio Effects and Instruments](https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments), [AUAudioUnit](https://developer.apple.com/documentation/audiotoolbox/auaudiounit).
[^5]: Steinberg, [Processing](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html).
