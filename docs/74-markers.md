# 74. Маркеры-локаторы (срез 1.46.0)

## Что это
Именованные позиции на общем таймлайне 48 кГц: точки навигации/референсы (`Marker{frame,name}`). В отличие от темпо-карты дорожка может быть пуста: default state валиден без маркеров — их отсутствие не инвариант.

## Домен (`engine/domain`)
`State.markers` — вектор, отсортированный СТРОГО по кадру, кадры уникальны, `frame<kMaxMidiFrame`, `kMaxMarkersPerProject=256`. Команды по конвенции темпо-блока: `addMarker` (дубль кадра — `'A marker already exists at this position'`, 257-й отклоняется до копирования State), `removeMarker`, `renameMarker` (no-op-if-same). Все expected-revision, undo-стек общий; move = remove+add (v0). Имя валидируется ОБЩИМ `validateName()` (строгий UTF-8, 1–120 кодопозиций, NUL/control/overlong/surrogate отсекаются) — тем же, что у дорожек/шин/тейков: новых лимитов не вводилось намеренно.

## Хранение
Черновик v17→18: таблица `markers(frame INTEGER PRIMARY KEY, name TEXT NOT NULL)`. Читалка: гейт `>=18`, legacy v17 открывает проект с пустыми маркерами (легально), PK+`ORDER BY frame` снимают вопросы порядка/уникальности, пределы-строка до validate(), остальное — validate() как источник истины.

## Тесты (`marker_model`, 36 CHECK)
reject-матрица без расхода ревизии (дубли кадров, frame==2^40/+1, пустое/длинное имя, NUL, \xFF, truncated/overlong/surrogate UTF-8, revision-конфликт), сортировка при вставке front/middle/back, undo/redo трёх-командной историей, cap 256 через команды И через replace()/validate(), RU round-trip с edge-кадром, v17-fallback, шесть corrupt-кейсов (BLOB-имя, frame=-1, DESC-переукладка...).

## Открытые гэпы
C-ABI маркеров нет (мост/UI — следующая арка: отображение на линейке + переход по двойному клику), snap-to-marker при записи, crash-safe save-журнал наследует общий черновик без отдельной проверки, `.mydaw` zip-пакет на маркерах не прогонялся.

## Гейты кода
debug 16/16, ASan/UBSan 15/15, TSan 15/15 на слитом дереве; core/tempo-ассерты версии обновлены (одна санкционированная строка в tempo_model_tests + комментарий), PASS-тексты — на merge.