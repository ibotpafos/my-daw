# Editable inspector and integrated browser

Версия 1.23.0 продолжает B-018 и превращает правый dock рабочей области в две постоянные вкладки: Inspector и Browser. Визуальная иерархия продолжает направление [Fender Studio Pro](https://intl.fender.com/products/fender-studio-pro), но использует собственные компоненты и поведение My DAW. Формат проекта остаётся v14.

## Inspector

Выбор track, bus или master в mixer открывает channel editor. Он показывает тип и имя полосы, volume, pan, mute, solo, inserts и sends. Имя track/bus и параметры полосы изменяются существующими revision-aware командами domain model, поэтому изменения входят в Undo и сохраняются в `.mydaw`. Недоступные для конкретной полосы команды остаются без мутации.

Выбор клипа сохраняет контекст его дорожки и добавляет поля start, source offset, length, fade in и fade out. Применение выполняется одной командой `daw_edit_clip_full`, включая проверку границ исходного аудио и суммы fades. После успешной команды inspector перечитывает данные по durable track ID и clip index.

## Media browser

Audio browser перечисляет до 1000 WAV из папки, которую пользователь явно выбрал через системную панель. My DAW не сканирует `Documents`, домашнюю папку или весь диск автоматически и не сохраняет фоновый доступ к выбранной папке. Поиск фильтрует имя файла и имя родительской папки; Add импортирует выбранный WAV как новую дорожку через `daw_import_wav`.

Текущий import contract остаётся прежним: RIFF WAV, 48 kHz, mono/stereo, PCM16/24/32 или float32, до 60 секунд и 32 MiB. Более богатые media metadata, preview и persistent favorites потребуют отдельного read-only probe/catalog API.

## Plug-in browser

Browser объединяет transient AU и VST3 catalogs в один фильтруемый список. AU берутся из isolated scan/cache, VST3 дополнительно показывают vendor и availability. Недоступный VST3 остаётся видимым как missing, но добавить его нельзя.

Add направляет выбранный эффект в текущий track, bus или master. UI преобразует выбранную полосу в `DAW_INSERT_OWNER_*`, а C bridge вызывает `daw_add_insert_au` или `daw_add_insert_vst3` с актуальной revision. Domain model проверяет owner, durable ID, availability и лимиты inserts, создаёт один Undo step, сохраняет descriptor/state в `channel_plugins` и перестраивает transport renderer.

## Границы среза

Browser selection и раскрытая вкладка являются локальным состоянием UI и не входят в переносимый музыкальный проект. Native vendor editors, media preview, arbitrary sample-rate conversion и runtime plug-in sandbox не входят в этот срез. [1.24.0](51-pinned-headers-keyboard.md) добавил pinned track headers и единый keyboard command boundary.

Выполнена compile/build проверка локального macOS bundle. QA, запуск приложения, прослушивание, hardware и сторонняя vendor matrix не выполнялись по текущему режиму разработки.
