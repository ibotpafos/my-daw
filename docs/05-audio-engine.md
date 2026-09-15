# Аудиодвижок

## Временная модель

`FramePosition` — signed int64 в частоте проекта; диапазоны полуоткрытые `[start, end)`. Position и duration не смешиваются с пикселями и секундами double. Pre-roll может иметь отрицательный transport position, но сохранённые клипы S0 имеют неотрицательные позиции. Источник имеет собственную частоту и число кадров; импорт в S0 создаёт media в частоте проекта.

Beat time хранится отдельно для будущего MIDI. В S1 tempo постоянный; beats→frames вычисляются по общей tempo map, а не каждым view отдельно. Все seek/loop/automation события внутри блока имеют frame offset. При loop boundary блок делится на участки; содержимое до границы не повторяется и не пропадает.

## Deadline и формат

Float32 planar buffers, mono/stereo nodes, stereo master. При 48 kHz один буфер 128 frames длится 2.667 ms, 64 — 1.333 ms. Это длительность одного блока, **не** полная round-trip latency. Вход, выход, safety offsets, драйвер, преобразователи и plugins добавляют задержку.[^1]

Поддерживаемый реальный `nframes` читается из callback. Буферы готовятся под объявленный maximum; неожиданное превышение обрабатывается контролируемым отказом/тишиной и счётчиком, а не выходом за границы массива. На reconfiguration engine останавливает I/O, готовит новые ресурсы и только затем возобновляет работу.

## Запреты realtime

Никаких malloc/free/new/delete, file/network/SQLite I/O, mutex waits, sleeps, UI, Swift Tasks, форматирования логов и непредсказуемой ленивой инициализации. Контейнеры заранее резервируются. Атомики должны быть lock-free на целевой архитектуре; это проверяется, не предполагается. Loops ограничены числом nodes/events/frames и конфигурационными лимитами.

Memory pages, DSP tables и plugins прогреваются до play. Профилирование проверяет также скрытые allocations сторонних узлов; собственный тестовый guard не доказывает поведение всех плагинов. Рекомендации VST3 отдельно предупреждают о I/O и allocation в processing.[^2]

## Граф

Для S0 serial topological execution. Узлы: disk source, live input, clip gain/fade, track inserts, pan/gain, summing bus, master, output. Для S1 добавляются sends и latency compensation. Feedback запрещён; topological validation выполняется до публикации.

Прототип 1.9 компилирует main outputs и track sends в фиксированный topological render plan; cycle/missing-target validation выполняется в domain до остановки transport и публикации нового graph. Post-fader tap следует track gain/pan/mute/solo; pre-fader tap обходит gain/pan, но подчиняется общему mute/solo gate. Подробности и ограничения зафиксированы в [routing slice](35-routing-buses-sends.md).

Sends имеют явно pre/post-fader tap; sidechain — отдельный typed input, если включён в поддержанную AU-матрицу. Pan mono — equal-power law, начальный center −3 dB; stereo — balance, без скрытого downmix. Формулы и варианты фиксируются DSP-тестами. Это проектные настройки, не универсальный стандарт звучания.

Непрерывные параметры сглаживаются (начальный default ramp 5 ms); точная длительность параметра задаётся metadata. Automation учитывает sample offset. Самостоятельное сглаживание host не должно дублировать семантику plugin parameter без проверки.

## PDC и мониторинг

Для DAG вычислять cumulative latency по путям. На каждом summing point коротким путям добавлять delay до самого длинного. Учитывать plugin latency, внутренние узлы и явный IPC pipeline delay. Изменение latency требует подготовки delay lines вне RT и безопасного перехода.

PDC выравнивает playback-пути, но не устраняет физическую задержку живого входа. Low-latency monitoring исключает выбранные latency-heavy эффекты только явно, с видимым статусом. Не выдавать uncompensated live monitor за sample-aligned playback.

Позиция записанного клипа учитывает device timestamps и откалиброванную input offset policy. Loopback с импульсом проверяет, что compensation не применена дважды. Значение ручной calibration сохраняется по устройству/частоте; пользователю доступен reset.

## Чтение и запись

Disk read-ahead worker заполняет bounded buffers заранее. При cache underrun RT отдаёт определённую тишину для недоступного участка и увеличивает dropout counter. UI показывает событие; engine не вставляет предыдущий блок под видом нового.

Запись: RT копирует input в preallocated SPSC ring с frame sequence и host timestamp. Writer создаёт recoverable PCM chunk files и sidecar с последним подтверждённым frame count. При переполнении ring запись останавливается с явной ошибкой; потеря данных не скрывается. Размер ring настраивается по памяти и дисковым замерам, начальное планирование — 2 секунды на активный канал.

Media hashing, waveform generation и file header finalization выполняются writer/analysis workers. В UI возможно рисовать предварительные peaks из отдельного bounded канала; они не являются доказательством записи на диск.

## Waveform

Хранить min/max peak pyramid с ключом content hash + algorithm version. Уровни агрегации выбираются по zoom; не читать весь WAV при каждом paint. Для sample-level zoom использовать ограниченное окно исходных samples. Cache удаляем и пересоздаваем; в проектной целостности он не участвует.

## Экспорт

Offline renderer использует тот же graph semantics и frozen session snapshot. Встроенные детерминированные узлы сравниваются с realtime offline harness по заданному float tolerance; сторонние плагины могут быть недетерминированны. Полную битовую идентичность на разных CPU/backend не обещать.

Range задаётся в frames, после него — explicit tail policy (ручной лимит, начальный default 2 секунды). Tail-detect может быть добавлен позже, с верхней границей. Прерывание удаляет/оставляет помеченный временный файл, финальное имя появляется только после успешного закрытия. Offline-несовместимый plugin требует realtime bounce.

При переходе float→PCM применять выбранный TPDF dither один раз на финальном уменьшении разрядности; настройка документируется. Целевая архитектура не нормализует и не ограничивает float export автоматически. Прототип 1.0 для точного совпадения с текущим Play сохраняет его safety clamp `[-1, 1]`; снятие этого ограничения требует вынести защиту из renderer в output adapter и отдельно проверить playback parity. True peak/LUFS — отдельная измерительная функция. EBU R128 — ориентир методики громкости, а не единая обязательная цель для всех музыкальных релизов.[^3]

## Apple Silicon

Сначала однопоточный render plan и измерения. Если введены вспомогательные realtime workers, использовать Audio Workgroups по Apple API. Системный I/O thread уже включён в workgroup; обычным background workers туда входить не нужно.[^4] Не закреплять вручную каждый DSP на «performance core» на основании предположений о scheduler.

GPU/ANE inference выполняется вне callback. Общая память Apple Silicon не делает GPU операции синхронно бесплатными и не гарантирует audio deadline. Тепловой режим, работа от батареи и длительная нагрузка входят в приёмку.

[^1]: Apple, [AudioDeviceIOProc](https://developer.apple.com/documentation/coreaudio/audiodeviceioproc); численные длительности блоков рассчитаны как frames/sampleRate.
[^2]: Steinberg, [Processing](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html).
[^3]: EBU, [R128](https://tech.ebu.ch/publications/r128).
[^4]: Apple, [Understanding Audio Workgroups](https://developer.apple.com/documentation/audiotoolbox/understanding-audio-workgroups).
