# Обзор аранжировки

Небольшой навигатор над аранжировкой показывает расположение настоящих audio/MIDI
клипов в порядке дорожек, позицию воспроизведения, выделенный цикл и рамку видимой
области. Это карта **регионов**, а не waveform микса: аудио не декодируется и
дополнительные экземпляры инструментов не создаются.

## Управление

- Клик вне рамки центрирует видимую область на выбранном месте; перетаскивание
  рамки сохраняет точку захвата. Перемещается только горизонтальная прокрутка,
  не playhead и не клипы. Вертикальная позиция и pinned headers сохраняются.
- Двойной клик вызывает существующий fit-all zoom. Escape во время drag возвращает
  исходную прокрутку; последующий mouse-up не повторяет действие.
- При фокусе в навигаторе Left/Right сдвигают область на десятую её ширины,
  Page Up/Down — на 90%, Home/End — к краям. Delete/Command-Delete не удаляют
  данные аранжировки. Space сохраняет существующее управление транспортом,
  Command-K — палитру. Поиск и другие текстовые поля не перехватываются.
- Accessibility value/increment/decrement используют тот же путь прокрутки.
  Наличие действий не означает завершённой ручной приёмки VoiceOver.

## Переиспользование и границы

Геометрию фактического viewport и ограничение границ предоставляет AppKit
[`NSClipView.scroll(to:)`](https://developer.apple.com/documentation/appkit/nsclipview/scroll(to:)) /
`constrainBoundsRect`. Frame/bounds notifications обновляют рамку после обычной
прокрутки, zoom и изменения размера окна. Рамка отражает также существующий запас
пустого места на timeline. Новый масштаб или правила длительности не вводятся.

Контроллер проецирует существующие `laneViews`, `midiArrangementViews` и порядок
`mixerWorkspace.strips`. Чтения C ABI, копирования нот/PCM и file I/O в polling
транспорта нет. Пути клипов обновляются только после изменения document UUID,
revision, длины timeline или размера карты; playhead/cycle остаются overlays.

Новый документ (включая ту же revision) и изменение размеров отменяют текущий
жест. Навигация допустима во время захвата, поскольку не посылает команды движку,
не останавливает звук, не меняет project revision и не создаёт Undo. Это не новый
live-edit/audio path. Фокусный микшер скрывает навигатор вместе с аранжировкой.

Новых зависимостей, формата проекта, DSP или persisted command model нет. Общие
компоненты AppKit сохраняют identity; `main.swift` не разрастается.

## Проверки

`tests/arrangement_overview_tests.swift` проверяет чистую геометрию, NaN/Inf,
нулевые/большие размеры, UInt64.max и переполнение интервалов.
`tests/workspace_overview_tests.swift` выполняется production AppKit harness:
реальные NSClipView/NSEvent, локальный фокус и destructive-key guards,
frame/zoom/document invalidation, реальные track-color/Undo команды через C ABI,
неизменность transport/loop/revision, повторные resize и Mixer Focus.

Запуск: `bash scripts/test-workspace-ui.sh` после `./scripts/build-macos.sh`.
Окончательный результат, commit SHA и ссылки на CI фиксируются в PR; сам текст
этого документа не является доказательством выполнения тестов. Новые снимки:
`workspace-overview.png` и `workspace-overview-1060.png` — actual AppKit rendering
с тестовым материалом, не автоматически создаваемые дорожки продукта.
