# Routing, buses и sends — 1.9.0

15 сентября 2026. B-011 получил первый сквозной routing slice: domain model, realtime renderer, C bridge, SQLite format v9 и AppKit UI используют один контракт. Пользователь может создать до 16 bus, направить дорожку или bus в Master/другой bus и добавить до 8 sends на дорожку.

## Модель и команды

`outputBus == 0` означает Master. Дорожка имеет один main output и уникальные sends на существующие buses. Bus имеет gain, stereo balance, mute и один main output. Все имена, уровни и маршруты меняются ревизионными Session-командами с stale-revision check, no-op и Undo/Redo.

Перед commit Session проверяет все ссылки и запускает DFS по bus outputs. Неизвестная цель, self-route и любой цикл отклоняются до замены состояния. Ограничения прототипа: 256 дорожек, 16 buses, 8 sends на дорожку; gain `-120…+24 dB`, pan `-1…+1`.

## Realtime исполнение

`Renderer::prepare` компилирует ID в индексы, send gains и порядок bus graph алгоритмом Kahn. Callback работает с заранее выделенными фиксированными track/bus buffers и не обходит hash maps, не выделяет память и не блокируется. Gain и pan дорожек/buses сглаживаются; структурное изменение routing останавливает transport и публикует новый plan.

Post-fader send снимает сигнал после track gain/pan/mute/solo. Pre-fader tap обходит gain и pan, но подчиняется общему mute/solo gate с тем же коротким smoothing. Bus sends, bus solo, meters, inserts, sidechain и PDC ещё не реализованы.

## Формат проекта и UI

Format v9 добавляет `tracks.output_bus`, таблицу `buses` и таблицу `sends`. Reader v1–v8 открывает старые проекты с выходом всех дорожек в Master и пустым bus graph. Writer сохраняет позиции buses/sends и использует прежний atomic publish проекта.

В каждой дорожке есть Output, создание Send, уровень и переключатель PRE/POST. Ниже таймлайна buses показаны отдельными strips с редактируемым именем, mute, volume, pan и output. Popup items несут стабильные ID, поэтому переименование не меняет маршруты.

## Проверка и следующий шаг

Domain/C bridge тесты покрывают создание, редактирование, Undo/Redo, отсутствующие цели, self-route и цикл. Storage тест проверяет v9 roundtrip. DSP fixture проверяет track→bus и параллельный post-fader send с ожидаемой амплитудой. Debug, ASan/UBSan и TSan входят в локальный gate.

Tracktion пока не включён в shipping app. Проверенный SP-06 используется как готовый backend candidate; B-012 должен выразить этот graph через adapter, сравнить fixture с native renderer и только после parity подключать AU scan/host/state/latency. Физическое прослушивание и hardware latency этим срезом не доказаны.
