# 88. WAV-экспорт MIDI-only проекта из основного интерфейса

Следующий срез issue #9 после [настоящего instrument-source](85-vst3-instrument-source.md)
и [единой длительности транспорта](86-midi-transport-duration.md).
Эта ветка stacked поверх PR #7: `codex/midi-only-export-ui` →
`codex/vst3-instrument-source`. Не переносит инструментальный хост повторно,
не заменяет main и не отменяет работу Piano Roll / открытую QA #10.

## Поведение

Основное действие `DraftApp.exportMix()` и кнопка WAV принимают audio-only,
MIDI-only и смешанный проект. Наличие аудиофайла больше не обязательно.
`MixExportPolicy` — один критерий для кнопки, проверки menu action, прямого
вызова, повторной проверки после диалогов и восстановления после фонового job.

Проверяется ненулевая длительность **из C ABI**, а не новая формула Renderer.
Наличие материала не является обещанием звука: пустой MIDI-клип с длительностью
можно экспортировать, но без активного инструмента WAV будет тихим.
MIDI-only диалог и tooltip объясняют необходимость инструмента. Готовый файл
измеряется прежним `daw_measure_wav`; gated silence явно отражается в статусе,
а не объявляется успешно прозвучавшим инструментом.

Экспорт недоступен во время audio/MIDI capture, импорта/другого экспорта,
незавершённой automation/parameter/clip/range операции и второго диалога.
Действие **не останавливает запись** и не фиксирует жест автоматически.
Capture читается также непосредственно через `daw_get_recording` /
`daw_midi_record_status`: UI-флаг не единственная защита.
Один start без end остаётся позицией курсора, не ошибочным диапазоном.
Сохранение и recovery используют frozen snapshots и сами по себе экспорт не блокируют.

## Модальные диалоги и неизменность запроса

Стандартные AppKit NSAlert/NSSavePanel сохранены. Они запускают вложенный event
loop, поэтому перед ними фиксируются session pointer, UUID документа, revision
и диапазон. После выбора файла всё проверяется повторно. New/Open/recovery
меняют UUID даже при восстановлении той же сохранённой revision.

Смена документа, правки или диапазона не перенаправляют экспорт на другой
снимок молча: пользователь получает предложение открыть диалог заново.
Только после повторной проверки вызывается существующий
`daw_begin_export[_range]_with_options`. Renderer, хвосты, PCM24/Float32,
компенсация задержки и атомарная публикация файла остаются в движке.
Новых DSP/формата проекта/публичного C ABI/зависимостей нет.

`MixExportInteraction` подставляет только ответы человека и отображение
ошибок для native harness. Production использует NSAlert/NSSavePanel;
тесты не заменяют session, AU, storage, export job или PCM-файл.

## Проверки

```sh
bash scripts/test-mix-export-policy.sh
./scripts/build-macos.sh
bash scripts/test-mix-export-ui.sh
bash scripts/test-workspace-ui.sh
```

Первый runner не требует AppKit. Второй компилирует production sources manifest
с отдельной точкой входа: recovery/scanning/timers выключены так же, как в
существующем native workspace harness. Это не обычный запуск `.app`.

Native runner вызывает реальный `NSButton.performClick` / `DraftApp.exportMix`,
отвечает на диалоги, добавляет настоящий **Apple DLS Synth** через публичный
C ABI, ждёт job через production `pollStorage` и читает WAV системным AVAudioFile.
Отсутствие DLS — failure этой проверки, а не skip или замена тестовым синтезатором.
Проверяются finite/nonzero stereo 48 kHz PCM, диапазон, save/open, project Undo,
блокировки, повторный/запоздалый запрос, отмена с сохранением существующего файла,
реальная ошибка файловой системы, retry, bypass/отсутствие инструмента,
смешанный PCM24 и audio-only проект.

CI `Native workspace UI` сохраняет `mix-export.log`, фактический
`midi-only.wav` и `proof.json` рядом с прежними native артефактами.
Результат конкретного прогона и commit указываются в PR; наличие теста не
считается его прохождением.

## Границы приёмки

Подставленные ответы диалогов не доказывают ручную работу системного file picker,
Powerbox, перезапись выбранного файла с пользовательским подтверждением или
поведение разных vendor-инструментов. VST3 MIDI-to-audio проверен отдельно в
PR #7; данный новый UI runner доказывает путь **системного AU**, не все VST3.
Физические устройства, прослушивание и ручная приёмка остаются отдельными.
До интеграции stacked веток и повторного общего main CI issue #9 не закрывается.

Переиспользованы существующие AppKit и export APIs. Первоисточники:
[панель сохранения](https://developer.apple.com/documentation/appkit/nssavepanel),
[проверка доступности команд](https://developer.apple.com/documentation/appkit/nsuserinterfacevalidations).
