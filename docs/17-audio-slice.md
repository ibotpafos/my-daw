# Аудиосрез — 0.2.0

> История среза 0.2.0. В версии 0.3.0 изменены Play/Stop и добавлен seek: [аудиоволна и навигация](18-waveform-slice.md).

Проверено локально 14 сентября 2026. Прототип теперь импортирует WAV, воспроизводит до восьми аудиодорожек через Core Audio HAL, применяет уровень дорожки и сохраняет аудио внутри экспериментального черновика.

## Пользовательский сценарий

1. «Импорт WAV…» / ⌘I создаёт новую дорожку с одним клипом.
2. «Играть» / ⌘P начинает весь проект с нулевой позиции. Все клипы начинаются одновременно.
3. Ползунок меняет слышимый уровень при отпускании; DSP сглаживает изменение примерно за несколько миллисекунд. −120 dB считается mute.
4. «Стоп» / ⌘. прекращает вывод. Следующий Play снова начинает с нуля; Pause/Resume/Seek пока нет.
5. Сохранение включает дорожки и PCM-аудио. После открытия исходный WAV не требуется.

Если сумма перегружается, выход ограничивается до ±1 и интерфейс просит уменьшить уровни. Это защитный hard clip, **не** музыкальный лимитер и не средство мастеринга. Рендер не выполняет нормализацию импортированного файла.

Импорт — одна команда Undo. Undo/Redo останавливают воспроизведение, затем новый Play использует актуальную модель. Изменение gain во время Play публикуется через lock-free атомики. Переименование и добавление пустой дорожки не перестраивают активный аудиоплан.

## Форматы и лимиты

- RIFF/WAVE, mono/stereo, **44 100 / 48 000 / 88 200 / 96 000 / 192 000 Hz**. Нестандартная для проекта частота приводится к 48 000 Hz вне realtime через системный Apple AudioConverter; 48 kHz использует прямой путь без resampling.
- PCM signed 16/24/32-bit или IEEE float32.
- До 60 секунд на клип, до 32 MiB входной WAV, до 8 аудиодорожек.
- До 64 MiB декодированного PCM в текущем проекте; история удерживает не более 128 MiB уникального PCM (старые snapshots отбрасываются).
- WAV extensible, RF64, compressed WAV, MP3/FLAC и многоканальные файлы пока отклоняются с ошибкой. На non-Apple build частоты, отличные от 48 kHz, также отклоняются: system AudioConverter — macOS adapter, а не новая кроссплатформенная codec dependency.

Это ограниченный полностью RAM-resident прототип. Здесь **нет** disk read-ahead, streaming и больших сессий. Bounded PCM WAV import и import take выполняются отдельной cancelable worker job: чтение, parser, decode и resampling не удерживают control/main thread, а готовый immutable Clip применяет control thread одной revision-aware командой. Audio callback не обращается к диску. Save/export уже используют snapshots, но это не означает общий streaming файловый слой.

## Устройство вывода и RT-контракт

[HAL adapter](../engine/platform/macos/output.cpp) на Play получает текущий default output device, создаёт HAL Output Audio Unit и задаёт client format stereo float32/non-interleaved 48 kHz. Hardware sample rate системно не меняется; преобразование к формату устройства выполняет output unit, если конфигурация поддерживается.

До старта [renderer](../engine/audio/renderer.cpp) получает immutable clips и вычисленные gain targets. Callback выполняет только bounded mixing, smoothing, clamp и atomic telemetry: без Swift/ARC, I/O, mutex, выделения памяти и освобождения shared ownership. Clip ownership удерживает control-side подготовленный план; он меняется после остановки/уничтожения output unit. Общий hot-swap/retirement protocol из проектной архитектуры ещё не реализован.

Telemetry: позиция в client frames, число callback, peak до clamp и число ограниченных кадров. UI опрашивает состояние каждые 100 ms. Проверка устройства выполняется вне RT: отключение/смена default output останавливает вывод и выдаёт ошибку; новый Play выбирает актуальное устройство. Отсутствие прогресса callback более 2 секунд также вызывает остановку. Физическое отключение устройства и полная матрица интерфейсов пока не проверены.

Старт/остановка создают и уничтожают Audio Unit; постоянный engine с независимым transport — будущая оптимизация. Ручной Stop пока без отдельного fade-out, поэтому на ненулевом сигнале возможен щелчок. AU плагины, input monitoring, собственный выбор output и измеренная round-trip latency отсутствуют.

## Черновики v1 → v2

`.mydawdraft` сохраняет прежний application_id, но теперь `user_version = 2`. В таблице `tracks` добавлено nullable `pcm BLOB`: interleaved stereo IEEE float32, little-endian, 48 kHz. Это уже декодированное аудио, не ссылка на файл. Reader проверяет размер, finite samples и предел амплитуды; готовый Clip становится immutable.

Reader поддерживает старый v1 без аудио. Save записывает v2; старое приложение 0.1.0 такой файл не откроет. Для продолжения работы старой сборкой сохранить отдельную копию v1 до записи новой версией. Undo не переносится между запусками. Максимум файла на чтение — 72 MiB. Это по-прежнему временный snapshot-формат, а не production `.mydaw` с recovery journal.

## Доказательства

Локально прошли **три CTest targets** в Debug и ASan/UBSan:

- domain/storage/C bridge и чистый C consumer;
- WAV PCM16/24/32/float32, mono→stereo, ошибочная частота и frame layout, truncated chunks, NaN;
- кодирование PCM и повторное открытие из SQLite, чтение legacy v1;
- gain, smoothing, конец клипа/тишина, clamp, stop, Undo/Redo аудио и лимит аудиодорожек;
- 400 детерминированных повреждённых WAV headers — короткий parser smoke corpus, не длительный fuzzing.

Отдельный hardware smoke на текущем Mac выполнил **3 запуска по 350 ms с тишиной**. Каждый дал 33 callback и 16 896 client frames; после teardown callback counter не изменился. Это наблюдение текущего устройства, не обещание latency/скорости на других устройствах. Команда не включена в общий CI:

```sh
build/debug/daw_hardware_smoke
```

В AppKit проверены импорт 12-секундного тихого синтетического WAV, Play с движущейся позицией и ручной Stop. Затем проект сохранён, приложение перезапущено, исходный WAV перемещён в другое имя. Повторное открытие восстановило 12 секунд аудио, Play снова запустил вывод. В SQLite сохранено 4 608 000 байт PCM. Это доказательство embedded-media roundtrip, не listening acceptance и не power-loss recovery.

Для повторения создать собственный тестовый сигнал:

```sh
python3 scripts/make-audio-fixture.py
```

Скрипт создаёт тихий оригинальный файл `build/Audio check.wav`; он не включён в Git. UI smoke-project лежит в `build/Audio demo.mydawdraft`. Голос, личная музыка или внешние сервисы для этой проверки не использовались.

## Дальше

Приоритет: streaming/recovery и запись. Bounded PCM WAV variable-rate import реализован в [версии 1.35](62-variable-rate-wav-import.md), а фоновая cancelable job и optimistic apply — в [версии 1.36](63-background-wav-import.md). Не считать эти срезы выполнением полноценного S0: B-005/B-006 реализованы только для ограниченного output/RAM playback, SP-01 full-duplex и запись не закрыты.
