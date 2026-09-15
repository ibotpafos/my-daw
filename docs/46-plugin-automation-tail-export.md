# Plug-in parameter automation and tail export

Версия 1.19.0 продолжает B-013 и export hardening. Параметры Audio Unit/VST3 на track, bus и master получают сохраняемые normalized automation lanes, которые renderer воспроизводит внутри audio blocks. WAV export теперь учитывает finite VST3 tail сверх компенсации graph latency.

## Durable automation model

Каждый `PluginInsert` хранит ordered lanes по стабильному `parameterID`. Lane содержит сохранённое display name и точки `frame/normalizedValue`; normalized диапазон 0…1 не зависит от vendor unit и одинаков для AU/VST3. Пара `(plugin ID, parameter ID)` остаётся целью после перестановки insert внутри chain. Удаление insert удаляет принадлежащие ему lanes вместе с ним.

Domain допускает до 128 lanes на insert, 2048 строго упорядоченных точек на lane и 65 536 plug-in automation points на проект. Timeline ограничена десятью минутами при 48 kHz. Upsert существующего значения остаётся no-op; добавление, изменение и удаление используют expected revision и одну обычную Undo revision.

SQLite format v14 добавляет `plugin_parameter_automation`: plugin ID, parameter ID, стабильные lane/point positions, frame, normalized value и name. Reader v1–v13 создаёт пустые lanes. Существующие opaque plug-in states и `channel_plugins` format v13 остаются без изменения.

## Runtime delivery

Renderer копирует lanes в подготовленные effect slots на control thread. Для каждого bounded block он вычисляет linear normalized curve по project timeline и передаёт parameter changes до обработки соответствующей track, bus или master chain. Переход через loop boundary сбрасывает курсоры lane и снова читает правильный timeline segment.

VST3 получает `IParameterChanges` с sample offsets внутри текущего process block. Audio Unit получает scheduled parameter events с offsets, когда component поддерживает стандартный AU scheduling contract. Callback использует только заранее выделенные buffers и bounded arrays, не выделяет память и не берёт locks.

Missing или bypassed insert не блокирует playback: lane остаётся в проекте, а runtime продолжает dry path. Редактор использует generic catalog parameter metadata и сохраняет только writable параметры.

## macOS workflow

В generic parameter row показан `AUTO n`. Диалог перечисляет точки lane, позволяет записать текущее значение параметра в позиции transport и удалить выбранную точку. Один и тот же workflow доступен в track, bus и master insert panels; значения в списке отображаются как normalized 0…1, чтобы проект не зависел от локальной строки единиц vendor plug-in.

## Finite tail export

Renderer кэширует finite tail активных prepared VST3 effects. Для последовательной chain tail складывается с последующими участками пути; для параллельных track/bus routes выбирается максимальный путь до master. VST3 infinite-tail sentinel не создаёт бесконечный job: с 1.33 его происхождение сохраняется до export policy, где пользователь выбирает automatic bounded tail, исключение infinite component либо конечный предел для него. Текущий Audio Unit host не получает сопоставимую декларацию tail и публикует для AU ноль.

Range export сначала обрабатывает нужный timeline и компенсирует graph latency, затем подаёт тишину через весь graph на объявленную finite tail. WAV header и progress total включают appended tail frames, поэтому reverb/delay decay не обрезается концом выбранного диапазона. Сам requested musical range остаётся исходной частью файла перед tail.

## DAWproject и границы

Экспорт связывает lanes с parameter ID соответствующего device на его Channel и сохраняет normalized Points. Импорт совместимости в сторонние DAW остаётся отдельной matrix: разные hosts могут по-разному сопоставлять vendor parameter IDs и curves.

Runtime effects пока загружаются в процесс приложения. [1.20.0](47-plugin-touch-audible-playhead.md) добавил realtime playhead offset и Touch/Latch gestures непосредственно для plug-in controls. [1.33.0](60-infinite-tail-export-policy.md) добавляет user policy для infinite tail. Следующие hardening-срезы: vendor editors, Audio Unit tail declaration, hardware presentation timestamp и physical/vendor compatibility matrix.

Для продолжения 1.33 Debug build и полный CTest проходят 8/8; option-aware bridge path проверен реальным WAV без plug-in tail. Сборка macOS запускается, но tail dialog ещё требует повторного visual/VoiceOver smoke после таймаута Accessibility backend. Прослушивание, hardware и сторонние vendor plugins не проверены.
