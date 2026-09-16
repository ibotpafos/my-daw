# Multi-target automation and workflow preview — 1.13.0

15 сентября 2026. Этот срез расширяет B-013 и впервые проводит B-014 через manifest, public C boundary, preview, domain validation и atomic commit.

## Automation format v12

К volume lane дорожки добавлены track pan, bus gain и master gain. Каждая lane содержит до 2048 строго упорядоченных точек на 48 kHz timeline. Track/bus/master gain интерполируются линейно в dB, pan — линейно в диапазоне -1…1. Пустая lane использует статический control со сглаживанием.

Format v12 хранит новые lanes в отдельных таблицах с owner ID, стабильным position, frame и value. Reader v1–v11 открывает старые проекты с пустыми новыми lanes. UI показывает количество точек у соответствующего track, bus или master control; volume и pan curves дорожки рисуются поверх waveform.

Pre-fader send остаётся до track fader/pan automation. Post-fader send получает автоматизированный stereo level. Bus и master lanes применяются до master Audio Unit chain. Offline master latency compensation из 1.12 сохраняется.

## AU cache freshness

Scanner helper передаёт component tuple, путь `.component` bundle и `CFBundleShortVersionString`/`CFBundleVersion`. При загрузке persistent cache отдельная helper-enumeration сравнивает tuple/path/version. Обновлённые, перемещённые и удалённые entries исключаются; UI предлагает повторный scan. AU instances при freshness check не создаются.

## Workflow v0

Bundle содержит `org.mydaw.vocal-preparation` manifest. Host проверяет API version, ID, capabilities и разрешённый command-palette action. Пользователь выбирает дорожку, имя и gain. C bridge принимает bounded typed operations, а domain строит read-only before/after preview на ожидаемой revision.

После подтверждения тот же batch снова полностью проверяется и применяется одной project revision с одной Undo entry. Неизвестная operation, отсутствующая дорожка, неверное имя, gain вне диапазона или stale revision отклоняют весь batch без частичных изменений.

Текущий встроенный workflow подготавливает от одной до 32 выбранных дорожек сразу: `daw_preview_vocal_preparation` принимает массив ID, первый выбранный становится `Lead`, остальные `Double 1…N`, и предложения по peak/RMS считаются для каждой дорожки (multi-selection и peak/RMS suggestions из B-015 уже в ABI; актуальный контракт с числовыми границами — в [40 vocal preparation workflow](40-vocal-preparation-workflow.md)). Внешний executable/runtime не запускается: первый модуль остаётся bundled declarative recipe.

## Границы

- touch/latch/write recording automation ещё нет;
- AU parameter automation и graph-wide PDC ещё не реализованы;
- cache metadata для built-in/unmapped components может быть пустой;
- отдельный QA, физический и listening pass по указанию пользователя не запускались.
