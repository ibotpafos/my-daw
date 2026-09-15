# ADR-012: Tracktion Engine за domain/C ABI адаптером

- **Дата:** 2026-09-15
- **Статус:** selected-for-integration-spike
- **Заменяет для новых engine services:** ADR-002

## Контекст

Собственное ядро уже доказало project format, commands/Undo, renderer reference, Core Audio lifecycle, export, takes/comp и full-duplex code path. Дальнейшая самостоятельная реализация buses, plugin graph, PDC, MIDI и format services повторяет зрелую инфраструктуру и отдаляет удобный продукт.

SP-06 собрал Tracktion Engine из exact revision `ff794da4f58e732528b06d7799dad087635de0cb` с его JUCE submodule `37c894f83d379179b2070d437ccd0f1cd9af9576`. Headless proof создал два tracks, импортировал 48 kHz WAV, применил clip gain и loop state, сохранил и повторно открыл edit, затем вывел 48 000 frames при 48 kHz с peak `0.176986`. Запуск после сборки занял 1,25 с; executable — 22 824 536 bytes. Полный checkout и build заняли около 365 MiB. Proof обнаружил path-policy detail: для temporary edit media reference нужно явно сделать absolute, иначе upstream relative heuristic сохранил `../source.wav`.

## Решение

Сохраняем Swift/AppKit UI, собственную domain model, `.mydaw` storage, typed commands и C ABI. Для B-011/B-012 создаём backend-neutral routing/hosting contracts и Tracktion adapter. Существующий native backend остаётся executable reference для parity tests, rollback и full-duplex physical gate.

Tracktion/JUCE не входят в `My DAW.app`, пока не выполнены все gates:

1. session → adapter → render parity на согласованных fixtures;
2. Undo, recovery и stable IDs не зависят от Tracktion ValueTree;
3. callback не делает allocations, locks, I/O или UI calls;
4. binary/startup/RAM укладываются в бюджет;
5. выбран и проверен совместимый GPL/AGPL либо commercial distribution path;
6. dependency revisions, archive hashes и notices воспроизводимы.

## Последствия

Мы ускоряем graph, PDC, plugin hosting, MIDI и render work, но получаем крупную C++ dependency и adapter maintenance. Tracktion предоставляет engine services, а не интерфейс DAW. Migration выполняется по одному vertical slice; переключение всего приложения одной заменой запрещено. При провале parity, latency, recovery или license gate adapter удаляется без миграции пользовательского формата.

Runtime `Engine::getVersion()` в выбранном tree возвращает `3.1.0`, хотя `VERSION.md` и module metadata содержат `3.5.0`. Dependency identity поэтому берётся только из [`dependencies.lock.json`](../../dependencies.lock.json) и Git SHA.
