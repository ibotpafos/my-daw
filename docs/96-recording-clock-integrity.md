# Защита временной шкалы записи

## Задача и границы

Независимый срез #27 после ordinary duplex PR #26: не склеивать
несмежные аудиоблоки в запись с правдоподобной, но неверной длительностью.
Используется существующий `DuplexCapture`, а не второй recording path.

Это проверка **callback/request clock AUHAL** — временной метки, с которой
адаптер вызывает `AudioUnitRender`. Она не измеряет отдельную акустическую
input/output задержку, не исправляет latency и не закрывает P0-04 целиком.
Задержки устройства/стримов, safety offsets, round-trip calibration и точное
акустическое размещение по-прежнему требуют отдельной реализации/приёмки.

## Контракт

- Каждый непустой блок до лимита capture требует валидную конечную
  `sampleTime`. Первое значение может быть отрицательным или дробным; это
  абсолютное устройство-время, не позиция клипа.
- Следующее значение сравнивается с первым плюс общее число принятых
  кадров. Допуск 0.001 сэмпла не накапливается между блоками. Пропуск или
  повтор даже одного сэмпла — ошибка. Блоки переменной длины допустимы.
- `hostTime` проверяется только с valid flag. Если он присутствует, обязан
  увеличиваться относительно последнего валидного hostTime. Отсутствие
  опционального hostTime не сбрасывает baseline. Host-only timestamp не
  заменяет sampleTime и не создаёт ложную гарантию непрерывности.
- Поля без соответствующего флага не читаются в HAL-адаптере. Нулевая длина
  не меняет clock; после намеренного лимита поздние callbacks глушатся без
  новой ошибки. Чрезмерные/нечисловые sampleTime отвергаются до HAL render.
- Проверка общая для обычной записи, take, pre-roll и loop. При переходе
  границы loop музыкальная позиция оборачивается, аппаратные timestamps нет.

## Отказ и восстановление

При clock fault текущий и все последующие блоки дают тишину, не меняют
writer/capture cursor и не могут возобновить тот же take. Backing и MON
глушатся вместе. Причина публикуется lock-free atomic; audio callback не
ждёт диск, не создаёт строки/объекты и не останавливает AudioUnit.

Control-thread `checkDevices()` останавливает устройство и передаёт
конкретную причину в существующий немодальный UI failure/recovery path.
`finish()` также проверяет latched error **после остановки writer**:
прямой Stop до следующего poll не выдаёт аварийную запись за успешную.

Подтверждённый непрерывный prefix остаётся в прежнем recovery-формате.
Восстановление — явная существующая команда с одним Undo и Save/Open,
а не автоматическое включение непроверенных кадров в проект.
При ошибке до первого принятого кадра восстанавливать нечего.
Новый capture получает новый clock; неисправный capture не переиспользуется.

## Проверки

`recording_clock`: sample/host flags, NaN/Inf/bounds, отрицательные и дробные
начала, 4000 наблюдений блоков 1/64/257/4096, gap/overlap, host reversal,
отсутствующие timestamps, zero frames и отсутствие накопления погрешности.

`duplex_capture`: реальный renderer/writer, pre-roll и loop, немедленная
тишина, отсутствие лишних PCM и ложного cursor advance, отказ finish,
побайтовая проверка dry prefix. Сохранены прежние partition/MON/cap tests.

`e2e_duplex_recording`: production C ABI и recovery → WAV/SQLite → Undo/Redo →
reopen; при clock fault Stop проверяется и до, и после status poll.
Только аппаратный factory подменён при линковке отдельного теста.

`bash scripts/test-recording-ui.sh`: настоящий DraftApp/NSButton/поля статуса,
ошибка gap/missing timestamp, возврат controls и явное восстановление.
Готовность macOS и точные результаты фиксируются по CI конкретного PR/head,
а не по одному наличию теста. Физические драйверы и VoiceOver остаются
отдельной приёмкой. ABI и формат проекта не меняются; зависимостей нет.

## Системные API

Переиспользуются Core Audio `AudioTimeStamp` и его validity flags,
стандартные C++20 `std::isfinite`/`std::atomic` и прежний writer/renderer.

- [AudioTimeStamp.mSampleTime](https://developer.apple.com/documentation/coreaudiotypes/audiotimestamp/msampletime)
- [AudioTimeStamp.mFlags](https://developer.apple.com/documentation/coreaudiotypes/audiotimestamp/mflags)
- [Device input using AUHAL (TN2091)](https://developer.apple.com/library/archive/technotes/tn2091/_index.html)

Наличие непрерывных request timestamps не доказывает отсутствие ошибки
внутри стороннего драйвера, качество физического clock или совпадение
микрофонного транзиента с акустической подложкой. Никакой silence-insertion,
ресемплинг или автоматический сдвиг записанного клипа здесь не выполняется.
