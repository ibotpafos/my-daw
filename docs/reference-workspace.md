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
