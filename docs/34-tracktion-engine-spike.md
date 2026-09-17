# Tracktion Engine vertical proof

## Что доказано

Изолированный SP-06 использует настоящий Tracktion Engine и его pinned JUCE submodule. Он автоматически выполняет цепочку, близкую к минимальному DAW backend:

1. создаёт mono PCM24 WAV, 48 kHz, 48 000 frames;
2. создаёт edit с двумя audio tracks;
3. импортирует clip, задаёт `-3 dB` и loop range;
4. сохраняет `.tracktionedit` и открывает его заново в `forRendering` role;
5. делает offline PCM24 render;
6. проверяет 48 kHz, 48 000 frames и ненулевой peak.

Фактический результат: `PASS`, Tracktion runtime string `3.1.0`, два tracks, source/render 48 kHz, 48 000 render frames, peak `0.176986`. Это code proof без открытия output/input device; playback, запись, AU/VST3 scan, PDC и listening acceptance им не доказаны.

## Воспроизводимость и стоимость

Exact git SHAs находятся в [`dependencies.lock.json`](../dependencies.lock.json). Для стабильных архивных зависимостей lock также хранит SHA-256 source archive; быстро меняющийся Tracktion `develop` фиксируется самим commit SHA и его upstream-pinned JUCE submodule. Команды сборки — в [`spikes/tracktion/README.md`](../spikes/tracktion/README.md). На текущей arm64 машине чистый CMake configure занял 23,47 с и около 630 MB max RSS. Первый полный Release build с одной исправленной compile iteration занял 35,66 с; успешная incremental rebuild — 2,89 с. Proof executable весит 22 824 536 bytes; checkout около 229 MiB, build около 136 MiB. Эти цифры описывают один локальный запуск и не являются release benchmark.

Proof содержит 169 строк C++ и 50 строк CMake. Такое сравнение не означает, что полная миграция займёт 219 строк: model adapter, plugin scanner isolation, state migration, latency/recovery и UI contracts остаются отдельной работой.

## Вывод для продукта

Tracktion официально позиционируется как engine без UI и уже включает playback graph, recording/render, MIDI и plugin-format building blocks. Поэтому мы не переносим интерфейс на JUCE. Нативный современный UX остаётся главным продуктовым слоем, а готовые backend services подключаются через наш contract.

B-011 теперь имеет рабочий native reference bus/send routing contract. Следующий B-012 добавит Tracktion adapter parity fixture вместе с AU hosting spike. Только одинаковый результат fixture решит, можно ли затем использовать Tracktion для hosting и PDC. Лицензии остаются promotion gate: Tracktion — GPL-3.0-or-later/commercial, pinned JUCE — AGPL-3.0/commercial.
