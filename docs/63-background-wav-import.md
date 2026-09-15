# Background WAV import

Версия 1.36.0 переносит ограниченный PCM WAV import из control/main thread в один cancellable background job. Срез сохраняет формат и лимиты [версии 1.35](62-variable-rate-wav-import.md): RIFF PCM mono/stereo, PCM16/24/32 или float32, 44,1 / 48 / 88,2 / 96 / 192 kHz, до 60 секунд и 32 MiB. Он не добавляет streaming media engine, новые codecs или неограниченные файлы.

## Контракт job

`daw_begin_import_wav` и `daw_begin_import_take_wav` только валидируют intent и фиксируют source path, target (для take), session epoch и исходную revision. Они не читают PCM в UI thread и не меняют domain model. Worker выполняет чтение, RIFF validation, decode и Apple AudioConverter resampling вне realtime. Он публикует только immutable stereo Float32 48 kHz result и bounded status: reading, decoding, converting, ready, failed, canceled или applied.

`daw_poll_import` не меняет проект и потокобезопасно возвращает phase/progress, source metadata и output frame count. `daw_cancel_import` cooperative: отмена не публикует частичный Clip. `daw_release_import` запрашивает отмену running worker и немедленно освобождает handle; job не удерживает session. Ни worker, ни poll не вызываются из Audio HAL callback.

## Применение и revision

Когда job готов, `daw_apply_import(session, job, expected_revision)` выполняется только на owning control thread. Он добавляет ровно один track или take одним domain command и одной revision, затем делает job applied. Ошибка apply не создаёт пустой track/take и оставляет ready result доступным для повторной попытки.

UI автоматически применяет result лишь когда всё ещё используется тот же session и текущая revision равна зафиксированной при begin. Если во время чтения пользователь успел редактировать проект, status surface явно сообщает о готовом WAV, но сам ничего не добавляет. Кнопка «Добавить в текущий проект» становится отдельным явным действием и передаёт актуальную revision bridge. Это сохраняет предсказуемость работы и не отменяет изменения пользователя.

Один UI instance ведёт ровно одну import job. Повторные Toolbar Import, Browser Add или import take показывают, что текущий import уже выполняется. Пользователь может редактировать, сохранять, запускать playback и использовать mixer; старт import останавливает только Browser preview. New/Open/termination отправляют cancel и release до смены/уничтожения session.

## macOS UI

Нижний status surface показывает процент и фазу «чтение файла», «декодирование PCM» или «конвертация к 48 кГц» вместе с сообщением, что работа с проектом продолжается. Верхняя панель показывает отдельную кнопку «Отменить импорт» на время job. При stale-ready result рядом появляется «Добавить в текущий проект». У обеих кнопок есть понятные Accessibility label/help; status обновляет accessibility value.

После успешного track import UI выбирает созданную дорожку и её первый clip. После import take выбирается добавленный take. Failed/canceled jobs освобождаются и очищают import controls; ошибка остаётся кратким сообщением status surface без частичного проектного изменения.

## Проверяемые границы

Code acceptance должен покрывать: nonblocking begin, progress/poll, cancellation, malformed/oversize input, ready/apply, failed optimistic apply без мутации, retry с текущей revision, take placement, session epoch rejection и release running job. macOS acceptance должен проверить status/action states и сохранение доступности editing/playback во время import.

Этот документ не заявляет прослушивание converted audio, качество resampling на музыкальном материале, физическую latency, VoiceOver audit или общий media streaming. Они остаются отдельными gates.
