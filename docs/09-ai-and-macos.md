# ИИ и интеграция с macOS

## Архитектура помощника

Text/voice → intent parser → typed plan → validation → preview → commit через command API. Помощник не получает shell, SQL connection или возможность напрямую менять project files. Любая поддержанная операция остаётся доступной вручную.

Для «переименуй дорожку» можно применять точную команду по явному запросу без отдельного диалога. Для «сделай вокал ближе» нужен прослушиваемый вариант и показ изменяемых параметров. Не заставлять музыканта подтверждать каждый безопасный шаг; объединять изменения в одну Undo-группу.

В S0/S1 нет обязательного LLM. Начать с детерминированных действий и измерителей. Наличие модели не доказывает музыкальный вкус и качество мастеринга.

## Фоновые задачи

| Задача | Начальный метод | Проверка |
|---|---|---|
| Поиск тишины/пиков | DSP thresholds с параметрами | Не вырезать дыхание и тихие окончания автоматически |
| Измерение громкости | Проверенный measurement algorithm | Reference fixtures, channel/sample-rate handling |
| Alignment дублей | DSP correlation / time-stretch prototype | Consonants, sibilants, фазовые артефакты, ручной A/B |
| Speech-to-text | Отдельная optional model | Русский вокал, пение и речь оцениваются отдельно |
| Stem separation | Отдельная тяжёлая модель | Bleed/artifacts, лицензия весов, RAM и время |
| Intent parsing | Rule-based сначала, model adapter позже | Exact command evals и reject unknown actions |

Baseline кандидата inference — Core ML; Apple документирует использование CPU/GPU/Neural Engine.[^1] Новая Core AI экосистема также включена в план исследования.[^2] Backend выбирается по конкретной модели, minimum OS, RAM и конкуренции со звуком. Не гарантировать ANE execution или одинаковую скорость моделей по одному названию framework.

## План ресурсов

При записи тяжёлые analysis jobs ставятся на паузу; лёгкие измерители имеют фиксированный CPU budget. При play analysis работает в низком приоритете. По memory pressure очередь останавливается, model unload возможен вне RT. Начальный лимит одного ML job — 2 GiB resident budget на машине 16 GiB, но это экспериментальная граница, а не обещание любой модели.

Background job связан с immutable audio hash и revision. Результат устаревает после редактирования исходника; пользователь получает возможность пересчитать, а не тихое применение к другому клипу. Cancel должен освобождать временные ресурсы без удаления оригиналов.

## Данные и модели

По умолчанию audio и проекты локальны. Внешний provider — необязательный adapter, доступный только после явного выбора отправляемых данных. Не включать облачную обработку автоматически ради fallback. Ключи при будущей интеграции — в Keychain, не в проекте.

Каждая модель имеет manifest: upstream URL, версия/SHA, license code, license weights, разрешённое применение, размер загрузки/RAM, sample-rate/channel requirements, результаты listening eval. Лицензия inference runtime не означает право распространять веса. Собственный consented dataset хранится отдельно от публичного repo.

## Системные функции Mac

AppKit отвечает за gestures, drag-and-drop, menus, document windows и accessibility. SwiftUI применяется для небольших управляемых форм; high-frequency timeline не строится тысячами отдельных view без benchmark.

App Intents публикует ограниченные действия: открыть проект, показать transport, экспортировать через пользовательский выбор destination. Apple HIG различает App Shortcuts и действия App Intents на Mac: поддержку готовых App Shortcuts из iOS нельзя переносить напрямую.[^3] Siri integration — отдельный spike на поддержанной macOS, RU/EN locale и включённых системных возможностях. Работа DAW не зависит от Siri.

Голосовая запись команд только push-to-talk и с видимым состоянием; постоянно слушающий ассистент не нужен. Команда «начни запись» требует готового audio input, а не автономного включения микрофона при открытии проекта.

## Eval для S2

Набор не менее 100 заранее размеченных RU/EN запросов: точные операции, неоднозначные просьбы, отсутствующие объекты, устаревшие revision, запрещённые операции, отмена. Метрики — корректность плана, доля опасных/неподдержанных действий (цель 0 на наборе), успешная отмена, время выполнения. Ещё 10 реальных музыкальных сессий с A/B для оценки полезности; один synthetic benchmark не доказывает качество слухового результата.

[^1]: Apple, [Core ML](https://developer.apple.com/documentation/coreml).
[^2]: Apple, [Core AI](https://developer.apple.com/core-ai/), [Meet Core AI, WWDC26](https://developer.apple.com/videos/play/wwdc2026/324/).
[^3]: Apple, [App Shortcuts HIG](https://developer.apple.com/design/human-interface-guidelines/app-shortcuts), [App Intents](https://developer.apple.com/documentation/appintents).
