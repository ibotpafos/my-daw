# Машиночитаемые контракты v0

Это минимальные draft-контракты для первого implementation slice, не готовый SDK. JSON Schema draft 2020-12. Конкретные операции и semantics описаны в [расширениях](../docs/07-extensions.md), SQL subset — в [формате проекта](../docs/06-project-format.md).

| Файл | Назначение | Пример |
|---|---|---|
| [command.schema.json](command.schema.json) | Атомарный batch rename/gain | [command.json](examples/command.json) |
| [module.schema.json](module.schema.json) | Декларативный workflow manifest | [module.json](examples/module.json) |
| [project-manifest.schema.json](project-manifest.schema.json) | Заголовок package | [manifest.json](examples/manifest.json) |
| [project-v0.sql](project-v0.sql) | Durable S0 subset | Проверяется in-memory в check_docs.py |

Operation `track.setGain` принимает dB в диапазоне −120…24; это finite значение, отдельный mute не кодируется через −Infinity. UI должен предлагать привычный рабочий диапазон и показывать точное значение. JSON не допускает NaN/Infinity. Идентификаторы в v0 — lowercase ASCII tokens с `-`, длиной до 80; будущий UUID-only формат потребует миграции.

JSON Schema проверяет форму, но не существование track/project, matching revision, доступ к capabilities, уникальность action IDs, пути внутри package или атомарность выполнения. Эти проверки обязательны в domain/broker. `project.read`/`project.edit` в manifest — запрос прав, не доказательство их выдачи.

SQL foreign keys включаются **на каждом соединении**. Межтабличные условия clip source range и project sample rate проверяет controller перед commit. Единственный project row нужен для package. Не ссылаться на ещё не завершённое медиа. Клипы v0 не time-stretched: source_offset + duration не превосходит asset frames.

Схемы и примеры не выполняют команды, не запускают модуль, не содержат аудио. Проверка контракта не является тестом приложение→engine→device.
