# Design system и режимы workspace — 1.27.0

Этот срез переводит рабочее окно на единый визуальный контракт. `apps/macos/DesignSystem` содержит semantic colors, typography, spacing, radii, состояния controls, 38 смысловых SF Symbol mappings с русскими accessibility labels и data-driven primitives для waveform, meter и grid. Manifest и четыре SVG-листа копируются в bundle как документированный fallback; рабочие meters и waveform продолжают рисоваться только из данных engine/UI model.

Верхняя панель стала компактнее: запись, Play, Stop, rewind, loop и Undo/Redo используют системные scalable icons, а повторные текстовые кнопки удалены. Режимы `Создание`, `Запись`, `Сведение` и `Мастеринг` теперь меняют реальные пропорции workspace. Создание и запись отдают больше места arrangement; сведение расширяет mixer; мастеринг также выбирает Master и открывает его channel context в Inspector. Выбранный режим сохраняется локально.

Высота обычной track lane уменьшена со 132 до 92 points, поэтому три дорожки вместе с waveform остаются видимыми в стандартном окне. Импорт WAV автоматически выбирает новую дорожку и клип, открывая их параметры в Inspector. Mixer canvas использует верхнюю систему координат и адаптивную высоту fader: compact dock сохраняет названия и R/M/S, а mixing mode показывает полноценные channel strips.

Локальный UI QA выполнен на реальном окне 1240×800 в нескольких состояниях: пустой draft; две дорожки и bus; три дорожки, bus и сгенерированный stereo WAV; выбранный clip Inspector; compact mixer; расширенный mixing mode; Master context. Импорт проходил через явный `NSOpenPanel` и не вызвал автоматического запроса доступа ко всей папке «Документы». Play/Stop был проверен в предыдущем 1.26 lifecycle-прогоне; прослушивание, запись с физического входа, VoiceOver audit и пользовательская acceptance-сессия не выполнялись.

QA свежего worktree также обнаружил, что macOS build без локально установленного pinned VST3 SDK компилировал bridge, но не определял host functions. Добавлен явный VST3 stub: такой build теперь линкуется и сообщает, что hosting требует SDK, вместо undefined symbols. Build с SDK продолжает использовать полноценный `vst3_effect.cpp`.
