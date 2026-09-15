# 69. Темпо-карта (срез 1.41.0)

## Что это
Темп и размер стали частью проекта: карта точек по таймлайну, beat↔frame перевод, хранение и C-ABI. До этого темп был только цифрой в UI.

## Модель (`engine/domain`)
`TempoPoint{frame,bpm}` и `TimeSignaturePoint{frame,numerator,denominator}` (denominator — степень 2 из {1,2,4,8,16,32}, numerator 1..32) в `State.tempo`/`State.timeSignatures`. Конвенция: бит во всех beat-формулах — четвертная (framesPerBeat=2 880 000/bpm); такт содержит `numerator×4/denominator` четвертных (4/8 → 2 бита, 4/4 → 4). Дефолтные якоря 120 BPM и `{4,8}` вшиты в default state — пустой набор невалиден, frame строго возрастают и < 2^40, bpm ∈ (20, 999], не более 64 точек каждого рода. Команды expected-revision с upsert/no-op-if-equal: `setTempoAt`, `removeTempo` (нулевую точку не удалить — 'The first tempo point cannot be removed'), `setTimeSignatureAt`, `removeTimeSignature`.

## Beat-арифметика
const-методы State: `bpmAtFrame` (last-point-holds), `beatsAtFrame` (били от frame 0 как сумма сегментов `frames·bpm/2880000` с long-double аккумуляцией), `frameAtBeats` (обратная, nearest-rounding, throw вне диапазона). Точность: все кадры <2^40 точны в double; ошибка обратного хода на краю таймлайна ≤1 кадр (задокументировано, тест проверяет round-trip и край 2^40).

## Хранение и мост
Черновик `user_version` 16→17: таблицы `tempo_points(frame PK,bpm)` и `time_signature_points(frame PK,numerator,denominator)`; черновики ≤16 открываются дефолтными картами (тест legacy-fallback на живых MIDI-дорожках). C-ABI: `daw_set_tempo`/`daw_remove_tempo`/`daw_set_time_signature`/`daw_remove_time_signature` (style MIDI-блока, cancelStalePlaybackPreparation) + постраничных запросов нет — счётчики малы: `daw_get_tempo_count`/`daw_get_tempo_point(index,out)` и пара для размеров; POD'ы версионированы.

## Границы среза
UI пока не переваривает карту: сетка и степпер темпа живут в `main.swift` по-старому, snap-to-beat и барная линейка — отдельная арка; метроному/клику нужна эта карта — следующий слой. Экспорт/рендер темпа не читают (аудио без BPM-понятия). Честные гейты: GUI-проверки, слуховая приёмка.

## Гейты
debug CTest **15/15** (`tempo_model`: команды/валидации/инверсии/undo/round-trip v17/fallback v16/bridge-дым), sanitizers **14/14**, TSan **14/14**, сборка и запуск приложения зелёные.