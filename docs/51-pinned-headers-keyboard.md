# Pinned track headers and keyboard command routing

Версия 1.24.0 завершает основной code scope B-018. Arrangement разделён на фиксированную колонку track headers и независимо масштабируемый timeline. Визуальная плотность продолжает направление [Fender Studio Pro](https://intl.fender.com/products/fender-studio-pro), сохраняя собственные компоненты и модель взаимодействия My DAW. Формат проекта остаётся v14.

## Arrangement structure

Левая колонка и timeline используют отдельные `NSScrollView`. Горизонтальный scroll и zoom существуют только у timeline; имя и базовые controls дорожки остаются на месте. Вертикальные clip views синхронизируются через `boundsDidChangeNotification` с защитой от рекурсивного обновления.

Для каждой дорожки обе стороны получают одинаковую вычисленную высоту: основная lane занимает 132 pt, каждая раскрытая take lane добавляет 46 pt и общий spacing. Поэтому заголовок, waveform и take lanes не расходятся при прокрутке. Положение разделителя сохраняется средствами `NSSplitView` отдельно от переносимого проекта.

Pinned header содержит имя, R/M/S, gain, pan, количество takes и меню команд: import take, comp, split at playhead, duplicate clip, crossfade и delete clip. Выбор header синхронизирует mixer и Inspector. Track routing, insert chain и sends перенесены в нижний console dock: они остаются доступными, но не занимают горизонтальное пространство timeline.

## Keyboard boundary

Главное окно маршрутизирует общие DAW-команды до конкретного canvas:

- Space — play/stop;
- Home — rewind;
- Backspace/Delete — удалить выбранный clip;
- Command `+`, `-`, `0` — zoom in, zoom out и reset.

Стандартная menu routing обрабатывается первой. Глобальные команды не перехватываются, когда активен `NSTextView`, `NSTextField`, `NSSearchField` или marked-text composition, поэтому переименование, поиск и ввод чисел сохраняют системное поведение.

## Границы среза

Layout, zoom, browser selection и panel sizes являются UI state. Audio, routing, clips, takes, mixer values и plug-in chains продолжают изменяться через revision-aware C bridge и сохраняются в `.mydaw`. Runtime plug-in sandbox, vendor editors, media preview и physical UX/visual acceptance остаются отдельными задачами.

Выполнена compile/build проверка локального macOS bundle. QA, запуск приложения, прослушивание и hardware не выполнялись по текущему режиму разработки.
