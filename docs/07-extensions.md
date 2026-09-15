# Расширения и command API

## Три разных вида расширений

**Audio plug-in** обрабатывает audio/MIDI через AU/VST3/CLAP. **Workflow module** читает ограниченный snapshot проекта и предлагает операции монтажа. **UI contribution** добавляет объявленный control в разрешённую область интерфейса. Один пакет может объединять workflow и UI, но права и жизненные циклы разделяются.

Это позволяет встроить «подготовить дубли» в контекст клипов, не делая модулю доступным весь mutable session или audio callback. REAPER SDK и Reason Rack Extensions — полезные примеры глубины интеграции; наш контракт определяется собственными требованиями.[^1]

## Эволюция SDK

S0/S1: только встроенные модули, которые уже используют тот же command/query boundary. S2: декларативные внешние workflow-рецепты, состоящие из разрешённых действий. Следующий этап: подписанный worker process с brokered access. WASM/Lua/JS runtime не вводится до потребности в нём.

SDK version 0 — экспериментальный. До 1.0 возможны несовместимые изменения, указанные в release notes. Native DSP ABI для сторонних модулей не обещается: для DSP использовать стандартный plugin format.

## Manifest

Контракт и пример находятся в [specs](../specs/README.md). Manifest задаёт `id`, version, `apiVersion`, display name, required capabilities и actions. `uiSlot` ограничен `clip.context` / `track.context` / `command.palette`. Action содержит типизированную операцию; arbitrary view injection, JS strings и shell commands отсутствуют.

ID пространств имён стабилен, имя локализуется. Настройки контролов: label, unit, range, step, default, accessibility label и automation policy. Правила UI принадлежат host; module не меняет глобальную тему и не переназначает горячие клавиши без пользователя.

## API semantics

Query возвращает snapshot с project ID и revision. Команды формируют единую Undo-группу. Preview возвращает diff и при необходимости временный audition render. Apply проверяет, что исходная revision и assets ещё совпадают. При конфликте требуется новый план.

Первые атомарные операции: `track.rename` и `track.setGain`; встроенная декларативная action `track.prepare` разворачивается host в этот typed batch. Следующими остаются `clip.setGain` и `clip.split`. Для чисел — finite значения и диапазоны; frames — целые.

Ошибки: `invalid_schema`, `unsupported_operation`, `not_found`, `stale_revision`, `permission_denied`, `busy`, `storage_failed`, `engine_failed`. Ошибки не превращаются в частичный успешный batch. Сначала validate весь batch, затем подготовка и один durable commit.

Transport actions отдельны от Undo-проектных команд. Record требует пользовательского контекста и действительного входа. Экспорт/доступ к внешним путям — отдельные scoped operations, не свободная запись файлов.

## Права и изоляция

| Capability | Что разрешает | Чего не разрешает |
|---|---|---|
| `project.read` | Снимок выбранных объектов | Произвольный доступ к filesystem |
| `project.edit` | Предложение разрешённых команд | Указатель на session model |
| `audio.read.selection` | Broker выдаёт read-only копию/handle диапазона | Запись в исходник |
| `audio.render.preview` | Создать временный производный результат | Подменить media без commit |

Сетевой доступ отсутствует в первом SDK. Native worker process сам по себе не является sandbox: для запуска внешнего executable нужны OS sandbox / entitlements, broker file handles, bounded messages и negative tests. Пока они не реализованы, разрешены только доверенные встроенные модули и декларативные recipes без внешнего кода.

Политики: лимит запроса 1 MiB, batch до 100 операций, результат анализа до 16 MiB metadata, timeout и cancel для каждой задачи. Это исходные engineering limits, пересматриваемые по измерениям. Полное аудио передаётся через scoped media handles, не JSON base64.

## Отказ модуля

При timeout/crash preview удаляется, подтверждённый проект не меняется. Module state хранится с ID и version. При отсутствии модуля сохраняется opaque state; уже rendered media продолжает звучать. Нельзя автоматически запускать код модуля только из-за открытия чужого проекта.

Первый демонстрационный модуль: «Подготовить выбранные дубли». Сначала только переименовать выбранные дорожки по заданному шаблону и предложить gain changes по измеренному peak/RMS. Time alignment вводится позже: он требует независимого слушательского сравнения, а не только команды на перенос.

[^1]: Cockos, [REAPER Extensions SDK](https://www.reaper.fm/sdk/plugin/plugin.php); Reason Studios, [Rack Extension developer guide](https://developer.reasonstudios.com/documentation/rack-extension-sdk/4.1.0/rack-extension-dev-guide).
