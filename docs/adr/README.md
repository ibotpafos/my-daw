# Реестр архитектурных решений

Статус `selected-for-spike` означает выбранное рабочее направление, которое ещё не доказано реализацией. `proposed` — решение перед подключением/распространением. Ни один ADR не означает готовую функцию.

| ADR | Решение | Статус | Причина | Пересмотреть, если |
|---|---|---|---|---|
| 001 | macOS 14+, arm64 сначала | selected-for-spike | Сосредоточиться на Mac UX и измерениях | Windows становится равным продуктовым приоритетом |
| 002 | Собственное ограниченное C++20 ядро | superseded-for-expansion | Дало reference slices и RT boundaries | Новые graph/hosting services сравниваются через ADR-012 |
| 003 | Swift/AppKit shell, SwiftUI panels | selected-for-spike | Нативные windows/input/accessibility | Canvas/interop не укладываются в бюджет |
| 004 | C ABI между app и core | selected-for-spike | Узкий ownership и ABI contract | Измеримый выигрыш direct interop при сравнимом контроле |
| 005 | Один control owner, immutable render plans | selected-for-spike | Исключить locks из callback | Вводятся multiple render readers; нужен новый retirement protocol |
| 006 | SQLite package + immutable media | selected-for-spike | Транзакции, recoverability, переносимость | Recovery/Save As proof не обеспечивает инварианты |
| 007 | AU перед VST3, отдельный scanner | selected-for-spike | Mac-first compatibility scope | Пользовательский набор требует VST3 с первого дня |
| 008 | Workflow commands раньше arbitrary code SDK | selected-for-spike | Единый Undo и ограниченная поверхность API | Реальный модуль нельзя выразить typed actions |
| 009 | Optional background AI | selected-for-spike | Запись работает без сети/моделей | Доказан конкретный realtime use case и бюджет |
| 010 | MPL app, Apache standalone SDK | proposed | Открытые изменения core и доступный SDK | Выбрана GPL/AGPL dependency или другая стратегия экосистемы |
| 011 | DAWproject для обмена, не native storage | selected-for-spike | Не путать portability и active session I/O | Новые требования к обмену не выражаются форматом |
| [012](012-tracktion-adapter.md) | Tracktion за собственной domain/C ABI границей | selected-for-integration-spike | SP-06 доказал готовые edit/render services малым adapter-кодом | Parity, RT budget или лицензирование не проходят gate |

При изменении ADR добавлять дату, evidence, альтернативу и migration cost в отдельный файл. Не менять стек по одному новому framework announcement. Все принятые технические ограничения синхронизировать с MVP, schemas и roadmap.
