# Минимальный микшер — 1.5.0

15 сентября 2026. Дорожка хранит `gainDb`, `pan`, `muted` и `solo`; проект хранит `masterGainDb`. В карточке дорожки монтаж расположен отдельно от строки `VOL / PAN / M / S`, а master fader находится в собственной верхней секции.

## Семантика DSP

- `mute` всегда выключает дорожку.
- Если хотя бы одна дорожка в solo, звучат только `solo && !muted`.
- Pan в этом срезе — stereo balance: слева сохраняется L и ослабляется R, справа сохраняется R и ослабляется L.
- После clip fades применяется track gain/balance/gate, затем сумма дорожек, master gain и существующий safety clamp.
- Track L/R targets и master target атомарны; изменения сглаживаются примерно за 5 мс без пересоздания playback transport.

Импортированный mono WAV сейчас сразу дублируется в interleaved stereo `Clip`, поэтому исходная channel-layout информация теряется. Equal-power mono pan нельзя честно реализовать до добавления channel metadata в asset/clip model; текущий control подписан и документирован как balance.

## Команды и формат

Pan, mute, solo и master — ревизионные Session-команды с no-op, stale-revision validation и Undo/Redo. C bridge экспортирует поля в `daw_track`/`daw_snapshot` и отдельные setters. Writer сохраняет format v7: `tracks.pan`, `tracks.muted`, `tracks.solo` и `metadata.master_gain`. Reader v1–v6 присваивает нейтральные значения.

## Проверка и границы

Автотесты покрывают диапазоны и non-finite values, no-op, Undo/Redo, v6 defaults, v7 roundtrip, повреждённые pan/master, C ABI, крайние положения balance, solo/mute priority, master gain и точное совпадение offline export с Renderer. Listening acceptance и автоматизированная проверка layout на разных размерах окна остаются открытыми.


Продолжение микшера с buses, sends и graph routing находится в [версии 1.9](35-routing-buses-sends.md). Формат v7 остаётся историей первого mixer slice; routing использует v9, AU inserts — v10, а текущий writer с multi-target automation — format v12.
