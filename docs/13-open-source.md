# Открытая разработка и распространение

## Предложение лицензирования

Рекомендация — MPL-2.0 для собственного приложения/ядра и Apache-2.0 для отдельного SDK и примеров. MPL применяет copyleft на уровне покрытых файлов; это позволяет требовать публикации распространяемых изменений этих файлов и оставлять пространство для независимых дополнений.[^1] Это продуктово-техническое предложение, а не уже применённая лицензия: окончательный `LICENSE` пока не добавлен.

До публичного распространения определить правообладателя, применить выбранные notices и проверить фактические зависимости. Локальный Tracktion spike разрешает техническое сравнение, но не входит в app target. Его promotion требует одного явного пути: совместимая strong-copyleft лицензия всего распространяемого combined work либо коммерческие лицензии Tracktion и JUCE. Юридическая совместимость конкретной схемы проверяется перед публикацией, а не выводится из слова open source.

## Dependency policy

| Компонент | Проверенное положение | Последствие для решения |
|---|---|---|
| JUCE | Pinned spike revision сообщает JUCE 8.0.13; AGPLv3 / commercial | Не включать в MPL-приложение под предположением «оно бесплатное» |
| Tracktion Engine | Pinned develop tree VERSION 3.5.0, runtime string 3.1.0; GPL / commercial отдельно от JUCE | Не полагаться на runtime string как dependency identity; нужен совместимый план для обеих зависимостей |
| VST3 SDK | Текущий SDK MIT | Фиксировать выбранный commit; trademark отдельно |
| CLAP | MIT | Возможен отдельный host adapter |
| ARA SDK | Apache-2.0 | Поздняя integration task, не основа SDK workflow |
| Signalsmith Stretch | MIT | Кандидат, не уже подключённый DSP |
| SQLite | Public domain statement upstream | Сохранить provenance используемой поставки |
| Модели | Зависит от кода и весов отдельно | Проверить оба набора условий перед поставкой |

Источники: официальные license/repository pages перечислены в [реестре](sources.md). Таблица не означает юридического разрешения на любую комбинацию; итог зависит от конкретных revisions и способа распространения.

## Управление

Сначала один maintainer принимает архитектурные решения через ADR, с review для real-time, data-loss и public API изменений. Публичный API versioned. Предлагать DCO sign-off для внешних contributions; не вводить CLA без понятной потребности. Кодекс поведения, security contact и issue templates добавить перед открытием публичных contribution channels.

Не обещать community поддержку всех модулей. Catalog может иметь уровни first-party / verified / community, критерии верификации открыты. Пользователь может локально отключить модуль и открыть проект без него. Paid modules и закрытые дополнения — возможная бизнес-модель, не решённый факт.

## Распространение

Первый канал — direct download подписанного/notarized macOS приложения. Apple требует учитывать hardened runtime, в том числе при hosting plugins.[^2] Entitlements выдаются по фактической потребности; не отключать все проверки глобально ради одного plugin. App Store оценивается позже после hosting/sandbox proof.

Перед релизом: выбранная лицензия и notices, source archive по release SHA, SBOM, dependency lock, clean-machine install, microphone permission flow, plugin discovery, upgrade/recovery fixtures. Signing credentials не хранятся в git. Автообновление — отдельная задача после ручного release path.

## Устойчивость

Основная стоимость — разработка/проверка на реальных устройствах, сопровождение compatibility и UX. ИИ-инструменты могут ускорить код, но не заменить тесты с audio interface и прослушивание. Возможные источники финансирования: donations/sponsors, платные готовые сборки/поддержка, дополнительные first-party инструменты. Локальная запись и доступ к сохранённым проектам должны оставаться независимыми от облачного аккаунта.

[^1]: Mozilla, [MPL 2.0 FAQ](https://www.mozilla.org/en-US/MPL/2.0/FAQ/).
[^2]: Apple, [Notarizing macOS software before distribution](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution).
