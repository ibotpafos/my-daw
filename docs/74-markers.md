# 74. Маркеры-локаторы (срез 1.50.0)

## Что это
Именованные позиции на общем таймлайне 48 кГц: точки навигации/референсы (`Marker{frame,name}`). В отличие от темпо-карты дорожка может быть пуста: default state валиден без маркеров — их отсутствие не инвариант.

## Домен (`engine/domain`)
`State.markers` — вектор, отсортированный СТРОГО по кадру, кадры уникальны, `frame<kMaxMidiFrame`, `kMaxMarkersPerProject=256`. Команды по конвенции темпо-блока: `addMarker` (дубль кадра — `'A marker already exists at this position'`, 257-й отклоняется до копирования State), `removeMarker`, `renameMarker` (no-op-if-same). Все expected-revision, undo-стек общий; move = remove+add (v0). Имя валидируется ОБЩИМ `validateName()` (строгий UTF-8, 1–120 кодопозиций, NUL/control/overlong/surrogate отсекаются) — тем же, что у дорожек/шин/тейков: новых лимитов не вводилось намеренно.

## Хранение
Черновик v17→18: таблица `markers(frame INTEGER PRIMARY KEY, name TEXT NOT NULL)`. Читалка: гейт `>=18`, legacy v17 открывает проект с пустыми маркерами (легально), PK+`ORDER BY frame` снимают вопросы порядка/уникальности, пределы-строка до validate(), остальное — validate() как источник истины.

## Тесты (`marker_model`, 36 CHECK)
reject-матрица без расхода ревизии (дубли кадров, frame==2^40/+1, пустое/длинное имя, NUL, \xFF, truncated/overlong/surrogate UTF-8, revision-конфликт), сортировка при вставке front/middle/back, undo/redo трёх-командной историей, cap 256 через команды И через replace()/validate(), RU round-trip с edge-кадром, v17-fallback, шесть corrupt-кейсов (BLOB-имя, frame=-1, DESC-переукладка...).

## Мост (срез 1.50.0)
`daw_marker{struct_size,version,frame,name[128]}` — 144 байта, `DAW_MARKER_VERSION=1`. Читалки `daw_get_marker_count` / `daw_get_marker(index)` в конвенции темпо-геттеров (проверка только `struct_size`; Swift-инициализаторы `version` не выставляют). Команды `daw_add_marker` / `daw_rename_marker` / `daw_remove_marker` — expected-revision, `required(name)`, все пределы держит домен. Дорожка не влияет на звук и подготовку, поэтому команды без `cancelStalePlaybackPreparation`/`resetTransport` — как MIDI-ноты.

## UI (срез 1.50.0)
Верхняя полоса линейки (16px): жёлтый флажок, имя справа, вертикальная нить на всю линейку. Клик по флажку — переход к маркеру; двойной клик по полосе — «Новый маркер» (позиция снапается к сетке, имя по умолчанию «Маркер N», 1–120 символов проверяет домен); правый клик — меню «Перейти / Переименовать… / Удалить». «Проект → Добавить маркер в позицию курсора» (Cmd+Shift+M). Данные перечитываются из домена в `refreshBarMarks()` — линейка только отображает срез после команд/Undo/Load.

## Открытые гэпы
Snap-to-marker при арминге/записи, перенос флажка за рукоять (v0 — remove+add двумя командами), VoiceOver и мышь по новой полосе, `.mydaw` zip-пакет на маркерах не прогонялся; физическая приёмка переходов на реальном проекте.

## Гейты кода
Срез 1.46.0: debug 16/16, ASan/UBSan 15/15, TSan 15/15 на слитом дереве; core/tempo-ассерты версии обновлены (одна санкционированная строка в tempo_model_tests + комментарий), PASS-тексты — на merge.
Мост и UI (1.50.0): debug 17/17 (живая секция `pure_c_bridge`: 2×add, rename, no-op rename, readback с NUL-границей, reject дубля кадра и пустого имени, remove, undo/redo), ASan/UBSan 16/16, TSan 16/16, сборка приложения без warnings + 4-сек запуск. Открыто: физическая мышь/клавиатура по полосе маркеров, переходы на слух в реальном проекте.