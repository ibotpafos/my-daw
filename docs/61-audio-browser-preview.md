# 61. Browser audio preview

## Цель

Встроенный Browser должен быстро проиграть выбранный WAV перед импортом. Это audition, а не ещё один режим транспорта проекта.

## Реализация

`AudioPreviewController` использует `AVAudioPlayer` из AVFoundation и живёт только в macOS UI. Контроллер хранит выбранный URL, состояние воспроизведения и локальную ошибку. Он не знает о `daw_session`, C bridge, undo, project revision или playhead.

Browser передаёт UUID выбранной строки в `DraftApp`, где UUID сопоставляется с URL из явно выбранной пользователем папки. Кнопка «Прослушать» запускает этот же выбранный URL, «Стоп» останавливает только audition. Окончание `AVAudioPlayer` возвращает UI в готовое состояние.

Preview очищается при выборе plug-in, смене папки Browser, импорте Browser-аудио или обычном импорте WAV, создании/открытии draft и при завершении приложения. Поэтому старый файл не продолжает играть после смены контекста.

## Контракт UI

`InspectorBrowserView` получает `isPlaying`, UUID выбранного аудио и текст локальной ошибки через `updateAudioPreview`. Кнопки имеют отдельные accessibility label/help и не меняют доступность команд добавления/импорта.

## Проверка и границы

`scripts/test-audio-preview.sh` компилирует controller с fake-player тестом и зарегистрирован как Apple CTest `macos_audio_browser_preview`. Он покрывает выбор, старт, остановку, естественное завершение, ошибку открытия и игнорирование stale finish callback. Полный Debug CTest проходит 9/9; arm64 app 1.34.0 (build 44) собирается и проходит deep codesign validation.

Локальный visual/AX smoke подтвердил отдельную строку «Прослушать»/«Стоп», начальные disabled states, статус и read-only accessibility help в рабочем Browser при ширине окна около 1140 pt. Folder panel выбрала точную QA-папку, но после подтверждения системный Accessibility backend перестал возвращать дерево окна. Поэтому selected/playing UI state и фактический звук не отмечаются как пройденные.

Нужен отдельный ручной smoke на реальном Mac: выбранный WAV слышен в системном устройстве, Stop не сдвигает DAW playhead, переключение Browser/проекта прекращает звук, а VoiceOver ясно сообщает состояние и ошибку. Это не подтверждает профессиональную мониторную latency или совместимость всех форматов кроме поддержанных системой WAV.
