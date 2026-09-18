# Нативный экспорт: диагностика падений и повторный Save/Open

## Ограниченный срез core readiness

Продолжение [основы DAW](93-core-readiness.md), расследование issue #30.
Однократный SIGSEGV в нативном WAV export после Save/Open пока не локализован.
Зелёные отдельные повторы не доказывают его исправление. Этот срез не меняет
DSP, API/формат проекта, запись или latency compensation и не закрывает #30.

## Исполняемый путь

Существующий `scripts/test-mix-export-ui.sh` компилирует прежний полный DraftApp
и тест с `-g`, сохраняет dSYM для именно этого executable, удаляет старый
`proof.json` и запускает его **один раз** через `scripts/native_test_runner.py`.
Порядок постоянного workspace CI сохранён: сборка → workspace → запись →
экспорт → проверка и упаковка приложения. Ни один прежний сценарий не удалён.

Добавлены фазовые сообщения перед обновлением диапазона и началом экспорта.
После первоначального range export выполняются восемь фиксированных повторов
Save/Open → updateTimelineTools → настоящий export-range job → чтение WAV.
Проверяются document identity, неизменная revision, число кадров и слышимый PCM.
Первый отказ останавливает тест; это не retry-until-green. Реальный Apple DLS
Synth, рендерер, writer, filesystem и C API не заменены. Физический звук/микрофон
не запускаются. Наличие кода тестов само по себе не означает успешного CI.

## Диагностические артефакты

Runner сохраняет `run.json`: точный executable, PID, время запуска, исходный
returncode, итоговый exit code и признак timeout. Отрицательный код сигнала
переводится в обычный shell-код `128 + signal`, ненулевой код никогда не
превращается в успех. Timeout остаётся 124 и явно отделён от crash; перед
остановкой на macOS запрашивается ограниченный `sample` работающего PID.

При неуспехе ограниченное ожидание ищет IPS-отчёт только этого процесса:
имя executable, PID, время запуска, свежий mtime и `bug_type=309`. Apple IPS
содержит два JSON-объекта: metadata в первой строке, payload после неё.
Невалидные, неполные, слишком большие и symlink-отчёты не копируются. Чужие
имена не просматриваются; вся папка DiagnosticReports не выгружается.
Если OS не создала отчёт, `crash_report_found=false` остаётся видимым —
отсутствующий стек не заменяется предположением. Точный executable и dSYM
упаковываются вместе, чтобы последующая сборка не подменила символы.

`tests/test_native_test_runner.py` использует стандартные unittest/subprocess
и временные synthetic IPS, включая чужой PID/имя, устаревшее время, malformed
JSON, symlink/size bounds, реальные ненулевые exit/сигнал/timeout и одну попытку.
Это не проверка macOS CrashReporter; его физическое наличие отдельно от
переносимой проверки parser/runner. Новых зависимостей нет.

## Источники и границы

- [Apple: interpreting JSON crash reports](https://developer.apple.com/documentation/xcode/interpreting-the-json-format-of-a-crash-report)
- [Apple: analyzing a crash report](https://developer.apple.com/documentation/xcode/analyzing-a-crash-report)

Используются штатные CrashReporter IPS, dsymutil и sample, а не собственный
signal handler внутри приложения. Нельзя закрывать #30 только этим срезом:
нужно установить дефект, добавить проверяющую его регрессию и подтвердить
исправление. Реальный audio/driver/VoiceOver QA и полная P0-04 остаются открыты.
