# Track reorder — 1.38.0

Дата: 15 сентября 2026.

## Цель

Сделать порядок дорожек частью редактируемого проекта: дорожку можно перенести drag-and-drop за закреплённый заголовок arrangement или командами перемещения для выбранного канала arrangement/mixer, не отделяя её audio и channel state от владельца.

## Контракт

- `Session::moveTrack(id, newIndex, expectedRevision)` и `daw_move_track` проверяют revision, ID и допустимый индекс до изменения state.
- Одна перестановка создаёт ровно один commit и одну Undo entry. Перенос на текущую позицию — no-op без новой revision.
- Перемещается цельный `Track`: media/regions, takes, output/send state, automation, inserts и их parameter lanes остаются у того же track ID.
- Buses, master chain и routing identifiers не меняются. Меняется только порядок `State::tracks`, который writer хранит в `tracks.position`.
- Undo/Redo возвращают точный прежний/новый порядок. `nextID` не меняется и IDs не переиспользуются.

## Проверка

Storage test создаёт три audio tracks с разными clips, takes, volume/pan automation, inserts и plug-in parameter automation. Он переносит третью дорожку в начало и проверяет:

1. save/open сохраняет порядок `Third`, `First`, `Second` и полный owner subtree каждой дорожки;
2. Undo возвращает `First`, `Second`, `Third`;
3. повторный save/open сохраняет исходный порядок и точные durable поля.

Debug/sanitizer CTest, документационные schema/link checks и arm64 app build запускаются в release verification. UI smoke проверяет меню дорожки, клавиатурную команду, selection по ID и одинаковый порядок arrangement/mixer. Сам pointer drag target остаётся отдельной ручной проверкой.

## Границы

Срез не добавляет группы, folders, multi-select reorder, DAWproject migration или изменение signal routing. Он не доказывает listening acceptance, physical device behavior или совместимость реальных сторонних plug-ins.
