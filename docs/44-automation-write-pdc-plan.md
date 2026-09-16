# Automation write and graph PDC plan

Версия 1.17.0 продолжает B-013 двумя связанными частями: рабочими Read/Touch/Latch жестами в mixer UI и runtime-only планом компенсации задержек routing graph.

## Automation gestures

В верхней части mixer находятся общий режим `AUTO Read`, `AUTO Touch` или `AUTO Latch` и ARM picker. Целями могут быть volume и pan дорожки, gain bus или master gain. `Read` оставляет существующую lane управляющей воспроизведением. В Touch/Latch запись начинается только для выбранной ARM-цели и только при физическом движении соответствующего slider.

C boundary разделяет жест на `begin`, упорядоченные `write(frame,value)` и `end`. Domain копит изменения во временном State и делает единственный commit при завершении, поэтому Undo отменяет весь жест. Touch сохраняет фактически записанные точки. Latch добавляет последнее значение в end frame. Пустой или отменённый жест не меняет revision.

Ограничения совпадают с сохранёнными lanes: до 2048 точек, timeline до десяти минут, gain от −120 до +24 dB, pan от −1 до +1. Пока жест открыт, другие document mutations и Undo/Redo отклоняются, чтобы одна revision не смешивала несвязанные команды.

## Runtime graph PDC planner

`GraphLatencyPlan` отделён от сохраняемого project State: latency относится к уже инициализированным runtime effects. Planner принимает node latency дорожек и buses, проходит валидированный DAG от источников к мастеру и для каждой track-main, send и bus-output edge вычисляет `maximum incoming latency - source latency`.

Все stereo delay rings создаются в `Renderer::prepare`. Audio callback выполняет только bounded чтение/запись готовых массивов, без allocation и locks. Offline tail сначала дренирует routing delays, затем последовательную master effect chain. Ограничения planner: максимум 10 секунд на edge/path и общий бюджет 20 секунд stereo delay frames, около 7.3 MiB float storage.

В [1.18.0](45-track-bus-inserts-pdc.md) этот planner подключён к сохраняемым track/bus insert chains: renderer измеряет фактическую latency подготовленных эффектов и применяет рассчитанные задержки на main/send/bus-output edges. [1.19.0](46-plugin-automation-tail-export.md) добавил plug-in parameter automation, а [1.20.0](47-plugin-touch-audible-playhead.md) — запись Touch/Latch этих параметров и realtime audible playhead.

Выполнена только compile/build проверка. QA, запуск приложения, прослушивание и hardware/vendor matrix не выполнялись по текущему режиму разработки.

## Уточнения контракта жестов (проверено e2e)

`tests/e2e/e2e_automation.cpp` прогоняет жесты только через публичный ABI и закрепляет то, что раньше читалось только из `engine/domain/session.cpp`:

- на сессию открыт один жест, поэтому `daw_write_automation_gesture` не принимает `expected_revision`: блокировкой служит сам открытый жест — пока он активен, любая мутация отказывает (`Automation gesture is active`, вложенный `begin` в том числе), а чтения, snapshot и `daw_seek_frame` остаются живыми и ничего не стоят;
- `daw_end_automation_gesture` дополнительно отвергает `end_frame` раньше последнего принятого сэмпла, и отказ `end` (стейл-ревизия или плохой кадр) **оставляет жест открытым** — отменять нужно явно;
- отклонённый сэмпл не двигает курсор упорядоченности жеста, а `cancel` возвращает `void`, то есть канала отказа у отмены нет (пиннуется как «второй cancel не тратит ревизию»);
- тейк, все значения которого уже лежат в линии, всё равно коммитит одну ревизию: молчаливый no-op — свойство точечной `upsert`-команды, а не жеста;
- диапазоны: −120…+24 дБ для линий громкости дорожки, gain шины и master, −1…1 для линии панорамы (то же поле `gain_db`), 2048 точек на линию и 10 минут таймлайна. У четырёх основных линий эти числа — литералы в домене, у линий параметров плагинов те же значения вынесены в именованные константы; в `daw.h` их нет, а баннер ссылается на этот документ.

Сценарий автоматизации параметров вставки отдельным journey не выделен: без загруженного AU эта поверхность сводится к негативам (пустые владельцы, отказ по имени insert, ноль ревизий) и покрыта внутри `e2e_automation`; слышимая дуга с реальным загрузчиком — в `e2e_plugins_au`.
