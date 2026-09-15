# Professional DAW workspace

Версия 1.21.0 начинает B-018: существующие запись, монтаж, routing, mixer и plug-in функции собраны в одно рабочее окно, которое можно использовать как arrangement и console, не прокручивая длинную страницу проекта. Формат проекта остаётся v14.

## Рабочая область

Верхняя панель объединяет transport, project, edit, range и grid controls. Центральный `NSSplitView` делит окно на вертикально прокручиваемый arrangement и изменяемую по высоте нижнюю console. Строка состояния оставляет видимыми позицию transport, состояние сохранения и основные клавиатурные подсказки.

Arrangement оставляет track header рядом с его clip controls, routing, inserts, sends, waveform и take lanes. Timeline рисует ruler, major/minor grid, loop/range overlay и отдельный audible playhead. Selected и hovered клипы имеют разные контуры и курсоры, поэтому body, trim и fade зоны читаются до начала drag.

## Прямой монтаж

Перетаскивание clip body меняет timeline start, края меняют source offset и length. Fade-in и fade-out handles дают локальный preview и записывают новые frame bounds через существующую domain-команду. Snap применяется ко всем drag операциям, `Shift` временно его отключает, `Escape` возвращает geometry до начала жеста. Split, duplicate, delete, crossfade и точный числовой editor остаются рядом с выбранным клипом.

## Mixer console

`MixerWorkspaceView` строит горизонтальные полосы track, bus и master. Полосы поддерживают selection, record arm, mute, solo, vertical volume fader и pan; списки inserts и sends отображаются прямо на channel strip. Volume использует общий Read/Touch/Latch contract, поэтому вооружённая полоса записывает один automation gesture, а обычное движение меняет статический channel control.

Под полосами находятся подробные master/bus/plugin controls: routing, add/reorder/bypass/remove inserts, generic parameters и parameter automation. Console остаётся доступной одновременно с arrangement и изменяется divider-ом.

[1.22.0](49-studio-pro-workspace-meters.md) подключил общий horizontal zoom/ruler, правый inspector, сохранение dock layout и реальные per-channel stereo meters. [1.23.0](50-inspector-browser.md) добавил editable inspector/browser, а [1.24.0](51-pinned-headers-keyboard.md) — pinned headers и единый keyboard command boundary. Сохранение отдельных editor tabs и physical UX acceptance остаются дальнейшей работой.

Выполнена compile/build проверка. QA, запуск приложения, прослушивание, hardware и сторонние vendor plug-ins не выполнялись по текущему режиму разработки.
