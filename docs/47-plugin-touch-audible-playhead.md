# Plug-in Touch/Latch and audible playhead

Версия 1.20.0 завершает следующий B-013 control-flow slice: параметр Audio Unit/VST3 можно вооружить прямо в generic editor и записать одним Touch/Latch жестом, а transport показывает позицию сигнала после компенсации graph latency. Durable формат остаётся v14.

## Один жест — одна revision

Plug-in gesture адресуется owner kind, owner ID, глобальным plugin ID и vendor parameter ID. Begin проверяет существование insert и writable automation target, затем создаёт приватную копию State. Ordered writes добавляют normalized точки 0…1; Touch фиксирует только фактическое движение, Latch также удерживает последнее значение до end frame. End делает один commit и одну Undo revision, пустой или отменённый жест не меняет проект.

Session допускает только один открытый automation gesture: fader или plug-in parameter. Пока он активен, несвязанные mutations, Undo и Redo отклоняются. Timeline и размер lanes используют существующие bounds format v14.

## Живое управление

Control thread публикует единственный активный parameter override в lock-free atomics renderer. Audio callback сопоставляет глобальный plugin ID с подготовленным effect slot, подавляет сохранённую lane только для этого параметра и отправляет текущее normalized значение с offset 0. Остальные lanes продолжают воспроизводиться. Callback не выделяет память и не берёт locks.

После end/cancel override снимается. Завершённая lane попадает в новый prepared graph; если transport играл, output пересоздаётся и автоматически продолжает работу с воспринимаемой позиции. Это сохраняет единый immutable prepared plan, не разрешая UI изменять effect objects параллельно callback.

## macOS workflow

Параметры в master, track и bus editors используют continuous `AutomationSlider`. Рядом находятся `ARM` и `AUTO n`. При `AUTO Read` slider меняет static plug-in state как раньше. При Touch/Latch пишет только вооружённый параметр: mouse-down начинает жест, движения добавляют точки в текущей позиции transport, mouse-up завершает его. ARM state сохраняется по stable owner/plugin/parameter tuple, пока соответствующий insert существует.

## Audible transport position

Renderer отдельно хранит input cursor и audible position. Воспринимаемая позиция вычисляется от playback start и числа обработанных frames минус суммарная graph latency. До заполнения latency pipeline она остаётся на start frame; внутри loop вычитание корректно оборачивается в loop range. C ABI публикует эту позицию как transport frame и использует её при Stop, изменении loop и автоматическом restart после automation commit.

Значение учитывает DSP/PDC graph latency, но пока не включает отдельную hardware presentation latency Core Audio device. Diagnostic `output_latency_frames` продолжает показывать известную graph latency.

## Оставшиеся границы

Runtime plug-ins всё ещё выполняются в процессе приложения. Дальше нужны process isolation, vendor editors, Audio Unit/infinite tail policy, hardware presentation timestamp и compatibility matrix. В этом срезе выполняется только compile/build проверка; QA, запуск приложения, прослушивание, hardware и сторонние vendor plugins не выполняются по текущему режиму разработки.
