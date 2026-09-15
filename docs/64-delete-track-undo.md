# Delete track with Undo — 1.37.0

Дата: 15 сентября 2026.

## Цель

Дать рабочему проекту базовую операцию удаления дорожки, не превращая её в частичную очистку отдельных клипов. Пользователь удаляет выбранную дорожку из arrangement или mixer и может вернуть её обычным Undo.

## Контракт

- `Session::removeTrack(id, expectedRevision)` и `daw_remove_track` проверяют revision и существование ID до изменения state.
- Операция создаёт ровно один commit и одну запись Undo history.
- Из state убирается весь `Track`: исходный clip и regions, base start, takes, routing output, sends, volume/pan automation, inserts и их parameter-automation lanes.
- `Bus` и master state не изменяются. Они не содержат обратных ссылок на track; удалённый track поэтому не оставляет durable rows в track-scoped SQLite tables.
- Undo восстанавливает ровно прежний snapshot дорожки. Redo снова её удаляет. `nextID` остаётся монотонным по обычному правилу Session и удалённый ID не переиспользуется новым track.
- SQLite schema и `PRAGMA user_version` не меняются: writer уже записывает только текущие tracks и зависимые rows, reader восстанавливает только их.

## UI

Удаление доступно для выбранной дорожки в arrangement и mixer, а Command-Delete оставлен отдельной командой от обычного Delete клипа. Операция сразу обратима через Command-Z; после успешного commit selection переводится на ближайшую оставшуюся дорожку либо очищается, если проект пуст. Transport prepared state обновляется через существующую mutation boundary.

## Проверка

Storage job test создаёт project с удаляемой и surviving audio-track, включая takes, automation и insert. Он проверяет:

1. save/open сразу после удаления: в SQLite snapshot остаётся только surviving track с точными media, takes, automation, inserts и plug-in parameter automation;
2. Undo: удалённая дорожка возвращается вместе со всем своим state;
3. save/open после Undo: обе дорожки и их durable state снова присутствуют.

В составе release также запускаются обычные Debug и sanitizer CTest, документационные ссылки/schema check и arm64 macOS bundle build. UI smoke подтверждает доступность команды и корректный selection transition; это отдельная проверка от storage round-trip.

## Границы

Это не корзина, deferred media cleanup или необратимое освобождение файлов: media живёт в immutable snapshots и обычной bounded Undo history. Срез не добавляет DAWproject migration, не меняет realtime DSP или схему проекта. Он не доказывает listening acceptance, hardware output/recording path или совместимость реальных сторонних plug-ins.
