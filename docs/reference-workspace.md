# Рабочее пространство по визуальному референсу

## Цель этого среза

Референс владельца: библиотека слева, закреплённые заголовки и аранжировка в центре,
инспектор справа, устройства/редактор/микшер снизу, компактный транспорт сверху.
Палитра — графитовая с синим акцентом; цвета клипов остаются данными проекта.
Это развитие настоящего AppKit-приложения, не отдельный HTML-прототип.

## Компоненты

- `WorkspaceLayout` — ограниченная геометрия и настройки окна в UserDefaults,
  не часть проекта и не отдельная история Undo.
- `WorkspaceView` — постоянные нативные NSSplitView. Перетаскивание разделителей
  поручено AppKit, содержимое панелей не пересоздаётся при выборе вкладок.
- `LibraryBrowserView` — существующие каталог, импорт и AudioPreviewController,
  с поиском, категориями аудио/инструменты/эффекты и NSURL drag payload.
- `InspectorBrowserView` теперь отвечает только за выбранный канал/клип и MIDI-вход.
  Тот же `PianoRollEditorView` перенесён в нижнюю панель, callbacks сохранены.
- `ChannelRackView` показывает фактические inserts, порядок, bypass, формат и
  заявленную плагином задержку. Команды идут через существующий C ABI и редактор
  параметров. Произвольные графики EQ/компрессора не рисуются: они могли бы
  выдавать декоративную кривую за реальное состояние плагина.
- `MidiArrangementView` рисует проекцию реальных клипов и нот через C ABI;
  двойной клик выбирает тот же клип в существующем нижнем MIDI-редакторе.

Публичные интерфейсы NSSplitView, NSTableView, NSSearchField, NSScrollView и
NSPasteboard используются из системного AppKit. Нового UI/DSP framework нет.
Кнопки устройств не имитируют интерфейсы Serum/FabFilter/Waves из референса.
Нет выдуманных factory samples, CPU-метров, подключённого Apollo или нот проекта.
Демонстрационный аудиоматериал существует только в тесте и импортируется через ABI.

## Поведение

`Shift-Cmd-1/2/3` переключают библиотеку, инспектор и нижнюю панель.
`Shift-Cmd-0` восстанавливает размеры и масштаб. Масштаб 1 показывает ширину
всего проекта в текущем viewport, а не фиксированную полосу в 1400 пикселей.
Небольшое окно сначала сжимает боковые панели; временное скрытие при нехватке
ширины не стирает пользовательское предпочтение. Центр сохраняет рабочую ширину.
Изменение рабочего режима не запускает запись и не меняет аудиограф.

Импорт и добавление/удаление/перестановка устройств заблокированы при записи.
Недоступный элемент библиотеки нельзя добавить или прослушать. Ошибки числового
ввода clip inspector отклоняются до преобразования в unsigned frame.
Имена и значения в inspector читаются из сессии; после Undo/удаления выбор
синхронизируется. При выборе аудиодорожки устаревшие MIDI-ноты редактора очищаются.

## Проверка

```sh
./scripts/build-macos.sh
./scripts/test-workspace-ui.sh
```

`Native workspace UI` собирает тот же source manifest, запускает отдельный
AppKit-test executable и сохраняет PNG настоящего view hierarchy и логи.
Тестовый compile flag исключает только startup recovery, внешнее сканирование,
таймеры и обычный запуск окна. Материал проходит через реальные import/MIDI/insert
команды. Проверяются переключения без изменений revision, поиск и категории,
числовые guards, fader Undo, rack bypass/reorder/remove Undo и системный AU.
Чистая модель геометрии дополнительно проверяется более чем на 3000 размерах.

Окончательные результаты запуска фиксируются в PR вместе с head SHA.
PNG AppKit не заменяет ручную проверку VoiceOver, клавиатурного фокуса, drag-and-drop
с Finder, изменения размера во время воспроизведения и физического аудио.

## Совместная работа

Этот срез основан на проверенной ветке PR #5. Независимые PR #3 (новый микшер)
и #4 (графический piano roll) не сбрасываются и не переписываются. В этой ветке
нижняя панель размещает имеющиеся базовые редакторы. Их новые версии нужно
объединить отдельным интеграционным проходом, сохранив callbacks и нативные проверки.

## Navigation and density refinement

The ruler now consumes native mouse events rather than only storing callbacks:
a marker chip seeks the existing session locator, a double-click in the marker
band invokes the existing add-marker dialog, and a context click obtains the
existing rename/delete menu. Chips represent **point locators**, not a new
arrangement-section model. Their labels are clipped before the next locator;
nearby collapsed chips still have a flag hit target. Beat labels continue to
come from the authoritative tempo map; seconds and hit testing share the same
48 kHz frame coordinate system.

The command bar keeps import, track creation, Undo/Redo, grid and zoom visible.
Track creation and secondary commands use AppKit `NSPopUpButton` / `NSMenu`;
their items refer to the original buttons, preserving target, sender and all
existing recording/Undo guards. Disabled or hidden sources are checked again
at dispatch, even when a previously opened menu item has become stale. No
additional command registry or third-party UI framework is introduced.

Refresh still rebuilds track projections, but preserves the native scroll
origin and clamps it through `NSClipView.constrainBoundsRect`. This prevents
selection, gain changes and Undo from jumping to the start of a zoomed
arrangement. One shared lane height aligns audio, MIDI and pinned headers.
Only the authoritative selected channel is highlighted; an old inspector
selection cannot highlight a second track. Validation errors are cleared when
the inspected clip changes. Native controls retain AppKit tracking and
accessibility; the new drawing is limited to styling and ruler visuals.

Run `bash scripts/test-workspace-ui.sh` after `./scripts/build-macos.sh` on a
Mac. The integration harness exercises native menu dispatch and mouse events,
actual session seek/revision/recording guards, retained scroll coordinates,
compact command-bar bounds, and the previous browser/inspector/rack tests.
The usual workflow captures real AppKit PNGs at three window sizes. Synthetic
input is not a substitute for physical device or interactive user acceptance.

Primary AppKit contracts used here:
[menu item validation](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/MenuList/Articles/EnablingMenuItems.html),
[clip-view bounds constraints](https://developer.apple.com/documentation/appkit/nsclipview/constrainboundsrect(_:)),
and [native color blending](https://developer.apple.com/documentation/appkit/nscolor/blended(withfraction:of:)).


## Cycle-range and zoom iteration

The 24-point strip below the seconds ruler edits the existing transport/export/
comp selection. Drag empty space to create it, either handle to resize it, or
its body to move it without changing duration. Option-drag creates a new range
inside an existing one. Shift bypasses the existing tempo-map beat grid; Escape
cancels a preview. A click or a zero-width creation is a no-op. The native context
menu toggles looping without losing the selected export range, or clears both.
Delete while this strip has focus clears the range, not a clip.

Only release commits: mouse movement never calls the audio engine. The existing
`daw_set_loop` validates against actual project duration (not viewport padding)
and **stops playback when changing a loop**. This iteration preserves that ABI
behavior; it does not claim uninterrupted live loop-boundary editing. No project
revision or undo entry is consumed. Audio and MIDI recording lock all entry
points, including previously available In/Out/Clear/Loop commands and stale menu
callbacks. If the project changes before release, the public ABI revalidates the
range and a failed commit keeps the prior UI selection.

Zoom buttons and existing keyboard shortcuts anchor to a visible playhead. If
it is offscreen, they retain the viewport midpoint instead. AppKit clamps scroll
bounds and keeps the pinned headers aligned. Range/zoom command routing is in
`Workspace/TimelineNavigation.swift`, extracted from the large coordinator.

The additional native tests are in `tests/workspace_timeline_tests.swift`;
Foundation-only range/zoom geometry is checked by `tests/timeline_range_tests.swift`.
Both are invoked by `bash scripts/test-workspace-ui.sh`. The native harness
uses real NSEvent, NSMenu and existing C ABI transport commands, captures
`workspace-cycle.png`, and does not open physical audio or MIDI devices.

Ready-made mechanisms reused: AppKit [autoscroll](https://developer.apple.com/documentation/appkit/nsview/autoscroll(with:)),
[NSClipView bounds constraints](https://developer.apple.com/documentation/appkit/nsclipview/constrainboundsrect(_:)),
native target/action menus, the existing beat-grid resolver, and `daw_set_loop`.
There is no new DSP or dependency and no duplicate persisted range model.
