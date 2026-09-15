# Приёмка альфы (S1)

Базовый уровень приёмки ALPHA (рабочая альфа, этап S1 из [docs/02-mvp.md](02-mvp.md)). Документ фиксирует честное текущее состояние прототипа 1.38.0 и перечень того, что можно закрыть только на реальном железе, прослушивании и vendor-плагинах.

## Статус альфы

«Alpha/S1» здесь означает законченную запись вокала с дублями, ручным comp, базовым микшером, AU-эффектами и восстановлением (см. границы S1 в [docs/02-mvp.md](02-mvp.md)). Честное текущее состояние: C++ ядро собирается через CMake, все 14 headless CTests зелёные в debug preset (`storage_jobs_crash`, `audio_render_storage`, `background_wav_import`, `aiff_import`, `offline_wav_export`, `dawproject_export`, `recording_crash_recovery`, `vst3_runtime_isolation`, `approved_au_host`, `session_storage_bridge`, `pure_c_bridge`, `midi_model`, `midi_bridge`, `midi_input`); в sanitizers preset (CI-гейт, ASan/UBSan) — 13/13 (`vst3_runtime_isolation` debug-only), thread-sanitizer — 13/13; AU/VST3 browser audition (`macos_audio_browser_preview`) запускается вручную через `scripts/test-audio-preview.sh` и не входит в CTest (требует реального устройства); macOS AppKit-приложение собирается через `scripts/build-macos.sh` и запускается; `hardware_smoke` проходит на этой машине (есть реальное output-устройство). При этом физические гейты остаются: полный full-duplex путь, прослушивание, приёмка на слух, vendor-матрица AU/VST3, hardware presentation latency и полный VoiceOver-аудит не закрыты в этой среде. Зелёный CI и проверка Markdown/SQL не равны сквозной приёмке S1 — успех требует приложения и аудио на физическом устройстве (см. пример сквозной приёмки в [docs/02-mvp.md](02-mvp.md)).

## Матрица приёмки S1

Статусы используются точно: **Проверено на этой машине** — есть конкретное headless- или device-доказательство, полученное на этой машине; **Готово в коде (нужен физический гейт)** — код завершён и покрыт срезами/CTest, остаётся физическая проверка (железо/прослушивание/vendor); **Физический гейт (не закрыт здесь)** — проверка возможна только на реальном устройстве/топологии, которая в этой среде недоступна (или есть незакрытый code gap, отмеченный в зазоре).

> Примечание: требования **EXT-01** и **AI-01** относятся к этапу **S2 (пост-альфа)** и не входят в базовый уровень приёмки S1; включены в таблицу для полноты карты требований из [docs/02-mvp.md](02-mvp.md).

| ID | Требование (кратко) | Статус | Доказательство / где проверено | Оставшийся зазор |
|---|---|---|---|---|
| DEV-01 | Выбор одного full-duplex Core Audio устройства и каналов | Физический гейт (не закрыт здесь) | [docs/29-output-lifecycle.md](29-output-lifecycle.md) — `hardware_smoke` 3×350 ms, 33 callbacks, отдельные тексты device change/stall/buffer-error; macOS app собирается и запускается, `hardware_smoke` проходит на реальном output-устройстве | Full-duplex требует единое устройство или aggregate: `daw_duplex_hardware_smoke` не запускается на раздельных входе/выходе MacBook; property listener ещё не установлен (poll 100 мс); восстановление доступного UID при reopen не проверено на реальном устройстве |
| DEV-02 | Разрешение микрофона; отказ не ведёт к падению | Готово в коде (нужен физический гейт) | [docs/23-dry-recording.md](23-dry-recording.md) — проверка разрешения перед Record, объяснение в UI, запись `Info.plist`; CTest не открывает микрофон | Реальный системный TCC-запрос и физический микрофон не прогнаны автоматически; ad-hoc подпись вызывает повторный запрос между сборками (нужна стабильная Developer ID identity) |
| TRN-01 | Play/Stop/Seek/Record; UI соответствует engine acknowledgement | Готово в коде (нужен физический гейт) | [docs/17-audio-slice.md](17-audio-slice.md) — AppKit: импорт, Play с движущейся позицией, ручной Stop, save/reopen; [docs/18-waveform-slice.md](18-waveform-slice.md) — seek; `hardware_smoke` подтверждает output-callbacks | Полный путь Record и синхронизация UI↔engine acknowledgement проверены в коде, но требуют реального устройства/микрофона; full-duplex transport не запускался (`daw_duplex_hardware_smoke`) |
| IMP-01 | Импорт PCM WAV/AIFF mono/stereo; медиа в проект; ресемплинг вне RT | Готово в коде (нужен физический гейт) | CTest `background_wav_import` ([docs/63-background-wav-import.md](63-background-wav-import.md)) и `audio_render_storage`; [docs/62-variable-rate-wav-import.md](62-variable-rate-wav-import.md) — 44,1/48/88,2/96/192 кГц; AIFF/AIFC закрыты CTest `aiff_import` ([docs/67-aiff-import.md](67-aiff-import.md)) — 16/24/32-bit int, AIFC fl32, mono/stereo, 44,1/48/96 kHz, ресемплинг в 48 kHz, отмена; полный suite 14/14 Debug, 13/13 ASan; `hardware_smoke` (тихой playback) | UI-путь AIFF в приложении смонтирован (роутинг по расширению .aif/.aiff, панели WAV/AIFF; сборка приложения зелёная); остаются прослушивание converted audio и качество resampling (listening gate) |
| REC-01 | Запись одного mono входа; корректный клип и воспроизводимый файл | Готово в коде (нужен физический гейт) | CTest `recording_crash_recovery` ([docs/24-crash-safe-recording.md](24-crash-safe-recording.md): mono→stereo, NaN, clamp, bounded overflow, continuous prefix, header, region position, C bridge, delete after commit, reject corrupt); [docs/23-dry-recording.md](23-dry-recording.md) | Реальный микрофонный вход и прослушивание записанного дубля не проверены; USB-disconnect и смена sample rate во время записи открыты |
| REC-02 | До двух одновременных входных каналов; 30 мин без пропусков | Физический гейт (не закрыт здесь) | Кодовая база собирается; multi-channel capture не покрыт прогоном | Требует единое full-duplex устройство или aggregate; `daw_duplex_hardware_smoke` не запускается на раздельных входе/выходе; 30-минутный прогон без xruns и two-channel capture не закрыты здесь |
| REC-03 | Loop recording, take lanes, pre-roll/metronome | Физический гейт (не закрыт здесь) | [docs/31-take-lanes-comp.md](31-take-lanes-comp.md) — comp-логика, v8 roundtrip; [docs/33-full-duplex-loop-recording.md](33-full-duplex-loop-recording.md) — CTest разбиения проходов `4+4+2`, batch Undo, format v8 | **Code gap:** pre-roll/metronome и latency compensation для loop ещё не реализованы ([docs/23-dry-recording.md](23-dry-recording.md), [docs/33-full-duplex-loop-recording.md](33-full-duplex-loop-recording.md)); плюс sample-exact loop recording требует единое устройство/aggregate (`daw_duplex_hardware_smoke` не запускается) |
| EDT-01 | Move/trim/split/clip gain/fades; недеструктивно | Готово в коде (нужен физический гейт) | [docs/19-clip-editing.md](19-clip-editing.md), [docs/21-split-clips.md](21-split-clips.md), [docs/22-clip-operations-fades.md](22-clip-operations-fades.md) — format v3/v4/v5, Undo, проверка недеструктивности | Автоматизированный headless CTest для editing не входит в прошедшие 10 CTests; ручная UI-проверка на реальном устройстве и listening остаются |
| EDT-02 | Comp диапазонов из take lanes; границы и fades редактируемы; оригиналы сохранены | Готово в коде (нужен физический гейт) | [docs/31-take-lanes-comp.md](31-take-lanes-comp.md) — comp left/selected/right, source в Renderer, take сохранён после fade/split, Undo/Redo, v8 roundtrip | Прослушивание comp, fades на comp-границах и реальный recording в armed lane не проверены (physical gate, B-010) |
| MIX-01 | Gain/pan/mute/solo/master; предсказуемый mono/stereo; без маскировки clipping | Готово в коде (нужен физический гейт) | [docs/30-mixer.md](30-mixer.md) — gainDb/pan/muted/solo/master, safety clamp `[-1,1]`, без нормализации, v7 roundtrip, точное совпадение с offline export | Listening acceptance и layout на разных размерах окна открыты; mono pan пока balance, не equal-power (нет channel metadata в asset) |
| MIX-02 | Audio bus, pre/post-fader send; цикл отклоняется до изменения графа | Готово в коде (нужен физический гейт) | [docs/35-routing-buses-sends.md](35-routing-buses-sends.md) — DFS-проверка цикла до commit, pre/post sends, format v9; [docs/45-track-bus-inserts-pdc.md](45-track-bus-inserts-pdc.md) — полный graph PDC; CTests покрывают cycle/self-route rejection | Реальный routing graph на устройстве не проверен; bus solo и sidechain частично не реализованы ([docs/35-routing-buses-sends.md](35-routing-buses-sends.md)) |
| FX-01 | Встроенные gain/pan; тембр/эффекты — через AU/VST3; без RT-аллокаций | Готово в коде (нужен физический гейт) | [docs/30-mixer.md](30-mixer.md) — built-in gain/pan, smoothing, без RT-аллокаций в собственном коде | EQ закрыт решением владельца (2026-09-15): собственные эффекты вне скоупа, тембр — через AU/VST3-хостинг; listening/DSP acceptance остаётся физическим гейтом |
| FX-02 | AU effects arm64; scan/editor/state/automation/latency по матрице | Готово в коде (нужен физический гейт) | CTest `approved_au_host` ([docs/36-apple-au-master-inserts.md](36-apple-au-master-inserts.md): AUDynamicsProcessor/AUPeakLimiter/AUMatrixReverb state/latency/finite output), `vst3_runtime_isolation` ([docs/58-vst3-managed-runtime-isolation.md](58-vst3-managed-runtime-isolation.md)); [docs/37-au-scanner-parameters.md](37-au-scanner-parameters.md), [docs/45-track-bus-inserts-pdc.md](45-track-bus-inserts-pdc.md), [docs/53-async-auv3-hosting.md](53-async-auv3-hosting.md) | Vendor AU/VST3 compatibility matrix (B-012) не прогнана: реальные сторонние плагины, editor proxy, listening acceptance и hardware presentation latency остаются (physical/vendor gate) |
| MIDI-01 | Нотная модель: домен, хранение v16, C-ABI, табличный редактор | Готово в коде (нужен физический гейт) | [docs/68-midi-foundation.md](68-midi-foundation.md) — валидации/лимиты, undo/redo, round-trip v16 и fallback ≤15; CTests `midi_model`, `midi_bridge`, `pure_c_bridge` (MIDI-дым+негативы) | Живые клики редактора, undo после MIDI-мутаций на глаз и paging >8192 нот в UI не проверены; audible-плеер MIDI-only дорожек — code gap до арки instrument-source |
| MIDI-02 | Доставка нот в VST3-инструменты хостингом | Готово в коде (нужен физический гейт) | `vst3_runtime_isolation` MIDI-эхо (frame-accurate доставка через границу процессов, протокол v2, reject-пути), флаг Instrument в сканере (кэш v2) | Доставка в живой .vst3-синт и скан реального инструмента не прогнаны (нет плагина в этой среде); AU MusicDevice — отдельная арка |
| MIDI-03 | MIDI-вход с устройства (ring, running status, timestamps) | Готово в коде (нужен физический гейт) | CTest `midi_input` — парсер пакетов, resync, drop-oldest кольцо, арифметика кадров; [docs/68-midi-foundation.md](68-midi-foundation.md) | Живой контроллер, доставка через run loop приложения и запись в клипы не проверены; MIDI 2.0 (MIDIEventList) — будущая арка |
| AUT-01 | Read/write volume и pan automation; sample offset; одна Undo-группа на drag | Готово в коде (нужен физический гейт) | [docs/38-volume-automation-pdc.md](38-volume-automation-pdc.md) — sample-accurate lanes, format v11, Undo; [docs/44-automation-write-pdc-plan.md](44-automation-write-pdc-plan.md) — Read/Touch/Latch, atomic gesture Undo; [docs/46-plugin-automation-tail-export.md](46-plugin-automation-tail-export.md) — sample-offset lanes, format v14; [docs/47-plugin-touch-audible-playhead.md](47-plugin-touch-audible-playhead.md) | Listening acceptance и hardware presentation timestamp (B-013 gate) не проверены; автоматизированный listening/QA не запускались |
| PRJ-01 | Save/reopen/Save As; переносимые пути; копия открывается без оригинала | Проверено на этой машине | CTest `session_storage_bridge` и `storage_jobs_crash`; [docs/17-audio-slice.md](17-audio-slice.md) — AppKit: save → перемещение исходного WAV → reopen восстановил 12 с аудио; [docs/64-delete-track-undo.md](64-delete-track-undo.md) / [docs/65-track-reorder.md](65-track-reorder.md) — SQLite round-trip | Только surviving project state и SQLite round-trip проверены; глубокая matrix power-loss/коррупции вне scope |
| PRJ-02 | Crash recovery; последняя подтверждённая транзакция; recoverable recording отдельно | Проверено на этой машине | CTest `recording_crash_recovery` ([docs/24-crash-safe-recording.md](24-crash-safe-recording.md): дочерний `_exit` после checkpoint → родитель читает confirmed prefix) и `storage_jobs_crash`; [docs/24-crash-safe-recording.md](24-crash-safe-recording.md) — recovery `.mydawtake` | Реальный device disconnect/смена sample rate/power-loss во время записи не проверены (physical gate); filesystem corruption matrix отдельная |
| EXP-01 | WAV stereo 24-bit PCM / 32-bit float; длина/частота/каналы/уровень; отмена без «готового» файла | Проверено на этой машине | CTest `offline_wav_export` ([docs/25-offline-export.md](25-offline-export.md): точное совпадение с renderer, заголовки PCM24/float32, PCM24 с допуском, неизменность файла при отмене, отказ пустого проекта, 6/6); целевой CTest `dawproject_export` — DAWproject export завершается успехом и zip содержит `project.xml`, `metadata.xml`, `loss-report.json`, `audio/` (атомарная публикация и кооперативная отмена — реализация `writeZipPackage`) | Listening PCM24/float32 acceptance остаётся (physical); нет stems/LUFS/true-peak/sample-rate conversion/metadata |
| EXP-02 | Offline bounce с AU и хвостами; unsupported offline plugin → realtime bounce; сравнение по допуску | Готово в коде (нужен физический гейт) | [docs/25-offline-export.md](25-offline-export.md) — renderer match; [docs/46-plugin-automation-tail-export.md](46-plugin-automation-tail-export.md) — finite VST3 tail, AU tail=0; [docs/60-infinite-tail-export-policy.md](60-infinite-tail-export-policy.md) — automatic/finite-only/limit 2/5/15/30 с; CTest `offline_wav_export` покрывает VST3 sentinel/sequential/parallel tail (прогон sanitizers: 10/10) | Listening-сравнение playback vs export; реальный AU tail для reverb/delay не объявлен (AU tail=0); realtime-bounce fallback для unsupported offline plugin нужно подтвердить на реальных плагинах (vendor gate); tail-dialog visual/VoiceOver smoke не завершён ([docs/60-infinite-tail-export-policy.md](60-infinite-tail-export-policy.md)) |
| EXT-01 | Один workflow-модуль через публичный контракт (S2) | Готово в коде (нужен физический гейт) | [docs/11-roadmap.md](11-roadmap.md) — B-015 «Code scope готов» (Lead/Doubles, peak/RMS, preview, atomic apply) | **S2 (пост-альфа):** вне базового уровня приёмки S1; требует listening evidence и реального модуля; не входит в гейты альфы |
| AI-01 | Preview плана поддержанных команд (S2) | Готово в коде (нужен физический гейт) | [docs/11-roadmap.md](11-roadmap.md) — B-014 «Prototype готов» (bundled manifest, typed preview, atomic one-revision commit) | **S2 (пост-альфа):** вне базового уровня приёмки S1; не входит в гейты альфы |

## Физические гейты, остающиеся для S1

То, что можно закрыть только на реальном железе, прослушивании или vendor-плагинах (не дефекты кода, а ограничения среды/топологии/лицензий):

- **Sample-exact loop recording (REC-03)** требует единое full-duplex устройство или aggregate в Audio MIDI Setup; `daw_duplex_hardware_smoke` не запускается на типичном MacBook с раздельными входом/выходом (ограничение топологии железа).
- **До двух одновременных входов (REC-02)** и 30-минутный прогон без xruns — то же ограничение единого устройства; многоканальный capture не прогнан.
- **Full-duplex выбор устройства (DEV-01)** и property-listener обнаружение device-lost — нужен реальный device/disconnect; reopen UID restore не проверен.
- **Прослушивание / listening acceptance (IMP-01, REC-01, MIX-01, EDT-02, AUT-01, EXP-01/02, FX-02)** — ни один автоматический тест не заменяет приёмку на слух; quality resampling и точное совпадение playback↔export на музыкальном материале не подтверждены.
- **Матрица совместимости AU/VST3 (FX-02, B-012)** — реальные сторонние плагины, editor proxy, sidechain и compatibility matrix не прогнаны (self-hosted fake helper только для VST3 isolation, [docs/58-vst3-managed-runtime-isolation.md](58-vst3-managed-runtime-isolation.md)).
- **Hardware presentation latency (B-013)** — measured round-trip latency и causal monitoring ещё не измерены на устройстве.
- **Полный VoiceOver-аудит** — Accessibility backend переставал возвращать дерево окна после подтверждения системных панелей ([docs/60-infinite-tail-export-policy.md](60-infinite-tail-export-policy.md), [docs/62-variable-rate-wav-import.md](62-variable-rate-wav-import.md)), поэтому tail-dialog, browser-preview UI-состояния и общий VoiceOver pass не завершены.
- **Crash / disconnect тесты на реальном устройстве (B-005, PRJ-02)** — device disconnect во время playback/recording, смена sample rate и power-loss не прогнаны на железе; filesystem corruption matrix отдельная.
- **TCC / микрофон (DEV-02, TRN-01, REC-01)** — реальный системный запрос разрешения и стабильность между сборками (ad-hoc подпись) не проверены.

## Как собрать и проверить

Из корня репозитория:

```sh
# Ядро (CMake presets)
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# macOS AppKit-приложение
./scripts/build-macos.sh

# Утилиты разработки и документации
python3 scripts/doctor.py
python3 scripts/check_docs.py
```

Headless CTest: debug — 11 тестов (`storage_jobs_crash`, `audio_render_storage`, `background_wav_import`, `aiff_import`, `offline_wav_export`, `dawproject_export`, `recording_crash_recovery`, `vst3_runtime_isolation`, `approved_au_host`, `session_storage_bridge`, `pure_c_bridge`); sanitizers — 10 (`vst3_runtime_isolation` собирается только с VST3 runtime helper, то есть debug-only).

Device-тесты запускаются **вручную** на машине с аудиоустройством (не входят в общий CI):

```sh
# Тихой output smoke (реальное output-устройство)
./build/debug/daw_hardware_smoke

# Full-duplex loop smoke — требует единое устройство или aggregate
./build/debug/daw_duplex_hardware_smoke

# Browser audio preview (отдельный ручной listening/AX smoke)
./scripts/test-audio-preview.sh
```

Полная валидация JSON Schema в `check_docs.py` пропускается без пакета `jsonschema`; ссылки Markdown и SQL-ограничения PASS. Для полной проверки схем и пересборки HTML — см. раздел «Проверка подготовленных материалов» в [README.md](../README.md).

## Следующие шаги до беты

Связано с backlog M2/M3 из [docs/11-roadmap.md](11-roadmap.md) и этапом M4:

- **B-010** — физическое доказательство take lanes / loop recording / playback loop / AUHAL loop recording (latency/recovery v2, pre-roll/metronome, sample-exact на едином устройстве).
- **B-012** — vendor matrix AU/VST3: реальные сторонние плагины, editor proxy, sidechain, compatibility matrix (закрывает физический гейт FX-02).
- **B-013** — listening acceptance и hardware presentation timestamp (закрывает гейты AUT-01/MIX/FX и EXP-02).
- **B-005** — полный physical disconnect / смена sample rate / power-loss на реальном устройстве (закрывает DEV-01/REC-01/PRJ-02 device-гейты).
- **B-014 / B-015 / B-016** — workflow SDK v0, preview/Undo и один полезный модуль (EXT-01), AI preview плана (AI-01), DAWproject import matrix.
- **MIDI-фундамент** — модель, доставка в инструменты и пианино-ролл: следующая крупная арка за пределами S1.
- **Темпо-карта** — выход за фиксированный темп 120 BPM: модель, transport и доставка в аудио/плагины.
- **М4** — VST3 production hardening, DAWproject migration, install evidence и beta acceptance (compatibility, migration, crash/disconnect на реальном устройстве).