# 76. Редактирование клипов и дорожек (срез 1.49.0)

## Что это
Домашняя арка «удобно как в любой DAW»: клип и дорожка получают полный набор свойств и операций уровня редактора — цвет, clip gain (dB), mute/solo/gain/duplicate дорожки, цвет/transpose/quantize MIDI-клипов — через domain-команды, черновик v19, C-ABI и AppKit UI. Деструктивности нет: всё — ревизии + один Undo-шаг.

## Домен (`engine/domain`)
Десять команд общей конвенции (expected-revision, no-op при идентичном значении, один snapshot на commit):
- дорожки: `setTrackMuted`, `setTrackSolo`, `setTrackColor`, `setTrackGain` (тот же clamp −60..12 dB, что у master/bus), `duplicateTrack` → новый id (копируются regions, takes, MIDI, routing, automation, inserts; имя — «{name} copy», cap 256).
- аудио-клипы (regions): `setClipColor`, `setClipGain` — поле `Region.gain` в dB по умолчанию 0.0.
- MIDI-клипы: `setMidiClipColor`, `transposeMidiClip` (int8 полутонов, питчи клампятся 0..127, all-clamped = no-op без ревизии), `quantizeMidiClip` (snap старта нот к кратным `gridBeats` через beats↔frames темпо-карты; grid≤0 = no-op).
`Track.color`/`Region.color`/`MidiClip.color` — uint32 0xRRGGBB, 0 = «цвета нет». trim/split MIDI сохраняют цвет обеих половин.

## Хранение (черновик v18→19)
`tracks.color`, `regions.gain,color`, `midi_clips.color` — все колонки NOT NULL с дефолтом по гейту `formatVersion>=19`; v18 и старее открываются с нулями. Гейт читалки, writer-порядок и corrupt-матрица — по конвенции v17/v18 (условные SELECT-строки, push_back по 0-based индексам колонок).

## Рендер
`Region.gain` складывается в голос: `Voice.gain` — линейный множитель (dB→linear при prepare, −120 dB и ниже = точный ноль), перемножается с fade-огибающей в mix-in до фейдера дорожки. Пустой дефолт (0 dB) даёт ровно 1.0 — бит-совместимость прежних проектов не тронута (ctest `audio_render_storage` зелёный без правок старых секций). Fades и crossfade уже применялись; `setCrossfade` закрыт ранее.

## Мост (v19 ABI)
Восемь новых вызовов: `daw_set_track_color`, `daw_duplicate_track` (новый id через out-параметр), `daw_set_clip_color`, `daw_set_clip_gain`, `daw_set_midi_clip_color`, `daw_transpose_midi_clip` (int32 аргумент, ±127 валидируется до домена), `daw_quantize_midi_clip` (grid ∈ (0,64] бит). Структуры расширены append-полями: `daw_track.color`, `daw_clip.color/gain_db`, `daw_midi_clip.color` (версия 2). Читалки заполняют поля; негативы ABI (нет regions → `Audio clip not found`, NULL out-параметр, версия+1, grid 0) не тратят ревизию — `pure_c_bridge` прогоняет живую sequence rev 10→17.

## UI (1.49.0)
- **Клип**: правая кнопка на волне — контекстное меню (цвет из палитры 8+сброс, «Громкость клипа…» диалог dB, дубль/split/удаление). В фокусе волны — горячие `S` (split в позиции курсора), `D` (дублировать), `Delete` (удалить); без модификаторов, Cmd-S остаётся «Сохранить». Durable-краска перекрывает акцент дорожки; метка выбранного клипа показывает `· −6.0 dB` при ненулевом гейне.
- **Дорожка**: «•••» меню — «Дублировать дорожку», «Цвет дорожки…» (палитра у курсора); цвет хедера, номера строки, микшерного стрипа и волны — из `track.color` с fallback на палитру по позиции.
- **MIDI**: те же «•••» — «Транспонировать MIDI-клип…» (полутоны −127…127), «Квантовать MIDI-клип…» (1/4, 1/8, 1/16, 1/32, 1/4-триоль, 1/8-триоль), «Цвет MIDI-клипа…». Цель — клип, выбранный в инспекторе пиан-ролла (`midiClipIndex`), иначе первый.
- Меню «Проект» — пункты разделения/дублирования/удаления выбранного клипа и «Дублировать дорожку» (Cmd+Shift+T) с `validateMenuItem`-гейтами по выбранной дорожке и записи.

## Буфер обмена и перенос между дорожками (1.51.0)
Четыре revision-команды домена: `copyClipToTrack` / `moveClipToTrack` (аудио) и `copyMidiClipToTrack` / `moveMidiClipToTrack`. Копия сохраняет всю запись региона (offset/fades/take/gain/color) с новым стартом; move стирает источник — одиночный регион не может покинуть дорожку (то же правило, что у deleteClip). Аудио-вставка на другую дорожку законна только когда целевая дорожка резолвит **тот же shared-источник на том же take-start** (take 0 — базовое аудио дорожки, индексы take локальны дорожке; совпасть могут дорожка-источник, дорожки с общими дублями и duplicateTrack-копии). Пересечения регионов и перекрытия MIDI по-прежнему решает `validate()` — команды ничего не дублируют в правилах. Хранение не менялось: v19 уже пишет регионы и MIDI-клипы целиком.
Мост: `daw_copy_clip_to_track` / `daw_move_clip_to_track` / `daw_copy_midi_clip_to_track` / `daw_move_midi_clip_to_track` (все expected-revision, геометрия → `resetTransport`). Живая секция `pure_c_bridge`: reject-пути аудио + полное MIDI-путешествие lane/notes/colour с undo.
UI: контекстное меню клипа — «Копировать клип (C)», «Копировать на дорожку ▸» и «Перенести на дорожку ▸» (подменю всех дорожек с последними именами); хоткеи `C`/`V` в фокусе волны (вставка — к снапнутой позиции курсора); «Проект → Копировать/Вставить выбранный клип» с теми же гейтами; меню «•••» — «Перенести MIDI-клип на дорожку…» (палитра целей у курсора) и «Копировать MIDI-клип». Буфер — собственный снимок (дорожка, индекс, флаг MIDI), paste перечитывает актуальный источник через домен: удалённый источник честной ошибкой.

## Тесты
`clip_track_model` (17-й CTest, 42 CHECK): все десять команд — ревижн-матрица, no-op'ы, undo/redo до состояния импорта и обратно, duplicateTrack (новый id, «copy» в имени, копия regions/MIDI с цветами; `Track not found`; общий cap 256 не гонялся — унаследован), transpose с клампом обоими краями (127 сверху, 0 снизу), quantize (on-grid не двигается, 0.75 бит → вверх, grid≤0 no-op), storage round-trip v19 с обоими цветами и −9 dB. Секции clip-gain (−6.0206 dB = ровно ×0.5 на миксе) и fade-in огибающей — в `audio_render_storage`; clip/track sequence + C-ABI негативы — в `pure_c_bridge`.

## Гейты кода
debug 17/17, ASan/UBSan 16/16, TSan 16/16 на слитом дереве; сборка AppKit + 4 s launch — зелёные. Физические гейты (мышь по новым контролам, слух clip gain) остаются открытыми.
