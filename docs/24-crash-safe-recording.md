# Crash-safe recording — 0.9.0

15 сентября 2026. Dry recording больше не зависит от сохранения всего дубля в памяти до Stop. Core Audio callback передаёт mono-сигнал через заранее выделенный SPSC ring фоновому writer. Writer сохраняет stereo float32 PCM в отдельный `.mydawtake` и каждые 24 000 кадров подтверждает durable checkpoint.

## Поток данных

```text
Core Audio input callback
        │ mono float32, без allocation/file I/O/lock
        ▼
bounded SPSC ring, 2 секунды
        │ один background consumer
        ▼
.mydawtake: header + stereo float32 PCM
        │ fsync data → update confirmed_frames → fsync header
        ▼
Stop: Clip + Undo commit + удалить recovery-файл
Crash: открыть только confirmed_frames
```

Producer и consumer используют монотонные atomic indices. Producer публикует кадры release-store после копирования, consumer читает их acquire-load и освобождает место другим release-store. На arm64 необходимые atomics проверены compile-time как lock-free.

Ring хранит две секунды mono-аудио. Writer выделяет stereo scratch в своём background thread и дублирует mono по двум каналам перед `pwrite`. Если ring, timeline или media budget заполнены, producer фиксирует overflow и больше не принимает кадры. Получается непрерывный префикс записи без скрытой дыры и последующего продолжения.

## Формат recovery take v1

Первые 64 байта содержат magic `MYDAWTAK`, версию, размер header, sample rate 48 000, два канала, стартовый frame и `confirmed_frames`. Дальше идут interleaved stereo float32 samples. Writer сначала синхронизирует PCM, затем меняет подтверждённую длину и снова синхронизирует файл. Reader игнорирует неподтверждённый хвост и отклоняет неизвестный magic/version, пустые и слишком длинные записи.

Файлы создаются с mode `0600` в `Application Support/My DAW/Recording Recovery`. Имя начинается с PID процесса. При обычном Stop все принятые кадры подтверждаются, файл превращается в обычный `Clip`, модель коммитится и recovery-файл удаляется. При ошибке commit или writer подтверждённая часть остаётся на диске.

## Восстановление

При запуске приложение ищет `.mydawtake`, PID которых больше не существует. Пользователь может восстановить последний подтверждённый префикс как новую дорожку `Восстановленная запись`. Позиция на таймлайне читается из header. Импорт создаёт обычную Undo-команду; файл удаляется только после успешного model commit. Нечитаемый файл получает дополнительное расширение `.unreadable`, чтобы не создавать повторяющийся alert при каждом запуске.

Checkpoint выполняется примерно каждые 0,5 секунды. Process crash может потерять только хвост после последнего checkpoint. Обычный Stop подтверждает остаток полностью. Это ещё не обещание сохранности при отказе накопителя, повреждении файловой системы или внезапной потере питания; такие сценарии требуют отдельной матрицы и нескольких файлов/sidecar при необходимости.

## Проверка

Добавлен пятый CTest `recording_crash_recovery`. Он проверяет mono→stereo, NaN sanitization, clamp, bounded overflow, непрерывный prefix, чтение header, позицию региона, C bridge import, удаление после commit и отказ на повреждённом файле.

Process-crash сценарий выполняется отдельным дочерним процессом: он пишет 30 000 кадров, ждёт durable checkpoint и вызывает `_exit` без деструкторов. Родитель открывает оставшийся файл и проверяет старт, подтверждённую длину и samples. Debug и ASan/UBSan должны проходить 5/5.

Микрофон в автоматических тестах не включается. Остаются физический smoke на встроенном и USB-входе, прослушивание, смена sample rate, disconnect во время записи и измерение dropout/latency. Full-duplex playback/monitoring в этот срез не входит.
