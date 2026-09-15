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
