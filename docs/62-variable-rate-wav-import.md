# Variable-rate PCM WAV import

Версия 1.35.0 расширяет существующий импорт одной дорожки и import take. На macOS My DAW принимает mono/stereo RIFF PCM WAV с частотой **44 100, 48 000, 88 200, 96 000 или 192 000 Hz** и создаёт тот же канонический Clip, что и прежний 48 kHz import.

Это ограниченный compatibility slice для PCM WAV, а не общий media importer. Он не добавляет AIFF, MP3, FLAC, WAV extensible, RF64, compressed WAV или более двух каналов.

## Поведение

1. Пользователь выбирает WAV через toolbar, меню или Browser Add, либо выбирает WAV для нового take.
2. С [версии 1.36](63-background-wav-import.md) parser проверяет RIFF/WAVE контейнер, PCM16/24/32 или IEEE float32, mono/stereo, разрешённую частоту, размер входного файла не более 32 MiB и исходную длительность не более 60 секунд в отдельной cancellable job до domain command.
3. Для 48 kHz decoded stereo float32 samples становятся Clip без преобразования. Для остальных разрешённых частот macOS adapter вызывает Apple `AudioConverter` вне realtime. Число проектных frames округляется к ближайшему значению; непустой source короче одного project frame сохраняется как один скопированный stereo frame, потому что системный converter может законно не выдать packet для такой длительности.
4. Результат всегда stereo interleaved Float32 48 kHz. Только после успешной конвертации bridge выполняет существующую revision-aware import command; ошибка оставляет revision, Undo и проект без изменения.
5. Storage записывает уже сконвертированный PCM. После save/open или перемещения исходного WAV клип не зависит от исходной частоты или пути файла.

Тот же путь используется для `daw_import_wav` и `daw_import_take_wav`, поэтому placement take, comp и Browser Add не получают отдельной семантики. Конверсия выполняется до transport renderer и Audio HAL callback; audio callback не получает файловый I/O, AudioConverter или allocation из этого среза.

## Канонический формат проекта

| Слой | Контракт |
|---|---|
| Вход | RIFF PCM WAV, mono/stereo, PCM16/24/32 или float32; 44,1 / 48 / 88,2 / 96 / 192 kHz |
| Import result | immutable stereo Float32 Clip, 48 000 frames/sec |
| Storage | embedded interleaved stereo Float32 PCM, 48 kHz; schema не меняется |
| Playback / export | существующий fixed stereo Float32 48 kHz plan |

Проектная частота всё ещё фиксирована на 48 kHz. Conversion не меняет tempo, position, clip-editing rules или публичную форму C ABI. 48 kHz fast path нужен, чтобы старый approved import не менял samples из-за resampling.

## Выбранная библиотека и переносимость

На macOS используется готовый системный `AudioConverter` из AudioToolbox. Он изолирован в platform/import adapter за существующим C++ import boundary: доменная модель, storage и C bridge не получают Apple type в своих публичных контрактах. Это соответствует правилу проекта применять готовую библиотеку для стандартного codec/resampling слоя, сохраняя собственные session и project semantics.

На non-Apple build system AudioConverter недоступен. Такой build принимает только 48 kHz PCM WAV и сообщает точную причину для другой частоты, пока не будет добавлен и доказан отдельный adapter с license, parity и resource review. Нельзя подменять это неявным изменением частоты проекта.

## UI и ошибки

Open panels для импортирования дорожки и take явно показывают: PCM WAV mono/stereo, 44,1 / 48 / 88,2 / 96 / 192 kHz, фоновую автоматическую конвертацию к 48 kHz и лимит 60 секунд. Browser использует тот же engine path, поэтому не обладает скрытым расширенным списком codec или sample rates.

Некорректный контейнер, format, channel layout, частота, duration, размер, truncated data, non-finite float samples или ошибка converter показываются через существующую bridge error surface. Они не создают пустую дорожку, take или частично сохранённый PCM.

## QA и границы доказательств

Code acceptance покрывает fixtures 44,1 / 48 / 88,2 / 96 / 192 kHz, mono и stereo варианты, preservation 48 kHz fast path, expected output frame count, rejected rate/container/layout, failed import без изменения revision и storage save/open converted Clip. Локальный Debug CTest прошёл 9/9, ASan/UBSan CTest — 8/8. Сборка `My DAW.app` 1.35.0 (build 45) имеет arm64 executable и прошла `codesign --verify --deep --strict`.

UI smoke показал новый текст toolbar NSOpenPanel и импортировал через него 0,1-секундный mono WAV 44,1 kHz. Созданный recovery draft содержал один Clip на 4 800 проектных frames и прошёл SQLite integrity check; после проверки приложение закрыто, а временный fixture и его recovery draft удалены. Post-import AX snapshot не получен из-за timeout backend, поэтому визуальное состояние arrangement после импорта и take-panel smoke этим прогоном не доказаны.

Не заявляются прослушивание converted audio, сравнение качества resampling, physical-device latency, VoiceOver audit или non-Apple compatibility. Зелёная сборка не заменяет listening acceptance.

## Следующие границы

- Background/streaming import для больших файлов и медленных дисков.
- WAV extensible/RF64 и дополнительные codecs только после отдельного parser/compatibility contract.
- Сравнение resampler quality на музыкальном материале и physical listening acceptance.
- Вторая platform adapter strategy до обещания import parity за пределами macOS.
