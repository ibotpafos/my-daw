# Track/bus inserts and graph PDC

Версия 1.18.0 завершает основной channel-insert срез B-012/B-013. Дорожки, buses и master теперь используют одну модель ordered Audio Unit/VST3 chains, один C ABI и одинаковые операции добавления, параметров, bypass, перестановки и удаления. SQLite format v13 сохраняет эти цепи, renderer реально обрабатывает их в routing graph, а DAWproject переносит devices и state на соответствующие channels.

## Project model и команды

`Track.inserts`, `Bus.inserts` и master chain содержат общие `PluginInsert`. Insert имеет глобально уникальный ID, format identity, имя, opaque state, bypass и сохранённую latency. Domain ограничивает каждую owner chain четырьмя effects, весь проект — 64 inserts, отдельный Audio Unit state — 1 MiB, VST3 state и суммарный plug-in state проекта — 16 MiB.

Добавление, перестановка, bypass, обновление state и удаление проходят через expected revision и обычный Undo/Redo. Повтор same-position move, прежнего bypass или идентичного state/latency остаётся no-op и не создаёт лишнюю revision. Missing effect остаётся в проекте и пропускает dry signal.

SQLite format v13 использует одну `channel_plugins` table с owner kinds track, bus и master, owner ID и стабильной position. Writer больше не создаёт отдельную legacy master table. Reader v1–v12 сохраняет прежнюю совместимость: старые master inserts загружаются, а отсутствующие в старом формате track/bus chains становятся пустыми.

## Signal flow и block renderer

Renderer заранее подготавливает AU/VST3 instances и planar scratch buffers на control thread. Callback делит запросы на сегменты до 4096 frames и не выделяет память и не берёт locks. Для каждого сегмента он выполняет:

1. materialize clips каждой активной дорожки;
2. track insert chain один раз до volume, pan и mute;
3. pre-fader sends от post-insert/pre-fader signal, post-fader sends после channel controls;
4. main routes и sends через подготовленные PDC rings;
5. каждый bus insert chain один раз в порядке валидированного DAG, затем bus gain/pan/mute и compensated output route;
6. master gain и общую master insert chain.

Tail path подаёт тишину через все channel chains и PDC rings. Большие render requests безопасно сегментируются тем же block size.

## Runtime latency compensation

При подготовке renderer читает фактическую latency каждой успешно активированной AU/VST3 instance, суммирует её внутри track, bus и master chains и добавляет к внешнему `GraphLatencyPlan`. Для каждой track-main, send и bus-output edge planner задерживает более короткий путь до максимальной входящей latency следующего узла.

Ограничение составляет 10 секунд на edge или path и 20 секунд aggregate stereo delay frames, около 7.3 MiB float storage. Невалидная суммарная latency отклоняется при prepare; недоступный effect остаётся разрешённым dry fallback и сообщает plug-in error отдельно.

## C ABI и macOS UI

Generic insert API принимает owner kind `MASTER`, `TRACK` или `BUS` и owner ID. Он предоставляет добавление AU/VST3, enumeration, параметры, state update, bypass, move и remove. Master использует owner ID 0; track и bus требуют ненулевой ID. Некорректная owner pair отклоняется до catalog lookup и mutation. Старый master API сохранён для совместимости.

Каждая track и bus strip получила сворачиваемую секцию `INSERTS N`. В раскрытом состоянии доступны общий AU/VST3 picker, scan status, format badge, active/bypassed/missing state, latency, generic parameters, up/down и remove. В свёрнутом состоянии остаётся компактный disclosure, чтобы mixer не превращался в длинный список controls.

## DAWproject

Каждый effect экспортируется в `<Devices>` Channel своего track, bus или master. State paths исключают collisions между owners:

- `plugins/track-<owner>-<plugin>.aupreset` или `.vstpreset`;
- `plugins/bus-<owner>-<plugin>.aupreset` или `.vstpreset`;
- `plugins/master-<plugin>.aupreset` или `.vstpreset`.

VST3 state использует стандартный `.vstpreset` chunk layout; внутренний MDVS envelope наружу не переносится.

## Оставшиеся границы

В [1.19.0](46-plugin-automation-tail-export.md) появились plug-in parameter automation и bounded finite VST3 tail export. Runtime effects пока выполняются в процессе приложения. Следующие срезы: runtime process isolation, vendor editors и compatibility matrix, realtime playhead offset, Touch/Latch для plug-in controls и infinite-tail policy. Instruments, MIDI и sidechain остаются отдельными задачами.

В этом срезе выполнена compile/build проверка. QA, запуск приложения, прослушивание, hardware и сторонние vendor plugins не выполнялись по текущему режиму разработки.
