# P0-02: явное управление аппаратным форматом

## Область

Следующий core-срез после [выбора устройств](92-audio-device-selection.md),
по [очереди рабочей альфы](93-core-readiness.md). Проект, renderer и storage
по-прежнему работают в **48 kHz**. Это не variable-rate проекты, не новый
ресемплер, не компенсация задержки и не обычная duplex-запись.

«Настройки аудио» → **Формат входа… / Формат выхода…** открывает отдельную
панель для явно определённого UID. Системный default разрешается в конкретный
UID в момент открытия: дальнейшая смена default не перенаправляет запрос.
Видны текущая частота/буфер и диапазон буфера драйвера. Выбор включает обычные
32/64/128/256/512/1024/2048/4096, границы диапазона и текущее нестандартное
значение, только в пределах возможностей и лимита движка. HAL сообщает
диапазон, не обязательный дискретный список: каждое значение всё равно
требует подтверждения. Не обещаем работу каждого числа внутри диапазона.

Кнопка **Применить к устройству** явно запрашивает 48 kHz и выбранный буфер.
Она не назначена Return-действием по умолчанию. Это изменение оборудования,
которое может затронуть другие приложения. При одном duplex-интерфейсе
формат общий для входа/выхода. Разные устройства меняются отдельно; общей
атомарной перенастройки двух независимых clocks нет.

## Реализация и безопасность

`AudioHardwareBackend` — небольшой адаптер системного Core Audio, а не свой
драйвер. Считываются AvailableNominalSampleRates, BufferFrameSizeRange и
IsPropertySettable. Запись — AudioObjectSetPropertyData. Никаких изменений
внутри audio callback и новых зависимостей.

Четыре versioned C ABI функции: read capabilities, begin change, poll и release.
Begin проверяет idle состояния, затем запускает ограниченный фоновый worker.
Worker хранит собственный snapshot, не session pointer, поэтому закрытие или
смена проекта не приводят к use-after-free. Output/Input/Duplex держат общий
process-local IO lease; hardware worker — exclusive lease. Нельзя начать
HAL I/O из другой сессии или позднего prepare одновременно с перенастройкой.
Это не lock в callback и не межпроцессная блокировка чужих приложений.

Snapshot привязан к UID, временно захваченному DeviceID, rate и buffer. Перед
записью проверяются актуальные значения и права. Сначала частота, затем
повторное чтение buffer capabilities, затем размер буфера. Успех — три
последовательных чтения запрошенного состояния: частота с допуском 0.5 Hz,
буфер точно. Setter success сам по себе недостаточен.

Каждое ожидание ограничено 100 проверками с 10 ms паузой на worker. UI
опрашивает готовность таймером, не спит и не ждёт join. Это ограничение нашего
polling, не гарантия максимального времени зависшего внутри HAL драйвера.
При закрытии панели release запрашивает отмену; lease остаётся у worker
до завершения cleanup/rollback. Применение не запускается при New/Open,
восстановлении preferences или открытии панели.

При ошибке или отмене после записи выполняется best-effort возврат исходной
частоты/буфера с повторным подтверждением. Устройство не подменяется при
отключении или смене UID. При неожиданном внешнем изменении код не записывает
прежние параметры вслепую. `restored=0` / неизвестное состояние явно требуют
обновления и проверки аппаратной панели. Даже подтверждённое чтение не может
гарантировать, что внешний процесс или поздний драйвер не изменят формат
позже; существующие runtime device checks остаются обязательными.

Project revision, Undo, исходные медиа, device-selection preferences и MON
не изменяются этой операцией. Stopped Output уничтожается до получения
exclusive lease; его музыкальные данные остаются в модели.

## Проверки

- `audio_hardware_transaction`: детерминированный driver fake, подтверждение,
  readonly/no-op, stale/UID, частичный отказ, timeout, rollback, отмена,
  отключение, смена capabilities и process-local exclusion. Fake явно
  существует только в тестах, публичный C ABI не принимает test backend.
- `e2e_audio_hardware`: настоящий C ABI, неверные структуры/версии/значения,
  отсутствующее устройство, job lifetime после destroy, отсутствие изменений
  проекта и межсессионный busy guard. Никогда не настраивает живое устройство CI.
- AppKit workspace harness: реальная панель/кнопка, blocked/pending/completed
  состояния, обновление фактических значений, ошибочный refresh, закрытие,
  непоказательная для железа fixture и отдельный отрицательный реальный C path.
  Снимок `audio-hardware-settings.png` — native fixture, не живой интерфейс.
- Прежние core/SDK/workspace/Mixer/Piano Roll/export проверки сохранены.

Финальные результаты CI и merge фиксируются в PR; существование файла не
означает успешной сборки. Physical QA в issue #20 остаётся открытым: реальный
интерфейс и выбранные каналы, смена 44.1→48, 64/128/256, readonly/reject,
отключение в момент операции, повторный Play/Record, наушники/MON и VoiceOver.
P0-03 обычная duplex-запись и P0-04 placement/latency остаются следующими.

## Первичные API

- [HAL property writes](https://developer.apple.com/documentation/coreaudio/audioobjectsetpropertydata(_:_:_:_:_:_:)).
- [Writable properties](https://developer.apple.com/documentation/coreaudio/audioobjectispropertysettable(_:_:_:)).
- [Available nominal rates](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertyavailablenominalsamplerates).
- [Buffer bounds](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertybufferframesizerange).
