# Studio Pro-inspired workspace and live meters

Версия 1.22.0 продолжает B-018. Визуальная иерархия опирается на актуальные официальные экраны [Fender Studio Pro](https://intl.fender.com/products/fender-studio-pro): плотный тёмный chrome, крупный arrangement, цветовые акценты дорожек, правый inspector и нижний mixer/editor dock. My DAW сохраняет собственные элементы, названия и поведение; чужие branding и assets не используются. Формат музыкального проекта остаётся v14.

## Общий timeline

Все дорожки находятся в одном горизонтально прокручиваемом document и используют общий ruler/playhead. Масштаб 1×–8× меняется кнопками и командами меню, поэтому waveform, grid, loop/range и automation curves остаются выровненными. Значение zoom хранится в `UserDefaults`; размеры arrangement/inspector и arrangement/console сохраняются через `NSSplitView` autosave и не загрязняют переносимый проект.

Каждая дорожка получает воспроизводимый цветовой accent из ограниченной palette. Этот цвет связывает номер полосы, clip region и mixer strip. Повторяющийся ruler внутри каждой waveform lane убран: время читается по единой верхней шкале, а вертикальная grid продолжается через клипы.

## Inspector и dock hierarchy

Правый inspector показывает реальный выбранный channel или clip: тип и имя полосы, volume/pan, mute/solo, inserts/sends либо start, length и fades клипа. Нижняя console остаётся изменяемой по высоте. Панели используют тонкие separators, малый radius и спокойные фоны вместо набора крупных карточек.

## Realtime channel meters

Renderer вычисляет stereo block peaks в фактических post-insert/post-fader точках каждой подготовленной track и bus chain; master измеряется после master inserts до output clamp. Callback использует фиксированные stack arrays и заранее созданные atomics, без allocation и locks. `prepare`, tail и остановка очищают telemetry.

Versioned C ABI `daw_channel_meter` читает track/bus/master по stable owner ID. Stopped/no-renderer состояние возвращает нули. macOS UI опрашивает meters вместе с transport, применяет короткий peak-hold decay и обновляет существующие meter views без перестроения mixer strips.

[1.23.0](50-inspector-browser.md) добавил editable inspector и встроенный media/plugin browser. Следующие B-018 задачи: pinned track headers при горизонтальном scroll и единая keyboard focus/command routing model.

Выполнена compile/build проверка. QA, запуск приложения, прослушивание, hardware и сторонние vendor plug-ins не выполнялись по текущему режиму разработки.
