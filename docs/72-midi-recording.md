# 72. Запись MIDI в клип (срез 1.44.0)

## Что это
Движковый контур записи исполнения: поток событий транспорта → MidiRecorder (buffer во время хода) → батч нот → доменная команда append → durable через черновик v17. Файла у MIDI-тейка нет намеренно: ноты живут в проекте.

## Домен
`Session::appendMidiNotes(trackID, clipIndex, batch, expectedRevision)`: пустой батч — тихий no-op без ревизии; >65536 отклоняется ДО копирования State; дальше insert + обычный `commit→validate()` — те же инварианты, что у соседей: клип НЕ растёт (нота за окном — reject, пиши в клип нужной длины или режь батч предикатом `midiRecordBatchFits`), порядок захвата сохраняется (сортировка модели не требуется), одна команда = один undo, бюджет 65536 нот/проект держит validate().

## MidiRecorder (`engine/audio/recording.*`, рядом с RecordingWriter)
`arm/disarm/feed/stop(stopFrame)` + счётчики `recordedNotes/openNotes/dropped/unmatched`. note-on открывает pending; note-off закрывает НОВЕЙШУЮ совпадающую (pitch,channel) — LIFO для повтора одного тона; висящие закрываются на stop-кадре; длина ≥1 и ≤ 480000; кадры clip-relative (конвертер снаружи вычитает старт клипа); мусор (vel-0-on после нормализации, pitch>127, ch>15, кадр за 2^40, сверх бюджета) — счётчики, не отравление тейка; re-arm сбрасывает. Квантизация v0 = identity по кадрам, beat-snap — UI-пасс над картой.
События — собственный POD `RecordedMidiEvent`, platform не инклюдится (контур audio не зависит от CoreMIDI).

## Открытые гейты и гэпы (не закрыто здесь)
Живой контроллер (никто не вызывает arm/feed/stop из run loop приложения); конвертер `MidiCapturedEvent→RecordedMidiEvent` в glue-слое (frame=framesFromHostTime−clip.start, kind/vel0 маппинг, oldest-first); `daw_append_midi_notes` в C-ABI и UI-arming/Record-кнопка для MIDI-дорожек; crash-журнал непрокоммиченного тейка (v0 держит батч в RAM — сознательно, схема не менялась); punch/loop-запись; live-мониторинг записываемых нот; авто-растягивание клипа под запись (по конвенции — reject, рост окна отдельная операция).

## Гейты кода
`midi_model` (5 новых секций: append/rejects/лимиты/recorder/fit-предикат + end-to-end take→append→draft→undo), `recording_crash_recovery` без правок зелёный; тройной прогон на слитом дереве 15/14/14.