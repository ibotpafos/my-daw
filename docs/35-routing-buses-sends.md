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

## Группировка из UI (1.66.0)

Шина — это и есть папка/группа: волна закрыла последний разрыв, где движок уже умел всё, а UI нет. Мост получил `daw_create_bus(session,name,&out_id,rev)` (пустое имя — «Bus name required», NULL — штатный отказ; id возвращается последним автоинкрементом; пустая шина не трошает транспорт, только `cancelStalePlaybackPreparation` как у соседей) и `daw_get_bus_count`; `daw_get_bus` уже был. В шапке дорожки — пункт «Группировка в шину…»: всплывающее меню предлагает «Новая шина «Группа N»», список существующих шин с галочкой текущей маршрутизации и «Мастер (без шины)» для возврата. Маршрутизация — штатный `daw_set_track_output` (он же `routeTrack` в домене, со своей валидацией цикла).

Проверка: в `session_storage_bridge` блок шины покрыл создание («Группа 1» читается из daw_bus.name, id отличный от существующей), enumerate по count, «Bus index out of range», ABI-mismatch, пустое/NULL имя и NULL count-выход. Важный нюанс тестов: положительный `daw_create_bus` тратит ревизию, и последующий `daw_add_master_au` в том же блоке падает на устаревшем `snap.revision` — после мутаций обязателен свежий snapshot. Матрица MIX-02 дополнена; физический гейт (слышать сумму группы на реальном проекте) остаётся открытым. Multi-select-группировка (сразу несколько дорожек в шину) не делалась: одна запись = одна операция назначения.

## Удаление шин из UI (1.68.0)

Кнопка «✕» в правом верхнем углу bus-стрипа в микшере (скрыта для track и master). Нажатие вызывает алерт подтверждения с именем шины и описанием последствий: «Дорожки, направляемые в эту шину, будут перенаправлены на мастер. Sends на эту шину будут удалены.» При подтверждении `daw_delete_bus(session, id, rev)` → refresh + pollTransport.

Домен `deleteBus(id, expected)`: перенаправляет `track.outputBus` и `bus.outputBus` на мастер (id=0), удаляет `send.bus == id` из всех дорожек, стирает шину из вектора. Валидация: check(expected), «Bus not found».

Мост `daw_delete_bus(session, bus_id, rev)`: guard + `model.deleteBus` + `cancelStalePlaybackPreparation`. C-гейт: NULL сессии отклоняется.

Предельные условия: удаление шины с sends и автоматизацией — sends удаляются, автоматизация шины пропадает вместе с ней; мастер удалить нельзя (кнопка скрыта).

## Solo Exclusive и клавиатурные хоткеи (1.69.0)

Solo exclusive (как в Studio One/Logic Pro): Option+клик на Solo солирует только эту дорожку, остальные молчат. При повторном Option+клик — все снимают solo. Обычный клик работает как раньше (toggle). Реализовано в `mixerSetSolo`: если `NSEvent.modifierFlags.contains(.option)`, сначала снимает solo со всех дорожек, потом включает solo на выбранной (или оставляет все без solo если клик на уже solo-дорожке).

Хоткеи в меню «Проект» (состав на 1.72.0, подробности в [51](51-pinned-headers-keyboard.md)):
- **Space** — Воспроизведение/Стоп (toggle; во время записи завершает тейк)
- **⌥S** — Solo выбранной дорожки (toggle)
- **⌥M** — Mute выбранной дорожки (toggle)
- **⌥A** — Arm выбранной дорожки (toggle)

Буквы без модификатора остались у слоя волны (S разрезать клип, M mute клипа, D дубль, C/V копировать/вставить, L loop): эквивалент главного меню перехватывает клавишу раньше `view.keyDown`, поэтому голые **S** и **M** для дорожек ломали бы монтаж. Все четыре клавиши отключаются, пока фокус в текстовом поле.
