# Аудиоустройства и аппаратные каналы

## Задача и границы

Первый core-срез DEV-01 из [спецификации](02-mvp.md): собственный выбор
аудиовхода/выхода и аппаратных каналов вместо безусловного использования
системных defaults. Очередь оставшейся фундаментальной работы находится в
[core readiness](93-core-readiness.md). Это не заявление о полной готовности DEV-01/S1.

«My DAW → Настройки аудио…» / Cmd+, показывает реальные Core Audio устройства,
mono-вход и независимые L/R каналы stereo master. Числа в интерфейсе начинаются
с 1; API использует индексы с 0. Список обновляется явной кнопкой, без запуска
микрофона и без сканирования из audio callback. Одноимённые устройства различаются
по UID; числовой AudioDeviceID и позиция строки не сохраняются.

По умолчанию сохранено прежнее поведение: пустой UID означает явно выбранное
«Системное устройство по умолчанию». Выбранный непустой UID **никогда не заменяется**
системным устройством при отключении, перезапуске или открытии проекта. Отсутствие
устройства, неправильные каналы и неподходящий формат дают отказ запуска, а не
запись с другого микрофона. Missing-строка остаётся видимой после Refresh.

## Что именно применяется

Одна `AudioDeviceConfiguration` проходит через C ABI в playback preparation,
dry Input и full-duplex loop path. Worker получает копию конфигурации; смена
настроек запрещена при подготовке/воспроизведении/записи/MIDI capture. Setter
проверяет структуру, версию, bounded UID и каналы атомарно, но не открывает
hardware: сохранение намерения для временно отключённого интерфейса допустимо.
Перед каждым реальным запуском UID заново разрешается в актуальный ID.

AUHAL input channel map выбирает один физический источник. Output map содержит
по элементу на hardware destination: только выбранные L/R получают клиентские
каналы 0/1, остальные отмечены `-1` (silence). Mono/stereo преобразование и mapping
выполняет существующий AUHAL, собственный DSP/ресемплер не добавлен.

Сессия остаётся **48 kHz**. Панель сообщает текущую nominal rate и buffer size,
а не выдуманную измеренную round-trip latency. В этом срезе частота и размер
буфера **только читаются**: для изменения открывается системный Audio MIDI Setup,
размер буфера при необходимости настраивается в панели производителя.
Программная смена rate/buffer с подтверждением HAL — следующая задача, не скрытая
автоматическая настройка. Неподходящая частота или buffer >4096 запрещают запуск.

Full-duplex loop по-прежнему требует одного физического либо заранее созданного
aggregate-устройства для input/output. Независимые clocks не синхронизируются
тайно. Нынешняя обычная (не loop) запись всё ещё использует input-only path:
общая запись поверх проекта вынесена в следующий core-срез.

## Runtime и сохранение

На control thread проверяются alive/UID/nominal rate/buffer/channel configuration.
Изменение выбранного устройства или его формата останавливает текущий путь через
прежний lifecycle/recovery. Смена системного default не прерывает **явно** выбранное
устройство; для режима follow-default остаётся поводом остановить старый поток.
Существующие callback/storage recovery механизмы не заменяются.

`audio.devices.v1` — bounded versioned JSON в UserDefaults, не SQLite проекта.
Конфигурация восстанавливается при запуске и New; Open не загружает чужие аппаратные
настройки из проекта. Invalid input целиком отклоняется. Поиск устройств и
настройки не меняют revision, Undo, routing музыкального проекта или MON.

## Проверки и ручная приёмка

- `audio_device_selection`: разрешение UID/default, одинаковые имена, новый runtime
  ID, missing device, channel bounds, sample-rate/buffer bounds и stereo maps.
- `e2e_audio_device_settings`: реальный публичный C ABI, ошибки shape/version,
  atomic rejection, read-only HAL enumeration, project/prefs isolation,
  запрещённая смена во время preparation без открытия audio unit.
- `audio_device_preferences_tests.swift`: JSON round-trip, corrupt/oversized data,
  UTF-8 UID, channel bounds и перезапуск UserDefaults.
- `workspace_audio_device_tests.swift`: настоящий AppKit controller → production
  action → C ABI → preferences; missing/reordered catalog, UI bounds и PNG.
  Каталог UI-теста — явно названный fixture; это не физическое устройство.

Фактический статус прогонов фиксируется в PR, а не предполагается по наличию тестов.
Обязательная аппаратная проверка: выбрать интерфейс не равный system default,
записать физический вход 2/4, направить master только на 3/4, перезапустить,
отключить/подключить интерфейс, поменять system default и rate во время playback.
Проверить корректную остановку/восстановление; отдельно физические latency,
слух, 30-минутная запись и VoiceOver. CI этого не доказывает.

## Переиспользование и первоисточники

Core Audio AudioObjectGetPropertyData/GetPropertyDataSize, DeviceUID,
StreamConfiguration, AUHAL CurrentDevice/ChannelMap, AppKit и Foundation.
Новых зависимостей, project format и сторонних DSP нет.

- [Apple TN2091: Device input using HAL](https://developer.apple.com/library/archive/technotes/tn2091/_index.html)
- [Apple: Common Core Audio tasks](https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/ARoadmaptoCommonTasks/ARoadmaptoCommonTasks.html)
- [Buffer frame size](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertybufferframesize)
- [Nominal sample rate](https://developer.apple.com/documentation/coreaudio/kaudiodevicepropertynominalsamplerate)


## Следующий срез: аппаратный формат

[48 kHz и размер буфера](94-audio-hardware-format.md) добавлены отдельной
явной операцией. Выбор UID/каналов и чтение preferences по-прежнему не меняют
аппаратный формат автоматически. Исторические ограничения среза выше не
подменяют статус нового PR и физическую приёмку.
