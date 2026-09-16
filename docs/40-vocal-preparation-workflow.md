# Vocal preparation workflow — 1.14.0

15 сентября 2026. Первый полезный модуль B-015 теперь использует тот же manifest, analysis, preview и atomic command boundary, который предназначен для последующих workflow-модулей.

## Пользовательский поток

Команда `Workflow…` загружает bundled manifest `org.mydaw.vocal-preparation` версии 0.2. Пользователь выбирает от одной до 32 дорожек с аудио. Первый выбранный трек становится `Lead`, остальные получают имена `Double 1…N`. Доступны настройки base name, target RMS, peak ceiling и дополнительный уровень doubles.

До изменения проекта модуль показывает для каждой дорожки исходное и итоговое имя, measured RMS/sample peak, предложенный фейдер, predicted RMS/peak и признак ограничения по peak ceiling. Только отдельное нажатие Apply передаёт тот же deterministic plan в atomic commit.

## Анализ

Анализ выполняется по текущему immutable State и не запускает Audio Unit или realtime Renderer. Он читает только активные Region/Take sources, учитывает source offset, clip fades и matching crossfade overlap. Перекрывающиеся клипы суммируются один раз в общей timeline frame. Результат содержит stereo peak, stereo RMS и количество проанализированных frames.

Общий RMS рассчитывается из энергии двух каналов. Предложенный абсолютный track fader равен меньшему из усиления до target RMS и усиления до peak ceiling. Для Double к RMS target применяется отрицательный offset. Значение ограничивается диапазоном фейдера -120…+24 dB. Это техническая gain staging подсказка, а не оценка художественной громкости или loudness standard.

## Atomic plan

Domain planner принимает упорядоченные уникальные track IDs и рассчитанные gain suggestions. Он создаёт только разрешённые `track.rename` и `track.setGain` operations. Preview требует точную base revision и не меняет проект. Commit повторно строит и валидирует plan на той же revision, после чего создаёт одну revision и одну Undo entry. Ошибка одного track или параметра отклоняет весь batch.

Уточнение к формулировке (проверено сценарием `e2e_workflow_vocal`, 1.72.0): «одна revision и одна Undo entry» верны, только когда batch действительно что-то меняет. `Session::commitWorkflow` сравнивает результат с текущим состоянием и **молча возвращает ноль ревизий**, если диффа нет — повторный commit того же плана возвращает успех, не создаёт ревизию и не добавляет шаг в историю. UI не должен показывать «готово, отменить» на такой ответ.

## Числовой контракт ABI

Границы существуют в `daw_preview_vocal_preparation`/`daw_commit_vocal_preparation` и `daw_preview_workflow`/`daw_commit_workflow` (`engine/bridge/daw.cpp`), в заголовке `daw.h` они не описаны; сценарий `tests/e2e/e2e_workflow_vocal.cpp` держит их как исполняемую документацию.

| Параметр | Диапазон | Что происходит вне диапазона |
|---|---|---|
| выбрано дорожек | 1–32 | отклоняется, текст «1–32 selected tracks» |
| `targetRms` | −60…−6 дБ, обязательно конечное | отклоняется |
| `peakCeiling` | −18…0 дБ, конечное | отклоняется |
| `doubleOffset` | −24…0 дБ, конечное | отклоняется |
| операций в workflow-batch | 1–100 | отклоняется |
| итоговый трек-фейдер | зажимается в −120…+24 дБ | значение обрезается, не отказ |

Формула предложения: `proposed = min(targetRms + roleOffset − measuredRms, peakCeiling − measuredPeak)`, где `roleOffset` равен 0 для первой (Lead) дорожки и `doubleOffset` для остальных; дорожка без проанализированных фреймов получает усиление 0. Признак `peak_limited` выставляется сравнением выбранного усиления с усилением до RMS-цели с эпсилон 0.0001 дБ — то есть это «потолок peak ограничил сильнее, чем цель RMS», а не отдельная политика.

Счётчики двух preview-команд означают разное, и путать их нельзя: `daw_preview_workflow` возвращает `change_count` = число дорожек, у которых **фактически** что-то изменилось (две операции на одной дорожке дают один change, переименование в то же имя — ноль), а `daw_preview_vocal_preparation` возвращает `item_count` = всегда число выбранных дорожек. `after_revision` равна base+1 только при ненулевом диффе, иначе base.

Строгий JSON Schema находится в `specs/vocal-preparation.schema.json`. Контракт использует честные названия `rmsDb` и `peakDb`: текущий анализ не заявляет LUFS/integrated loudness или oversampled true peak.

## Открытые границы

- модуль подготавливает существующие выбранные дорожки и не создаёт копии аудио;
- alignment, timing correction и обработка Audio Unit не применяются;
- значения ещё не подтверждены прослушиванием на пользовательском материале;
- отдельный QA, physical и listening pass по указанию пользователя не запускались.
