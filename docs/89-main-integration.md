# Интеграция workspace, Piano Roll и MIDI-инструментов

Этот срез объединяет ранее раздельно проверенные ветки в одном приложении:
main с Piano Roll (#4), инфраструктуру/AU (#1), VST3 SDK (#5), workspace и
фоновую библиотеку (#6), VST3-инструменты (#7), MIDI-only WAV action (#11).
Общий интеграционный PR — #12. Во время интеграции main продвинулся от
`176aeb3` к `acbd043` (уже merged Mixer Console #3). Новая база учтена:
микшер, маршрутизация, send controls v22 и связанные фейдеры сохранены.
Отдельная Command Palette (#8) этим срезом не включена.

## Согласованные границы

- Сохранены **все** исходники Piano Roll и его тесты. Тот же экземпляр
  `PianoRollEditorView` находится в нижнем dock; его кнопка открывает отдельное
  окно редактора. Inspector передаёт context, metadata и notes атомарно через
  `apply(model:)`, а запись — через revision-bound `PRCommitRequest`.
- MIDI replacement принимает до 65536 нот, страницы чтения/add/append — 8192;
  правила общего бюджета проекта и negative fixtures не ослаблены.
- После New/Open/recovery обновляются существующие идентификаторы документа
  и для Piano Roll, и для экспорта. Совпадение номера сохранённой revision
  не разрешает устаревшему callback менять повторно открытый документ.
- WAV export запрещён при незавершённом жесте/transform preview Piano Roll и
  ожидающем подтверждения commit. Открытие диалога не применяет preview и не
  экспортирует молча старые ноты. Кнопка/меню обновляются после изменения
  состояния редактора, а прямой selector повторно проверяет состояние.
- Сохранены MIDI duration/seek/loop, реальный VST3 instrument/event input,
  POSIX page-padding validation и не-Apple libsamplerate integration.
- Build, SDK typecheck и интеграционные UI harness используют один явный
  Swift manifest и один `DAWBridge.h`. Обе независимые Piano Roll проверки
  остаются обязательными рядом с workspace/export, core и VST3 workflows.
- Фоновое чтение выбранной папки и bookmark restoration не обращаются к
  сессии из worker. Тестовый экспорт отключает автоматическое восстановление
  папки, как уже отключает startup recovery/scans/timers; явные folder tests
  продолжают проверять настоящую файловую систему и контроллер.

## Совместимость нового микшера

Все новые Mixer Swift/C++ sources и три mixer unit targets зарегистрированы в
существующих manifest/CMake modules. Сохранены дополнительные E2E, schema v22,
send mute/independent balance, live mixer gesture и linked-level contracts.
Standalone mixer AppKit и production binding/C ABI suites остаются в CI.

Focus теперь разворачивает **тот же** console внутри reference workspace, а не
обращается к устаревшим splitters. Возврат восстанавливает обычную компоновку;
Focus не меняет project revision или сохранённые предпочтения размеров. У
обычного dock с микшером увеличен минимальный запрошенный размер, при узком
окне остаются ограничения общей геометрии.

Экспорт блокируется во время console gesture. MIDI capture запрещает начало
и подтверждение жеста; при начале захвата незавершённый preview отменяется.
Устаревшие Mute/Solo callbacks повторно проверяют capture/gesture. Новый
`workspace_mixer_tests.swift` проверяет эти контракты на настоящем DraftApp,
доменном состоянии и revision/Undo, не на отдельной модели микшера.

## Проверка совместного результата

`test-mix-export-ui.sh` дополнен сценарием **реальный docked Piano Roll →
preview/pending guard → commit/Undo → equal-revision reopen → AU/WAV action**.
Тест задерживает только callback подтверждения, чтобы проверить pending state;
успешная запись отдельно использует производственный C ABI. Ответы пользователя
на export-диалоги по-прежнему подставлены; сам AU/renderer/job/WAV не подменён.

Команды: `scripts/build-macos.sh`, `scripts/test-workspace-ui.sh`,
`scripts/test-mix-export-ui.sh`, `scripts/typecheck-macos-ui.sh`,
`scripts/test-piano-roll-appkit.sh`, `scripts/test-piano-roll-bridge.sh`,
`scripts/test-piano-roll-transforms.sh`, `scripts/test-mixer-ui.sh`,
`scripts/test-mixer-groups.sh`; CTest debug/sanitizers и SDK workflow.
Результаты конкретного commit, source archives и downloadable .app фиксируются
в PR #12. Наличие проверки в workflow не считается её успешным выполнением.

## Не является релизной приёмкой

Сборка development/ad-hoc, без нотарификации. Физические MIDI/audio устройства,
пользовательские плагины, реальные диалоги, VoiceOver и длительные сессии остаются
ручными проверками (Piano Roll QA #10 и Mixer QA #13 не закрываются). Сохранённые параметры и
изменение цикла всё ещё останавливают playback по прежним контрактам. MPE/CC,
звучащая экранная клавиатура, новый live automation path не реализованы этим
merge. Подробности: [Piano Roll](piano-roll-validation.md),
[экспорт](88-midi-only-export-ui.md), [папки библиотеки](reference-library-folders.md).
