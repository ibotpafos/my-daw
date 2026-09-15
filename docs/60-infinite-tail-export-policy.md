# Infinite-tail export policy — 1.33.0

Некоторые эффекты объявляют конечный decay, а другие возвращают VST3 sentinel бесконечного хвоста. Бесконечный offline render не является полезным результатом: он может не завершиться и не должен молча превращаться в непонятный файл. Версия 1.33 делает это решение явным перед WAV export.

## Выбор при экспорте

Единый WAV dialog объединяет encoding и политику хвоста. Пользователь выбирает PCM24 или float32, затем один из вариантов:

| Вариант | Результат |
|---|---|
| `Автоматически` | Сохраняет обычный finite tail; infinite declaration ограничивается безопасным default limit. |
| `Только конечный` | Не добавляет infinite component; уже известный finite decay остаётся в WAV после необходимой latency compensation. |
| `Ограничить` | После range подаётся тишина 2, 5, 15 или 30 секунд для infinite component; известный finite decay не обрезается этой настройкой. |

Dialog поясняет, когда выбранный insert объявил infinite tail, и не называет resulting WAV «полным» после искусственного предела. Для finite tail текст показывает добавляемую длительность. Save panel сохраняет контекст выбранной policy, while percent остаётся основанным на фактическом количестве кадров job.

## Граница данных

`Renderer` сообщает export summary с finite frames и признаком infinite declaration. Versioned C export options передают policy в новый start path; существующие `daw_begin_export` и `daw_begin_export_range` остаются совместимыми и используют automatic default. Export job всё так же владеет frozen project snapshot, а policy не меняет `State`, revision, Undo или draft format.

Policy — локальное preference (`UserDefaults`), а не параметр проекта: export другой версии одного и того же проекта может иметь другую длину хвоста только по явному выбору пользователя. Сохранённое значение ограничено допустимыми preset values; invalid preference возвращается к automatic.

## Accessibility и отмена

Popup имеет AX label «Хвост после диапазона» и help, описывающий последствия active choice. Он участвует в обычном keyboard focus order между форматом и подтверждением export. No-tail и bounded modes не отключают Cancel: при отмене temporary WAV удаляется, прежний файл назначения остаётся нетронутым, как и в остальном export path.

## Проверка

Debug build и полный CTest проходят: 8/8. `offline_wav_export` проверяет распознавание VST3 sentinel, последовательное и параллельное сложение хвостов, automatic/finite-only/manual policy и отказ manual limit больше 30 секунд. C bridge покрыт version/size/mode validation, preview без изменения revision и успешным option-aware export реального WAV без plug-in tail; существующие header, progress и atomic-cancel проверки продолжают проходить.

Сборка macOS 1.33.0 (build 43) создана для arm64, deep codesign validation проходит. В локальном AX smoke подтверждены основное рабочее окно и доступность команды импорта; системная Open panel корректно выбрала тестовый WAV. После подтверждения файла Accessibility backend перестал возвращать дерево окна, хотя process sample показывал idle AppKit event loop, поэтому состояние самого tail dialog, переключение трёх policy и keyboard focus не отмечаются как визуально подтверждённые.

Открыты physical listening с реальными reverbs/delays, vendor VST3 с фактическим infinite declaration, whole-project/range UI matrix, restart persistence, полный VoiceOver проход и hardware presentation timestamp.
