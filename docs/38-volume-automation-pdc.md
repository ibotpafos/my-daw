# Volume automation and PDC — 1.12.0

15 сентября 2026. Первый B-013 slice добавляет сохраняемую автоматизацию громкости дорожки и компенсацию суммарной задержки master insert chain при offline export.

## Automation model

`Track.volumeAutomation` содержит до 2048 строго упорядоченных точек `frame/gainDb` на десятиминутной timeline. Диапазон совпадает с фейдером: -120…+24 dB. Upsert заменяет точку в том же frame, remove удаляет её; обе команды используют expected revision, Undo/Redo и обычную атомарную project mutation.

Format v11 добавляет `track_volume_automation(track_id, position, frame, gain_db)`. Reader v1–v10 создаёт пустые lanes. Пустая lane сохраняет статический track fader. При наличии точек Renderer использует значение первой точки до неё, линейную интерполяцию в dB между точками и значение последней после неё.

UI показывает `AUTO n` на strip, позволяет записать текущее значение фейдера в позиции курсора и удалить выбранную точку. Cyan curve и handles рисуются поверх waveform. Режимы Touch/Latch продолжены в [1.17.0](44-automation-write-pdc-plan.md); bezier shapes ещё не реализованы.

## Signal flow

Volume automation заменяет статическое значение фейдера дорожки. Mute, solo и stereo balance остаются отдельными control values. Post-fader sends получают автоматизированный уровень; pre-fader sends берут сигнал до фейдера и automation. Такая граница сохраняет существующую send semantics B-011.

## Master latency compensation

Каждый prepared AU сообщает заявленную latency, Renderer суммирует активную master chain и публикует её transport UI. Offline export при ненулевой latency начинает обработку от frame 0, отбрасывает начальную задержку, затем подаёт тишину для flush. Выбранный export range сохраняет исходные start/end и точное число output frames.

Этот шаг компенсировал последовательную master chain. [1.17.0](44-automation-write-pdc-plan.md) добавил runtime planner, [1.18.0](45-track-bus-inserts-pdc.md) подключил реальные track/bus chains, [1.19.0](46-plugin-automation-tail-export.md) добавил parameter lanes и finite VST3 tail, а [1.20.0](47-plugin-touch-audible-playhead.md) — запись plug-in Touch/Latch и audible playhead после graph latency. Infinite-tail policy и hardware presentation timestamp остаются продолжением.

## Реализационные границы

- automation sample-accurate внутри render loop, форма сегмента linear-in-dB;
- parameter automation AU/VST3 работает через normalized sample-offset lanes; plug-in controls поддерживают один Touch/Latch gesture и live override;
- live output показывает latency, но causal monitoring остаётся задержанным самим plugin chain;
- отдельный QA и listening pass по указанию пользователя не запускались.
