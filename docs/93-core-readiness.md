# Core DAW: очередь до рабочей альфы

Решение владельца: сначала надёжная основа DAW, без AI, Reference A/B, ASR,
новых возможностей палитры и внешнего Extension SDK. Это очередь по результату
сверки main `660c0e4` с [MVP](02-mvp.md), а не перечень полностью отсутствующих функций.
Существующие реализации дополняются, не переписываются без необходимости.
Статусы code/CI/physical acceptance различаются; сам документ не подтверждает merge.

| Очередь | Задача | Реальный пробел / критерий закрытия |
|---|---|---|
| P0-01 | Выбор аудиоустройств и каналов | [Текущий срез](92-audio-device-selection.md): UID, явные input/output maps, persistence, missing-device и busy guards. До него все три hardware paths жёстко брали system defaults. Физическая приёмка отдельна. |
| P0-02 | Управление rate/buffer | Явная установка поддержанной частоты/буфера с подтверждённым HAL состоянием, timeout/error и запретом изменения при записи. Не путать проектные 48 kHz и аппаратный режим. [Реализация и проверки](94-audio-device-rate-buffer.md): native editor, ranges/settable, notification + readback, ошибки/timeout, transport guards. Физическая HAL-приёмка отдельна. |
| P0-03 | Обычная запись поверх проекта | В `startRecording` duplex пока используется только для loop takes; обычный Record открывает input-only и выключает playback. Нужен единый full-duplex путь с backing playback, MON и preroll для обычной записи тоже. |
| P0-04 | Компенсация записи | Sample/host timestamps, input/output latency/safety offsets, подтверждённое размещение capture, loop/punch boundaries. PDC плагинов не подменяет этот пункт. |
| P0-05 | Два канала и длительная запись | Сейчас mono input, минутный лимит capture, 8 audio tracks / 32 assets / 64 MiB. Сначала bounded streaming/read-ahead и recovery, затем снять ограничения без RT allocation. Приёмка REC-02: 30 минут двух каналов с playback. |
| P0-06 | Ошибки записи и восстановление | Проверить input callback errors/stalls, отключение интерфейса, disk-full/slow writer, частично завершённые passes, cancel и завершение приложения. Recovery уже есть; нужен единый проверенный пользовательский путь. |
| P0-07 | Punch/loop/comp | Lanes, comp, fades и loop есть. Закрыть физическую синхронизацию, редактирование границ и недеструктивный сквозной сценарий 3 takes → 4 phrases → Undo → Save/Open. |
| P0-08 | Сохранность проекта | Save/Open/Save As/recovery/backup уже есть. Убедиться в копировании медиа, безопасном закрытии и повторном открытии старых fixtures, особенно после записи. |
| P1-01 | Routing/mixer/automation | Buses/sends/PDC/Read-Touch-Latch реализованы. Закрыть live-переходы, прерывание gestures, сохранение/Undo, реальные latency и отсутствие щелчков. |
| P1-02 | Плагины | AU/VST3 scan/state/instruments/isolation уже есть. Vendor editors, missing/crash recovery, проверенная матрица и sidechain остаются отдельными шагами. |
| P1-03 | MIDI | Piano Roll и нотная запись есть. MIDI→instrument→mixer→save/export и физический input; sustain/CC требуют реализации и regression fixtures. |
| P1-04 | Экспорт | WAV/стемы/tails и MIDI-only export есть. Сравнить реальное воспроизведение и файлы; realtime fallback для unsupported offline plugins и базовый freeze/render track остаются отдельными задачами. |
| P1-05 | UX и производительность | Общая selection/focus, start-from-empty, доступные ошибки, 50–100 дорожек, memory pressure, длинная сессия. Нынешний in-memory audio limit сначала требует P0-05, а не простого увеличения констант. |
| Release gate | Студийная приёмка | Физический Mac+интерфейс+микрофон+MIDI+AU/VST3, полный вокальный проект, Save/Open/Undo/export, прослушивание и VoiceOver. Не закрывать зелёным CI. |

Порядок работы: bounded PR от актуального main → целевые тесты → полный
применимый CI объединённого дерева → merge с проверкой head SHA → фиксация
оставшихся hardware gates. Следующая задача выбирается по этой таблице,
а не по простоте добавления нового экрана.
