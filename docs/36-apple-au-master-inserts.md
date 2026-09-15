# Apple Audio Unit master inserts — 1.10.0

15 сентября 2026. Первый B-012 vertical slice использует готовые системные DSP-компоненты Apple через AudioComponent/AudioUnit API. Master принимает до четырёх inserts; один и тот же prepared effect chain вызывается из общего Renderer для Core Audio playback и offline WAV export.

## Поддерживаемый каталог

Начальный allowlist ограничен тремя системными effects: `AUDynamicsProcessor`, `AUPeakLimiter` и `AUMatrixReverb`. UI не перечисляет произвольные установленные плагины. Локальный запуск `auval -a` загрузил сторонние bundles и показал конфликты классов, поэтому общий discovery/validation остаётся задачей disposable scanner с timeout и quarantine, а не main process.

Пользователь явно выбирает эффект в секции Master. Control path проверяет component tuple `type/subtype/manufacturer`, создаёт AU, настраивает stereo non-interleaved Float32, 48 kHz и maximum 4096 frames, получает state и latency. Произвольный component ID через C ABI не принимается: перед добавлением он должен присутствовать в approved catalog текущей сессии.

## State, latency и failure policy

Binary property list из `kAudioUnitProperty_ClassInfo` хранится в domain как bounded blob до 1 MiB на insert и до 4 MiB на проект. Format v10 добавляет таблицу `master_plugins` с stable ID, component tuple, именем, bypass, latency frames и state blob. Reader v1–v9 открывает старые проекты с пустой insert chain.

Latency из `kAudioUnitProperty_Latency` переводится в frames при 48 kHz и показывается в UI. Этот срез её наблюдает, но пока не выполняет PDC: компенсация parallel paths и tail policy входят в B-013. Параметры/editor ещё не меняются, поэтому сохраняется безопасный initial state; state refresh после parameter edit остаётся обязательным продолжением B-012.

Перед playback/export Renderer создаёт и инициализирует effects вне realtime callback. В callback заранее выделенные input buffers питают AU render callback. Если `AudioUnitRender` возвращает ошибку, effect восстанавливает dry block, Renderer увеличивает `pluginErrors`, а transport показывает `AU error: dry fallback`. Автоматический bypass в сохранённой модели не выполняется.

## Проверка

`daw_au_host_probe` — отдельный executable, который для каждого approved AU выполняет discovery, instantiate, format configuration, state capture/restore, четыре render block и проверку finite output без открытия аудиоустройства. Проверенный локальный результат:

- Apple AUDynamicsProcessor: state 234 bytes, latency 256 frames;
- Apple AUPeakLimiter: state 175 bytes, latency 96 frames;
- Apple AUMatrixReverb: state 328 bytes, latency 0 frames.

Domain/C bridge тесты покрывают add/bypass/remove, Undo/Redo, allowlist и ABI. Storage тест проверяет format v10 roundtrip. DSP integration test запускает approved AU внутри общего Renderer и проверяет ненулевой finite output без plugin errors. Эти проверки не доказывают качество звучания, сторонние AU, editor lifecycle, hang/crash recovery или realtime deadline на физическом устройстве.
