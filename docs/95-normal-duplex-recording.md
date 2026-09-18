# Обычная full-duplex запись

## Срез P0-03

Продолжение [core-очереди](93-core-readiness.md), implementation issue #23.
Обычная запись в новую дорожку и обычный дубль существующей дорожки теперь
используют тот же AUHAL full-duplex путь, что и loop takes. Input-only fallback
в приложении удалён: ошибка открытия выхода не превращается в «успешную» запись
без фонограммы. Существующий low-level input/probe API не удалён.

## Пользовательский путь

Выбрать один интерфейс (либо заранее настроенный aggregate) для входа и выхода
в [настройках аудио](92-audio-device-selection.md), совместимые 48 kHz и буфер
до 4096. Выбрать канал микрофона и выходную пару; надеть наушники и начать
с низкой громкости. Установить курсор, при необходимости преролл, затем Record.
Без ARM создаётся новая аудиодорожка; с ARM — дубль выбранной дорожки.

Проект воспроизводится с начала преролла. Захват начинается точно после
`min(preroll, startFrame)` кадров callback, включая границу внутри аудиоблока.
Эти подготовительные кадры не записываются. Курсор интерфейса берётся из
renderer transport, а не из числа записанных кадров; во время преролла он
движется до начала тейка. Пустой проект и запись после конца фонограммы
поддерживаются без добавления фиктивной дорожки или клипа.

MON доступен во время обычной и loop-записи, в том числе в преролле. Изменение
применяется на следующем callback через atomic flag, не меняет revision/Undo.
Это **прямой сухой mono input с unity-уровнем в обоих выходах**, добавленный
после project renderer: эффекты, панорама, фейдер дорожки и master не обрабатывают
этот прямой сигнал. Для собственного аппаратного direct monitoring выключить
MON в приложении, чтобы избежать удвоения сигнала. MON сам не открывает микрофон
и вне записи не запускает прослушивание. На диск идут только выбранный вход,
не фонограмма, метроном или сумма MON. Нефинитные значения и чрезмерные пики
обрабатываются существующей capture-политикой (0 для NaN/Inf, clamp ±16).

Record/Stop завершает capture, затем одним штатным project command добавляет
результат. Undo/Redo, Save/Open, recovery и WAV export используют прежние
реализации. Остановка во время преролла без принятых кадров ничего не создаёт
и не добавляет Undo. Решение о пустоте принимается **после остановки callback**,
не по устаревшему счётчику из UI. Checkpoint удаляется только после успешного
добавления тейка. Cancel/ошибка сохраняют принятый префикс для recovery.

Включённый transport cycle делает loop recording только при записи в ARM
дорожку. Новый linear track не делится на passes, его статус не помечается
«Луп-запись». Обычный дубль также не делится; существующий loop path сохраняет
разбиение полных и последних частичных проходов.

## Реализация и ограничения

Переиспользованы `MacDuplex`, `Renderer`, `RecordingWriter` (SPSC и worker),
`addTake`/`addTakes`/`importAt`, нынешний C ABI и AppKit controls. Общая композиция
`prepareDuplexCapture` / `renderDuplexBlock` вызывается реальным AUHAL callback
и тестовым устройством. В render callback не добавлены allocation, locks,
file I/O или destruction. Новых зависимостей, DSP, формата проекта нет.

Renderer получает опциональный конечный `recordingEndFrame`. Только запись
может воспроизводить тишину в пустом проекте/за его концом до этого горизонта;
обычные play/export вызывают прежний путь и по-прежнему отвергают пустой проект.
Горизонт не записывается в проект и не растягивает экспортируемую аранжировку.
Границы timeline/capture не увеличены. Process-local hardware IO lease из P0-02
сохранён, включая весь lifecycle новой обычной записи.

48 kHz, один физический источник, минутный capture, существующие ограничения
числа дорожек/активов/памяти остаются. Независимые устройства с разными clocks
не объединяются автоматически: ошибка предлагает один интерфейс или aggregate.
Программное совпадение границ кадров **не является измерением физического
recording offset/round-trip latency**. Sample/host timestamps и компенсация
входных/выходных задержек — P0-04; stereo/30 минут streaming — P0-05; полная
матрица disk-full/stalls/hotplug — P0-06. Эти пункты не объявляются завершёнными.

## Автоматические проверки

`normal_duplex_recording` — реальный C ABI → production render/writer → команды
проекта → Undo/Redo → Save/Open → независимое чтение WAV. Только executable
этого теста линкует детерминированный device factory вместо HAL; в приложении
нет test environment switch или публичной подмены backend. Проверяются пустой
проект, backing, sample-exact граница преролла и сухой PCM, live MON, курсор,
linear/take/loop, ранний Stop, ошибочный start без fallback, stale revision,
выбранные UID/каналы, recovery и уничтожение. WAV oracle учитывает существующее
сглаживание уровня renderer; сохранённые исходные сэмплы проверяются отдельно.

`workspace_recording_tests.swift` запускается существующим native workspace
runner: настоящий NSButton → action → C ABI, доступность MON при заблокированных
project controls, отсутствие Undo, отдельные статусы normal/take/loop.
Он не запускает физический capture. Прежние CTest/native/SDK suites сохранены.
Фактический статус и exact head/tree каждого прогона фиксируются в PR.

## Ручная приёмка — остаётся открытой

На реальном Mac записать build SHA, macOS, интерфейс, UID/каналы, rate/buffer.
Проверить новую и ARM-дорожку поверх audio/MIDI/AU/VST3 проекта, преролл,
переключение MON в наушниках, Stop до и после границы, 3 loop passes, Undo,
Save/Open/export и прослушивание. Отдельно проверить disconnect/reconnect,
ошибочный выбор двух независимых устройств, задержки и VoiceOver.
Зелёные mock/native CI tests не закрывают этот аппаратный gate из issue #20.

## Первоисточники

- [Apple TN2091: Device input using the HAL Output Audio Unit](https://developer.apple.com/library/archive/technotes/tn2091/_index.html) — существующий AUHAL single-device input/output и channel maps.
- [Apple Core Audio: Common tasks](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/ARoadmaptoCommonTasks/ARoadmaptoCommonTasks.html) — системные audio interfaces вместо собственного backend/DSP.
