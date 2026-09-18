# Аппаратная частота и аудиобуфер — P0-02

## Пользовательский путь и граница

«My DAW → Настройки аудио…» → **Частота / буфер входа…** или
**Частота / буфер выхода…**. Панель показывает актуальную nominal rate и buffer,
допустимый диапазон кадров и принимает выбор либо ручной ввод целого размера.
Открытие панели и «Обновить» только читают HAL; запись и микрофон не запускаются.

Проект пока **48 000 Hz**. Единственный целевой аппаратный режим этого среза —
48 kHz; поддерживаемые диапазоны частот читаются из HAL и проверяются, но другие
проектные частоты и скрытый ресемплинг не добавлены. Буфер — 1…4096 кадров и только
в границах, сообщённых устройством. Предложения степеней двойки не являются
обещанием их поддержки: окончательное решение принимает драйвер. Допустим
произвольный целый размер внутри диапазона, если устройство его подтверждает.

Кнопка явного применения меняет **физическое устройство**, а не проект. Вход и
выход одного интерфейса могут иметь общую частоту/буфер, и изменение влияет на
другие приложения. При разных UID каждое устройство настраивается отдельно;
неявного создания aggregate и синхронизации независимых clocks нет. Выбранное
«Системное устройство по умолчанию» разрешается в конкретный UID при открытии
панели; смена OS default не перенаправляет уже открытый редактор.

## Подтверждение вместо оптимистичного успеха

`AudioDeviceChange` — ограниченная control-thread операция с общим двухсекундным
сроком. Перед первой записью проверяются **обе** цели, стабильный UID/ID,
доступность, settable flags и отсутствие `DeviceIsRunningSomewhere`.

1. При необходимости отправляется nominal rate. Обработчик HAL property listener
   сообщает уведомление через атомарный счётчик; UI не вызывается из listener.
2. Только после уведомления **и** совпадающего readback частоты заново проверяются
   range/settable буфера: драйвер мог изменить их вместе с частотой.
3. При необходимости отправляется buffer. Success требует уведомления и
   совпадающего повторного чтения **обоих** значений. Неизменяемые значения не
   записываются: подтверждённый read-only no-op разрешён без ожидания уведомления.

Возврат `AudioObjectSetPropertyData` сам по себе не означает успеха. Потеря
устройства, смена UID/ID, запуск hardware другим приложением, несовпадающий ответ,
ошибка setter/listener или таймаут дают явную ошибку. Уже видимый requested value
без HAL notification не считается подтверждением. Poll не спит и не запускает
audio. Таймер UI работает через 50 ms в main run loop; engine стартует только
через существующий путь после завершения операции.

Срок ограничивает ожидание между HAL-вызовами, **не** способен прервать зависший
внутри системного вызова драйвер или преодолеть остановку main run loop.
Позднее подтверждение не превращает завершённый timeout в success. Если первый
setter прошёл, а второй нет, железо может остаться изменённым частично. При ошибке
setter текущие значения помечаются неизвестными до нового чтения. Автоматического
rollback/retry/default fallback нет: обратная запись также асинхронна и могла бы
перетереть внешнее изменение. Панель показывает ошибку и перечитывает состояние;
перед повтором пользователь сверяет фактические параметры.

## Интеграция и безопасность

Три versioned C ABI операции: `daw_get_audio_device_capabilities`,
`daw_begin_audio_device_change`, `daw_poll_audio_device_change`. Struct size,
version, bounded UID, конечные числа и frame limits проверяются до записи.
Снимок caps содержит до 64 диапазонов частот, текущие значения, range буфера,
settable/running flags. Poll возвращает Idle/Pending/Applied/Failed, отдельно
actual-known, possible-partial-change и текст ошибки.

Начало запрещено при playback preparation, Play, normal/loop Record и MIDI
capture; остановленный output unit освобождается перед изменением. Pending
операция блокирует новые Play/Record/MIDI capture. Одновременно выполняется одна
операция на процесс; её состояние переживает New/Open/destroy проектной сессии.
Последний терминальный результат доступен до следующего принятого изменения.
Существующий owner-thread контракт C ABI остаётся обязательным.

Ни model revision, ни Undo, ни project format, ни audio graph/DSP не меняются.
Hardware format не записывается в `audio.devices.v1` и не восстанавливается
автоматически при New/Open/restart. Сохранение UID/channel intent по-прежнему не
пишет HAL и не меняет UID текущей format-операции: новый проект может восстановить
машинные preferences без скрытого перехода на default. Внешнее изменение hardware
после подтверждения по-прежнему проверяется существующим runtime device guard.

В UI pending отключает Apply/Refresh/ввод и закрытие дочерней панели; повторное
нажатие не отправляет второй setter. `NSControl.validateEditing()` фиксирует
реальный combo field editor перед чтением значения, включая непосредственный
клик Apply. Ошибки не подтверждают устаревшее значение и не меняют musical state.
Listener не имеет указателя на уничтожаемый controller: его атомарное хранилище
живёт весь процесс. Удаление listeners выполняется на control thread; поздний
queued callback не обращается к освобождённой памяти. Audio callback не получает
mutex/allocation/file I/O.

## Автоматические проверки

- `audio_hardware_acknowledgement`: production state machine с явно внедрённым
  fake HAL adapter. Асинхронные readback/notification в разном порядке, два
  setter, no-op, read-only, caps/UID/числовые ошибки, изменение диапазона после
  rate, запуск/отключение устройства, setter failure, общий timeout/late ack.
- `e2e_audio_hardware_settings`: настоящий C ABI, malformed arguments, explicit
  absent UID без fallback, отсутствие phantom operation, сохранность revision,
  Save/Open и interlock подготовки playback. Ни один тест не пишет реальное
  устройство. Положительный HAL setter этим E2E не доказывается.
- `audio_hardware_format_tests.swift`: Foundation-only валидация формы.
- `workspace_audio_hardware_tests.swift`: настоящий AppKit controller, реальный
  combo field editor/Apply, pending/terminal/error states, missing/running
  hardware fixture, parent-button/default UID freeze, production bridge helpers
  для rejected requests, геометрия и PNG нативного view.

C++ CTests входят в обычную core Linux/macOS sanitizer матрицу. Swift/AppKit
входят в существующий `scripts/test-workspace-ui.sh`; новые Swift файлы включены
в production `sources.txt`, полный SDK typecheck и сборку приложения. Фактически
пройденные запуски и exact SHA/tree фиксируются в PR, не предполагаются документом.

## Физическая приёмка остаётся обязательной

На реальном Mac/интерфейсе записать build SHA, macOS/driver, UID, исходные и целевые
параметры. Закрыть другие аудиоприложения; начать с низкой громкости, MON выключен.
Проверить 44.1→48 kHz и буферы 64/128/256/512 в доступном диапазоне: UI, Audio MIDI
Setup/vendor panel и последующий Play/Record должны согласоваться. Проверить
read-only/unsupported значение, внешнее изменение, disconnect/reconnect,
сбой/timeout/частичное изменение и отсутствие скрытого fallback. Проверить New/
Open, оба направления одного интерфейса, клавиатуру и VoiceOver. Физический звук,
latency и устойчивость драйверов не подтверждаются mock, оффскрин UI или CI.

## Переиспользование и первоисточники

Существующие device UID routing, bridge guards, AppKit controls/field editor,
Foundation Timer и системный Core Audio HAL. Новых библиотек, собственного DSP,
AVAudioEngine-замены и изменений deployment target нет. Используемые C HAL API
доступны целевому macOS 14+; фактические SDK/Swift версии сохраняет native CI.

- [AudioObjectSetPropertyData](https://developer.apple.com/documentation/coreaudio/audioobjectsetpropertydata(_:_:_:_:_:_:))
- [Асинхронная семантика HAL setter](https://developer.apple.com/documentation/coreaudio/audiohardwareobject/setpropertydata(address:qualifier:data:)-4kzgv)
- [AvailableNominalSampleRates](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertyavailablenominalsamplerates)
- [BufferFrameSizeRange](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertybufferframesizerange)
- [DeviceIsRunningSomewhere](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertydeviceisrunningsomewhere)
- [NSControl.validateEditing](https://developer.apple.com/documentation/appkit/nscontrol/validateediting())
