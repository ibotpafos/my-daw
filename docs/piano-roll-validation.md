# Piano Roll — проверка редактора 2026-09-17

## Проверенное дерево

Функциональный срез безопасности: `e03a76ff5651cf7ece22d3e514f9bddc4e4404b1`.
Финальная визуальная итерация: `45fe0944539f9c26eac486fae472d9441909f1ea`.

[macOS run 35228756130](https://github.com/ibotpafos/my-daw/actions/runs/35228756130), job `105226996482`, завершился успешно. Candidate commit создан до сборки; исходники, build revision и commit.txt соответствуют `45fe094…`. Только после выполнения всех проверок commit опубликован обычным fast-forward в `codex/pro-piano-roll`. Временный workflow публикации и patch удалены; постоянные workflows остаются read-only.

## Выполнено на macOS arm64

| Проверка | Фактический результат |
| --- | --- |
| Полный SDK typecheck AppKit-приложения | Успешно |
| `test-piano-roll-transforms.sh`, Swift 6, warnings-as-errors, -O | 291 проверка; 1000 seeded-разбиений |
| `test-piano-roll-appkit.sh` | 36 проверок с настоящими NSView/NSWindow/NSEvent |
| `build-macos.sh` | arm64 .app собран; codesign --verify --strict успешен |
| `test-piano-roll-bridge.sh` | 23 проверки через настоящий C ABI, без подмены хранилища |
| Все Debug CTest | 31/31 |
| `check_docs.py --require-schemas` | Успешно |
| `e2e_coverage.py --strict` | Успешно; 212 из 214 функций, 2 именованных hardware-гейта |

Swift-набор: Transform 31, Harmony 162, ProState 30, Preview 29, TransactionSafety 39. C ABI-набор проверяет многостраничный клип на 8195 нот, одну ревизию на групповую правку, project Undo/Redo, сохранение/открытие, устаревшие контексты, отказ без потери нот, пустую замену, предел 65536 нот и общий бюджет проекта. Полная замена нот не разбивается на несколько команд Undo.

Нативные проверки покрывают мышь и клавиатуру, preview/apply/cancel, изменение текста перед Apply, поздний mouse-up от отменённого жеста, Cmd-Z при русской раскладке и геометрию окна. Это отдельный AppKit harness плюс независимая интеграция C ABI, а не полная сквозная автоматизация GUI приложения с плагинами.

## Визуальная проверка

PNG `piano-roll-1280.png` и `piano-roll-980.png` получены через AppKit cacheDisplay, не нарисованы как макеты. По первому набору исправлены первоначальная прокрутка к верхним октавам, положение инспектора и ширина полей Velocity Ramp. Повторный набор проверен визуально и автоматическими геометрическими assertions: при первом показе записанные ноты в видимой области; повторное открытие сохраняет viewport; инспектор начинается с Selection и прокручивается в небольшом окне; оба поля Ramp помещаются в строку. Снимки используют ту же darkAqua, что production-приложение.

## Артефакты

GitHub Actions artifact `piano-roll-candidate-35228756130` (`10500831088`) содержит commit.txt, source.zip, publication.txt, логи всех стадий, PNG, build manifest и `My-DAW-arm64.zip`. Артефакты CI имеют ограниченный срок хранения.

SHA-256 исходного .app ZIP:

```text
b04aac72e8ab55bea6dc464a5023a4f022f782e1afd8aa6fe3721b7a38051522
```

Сборка предназначена для Apple Silicon / macOS 14+. Проверялась на hosted macOS 15.7.9, Apple Swift 6.1.2 / Xcode 16.4. Подпись локальная ad-hoc; нотарификация не выполнялась. Полный запуск production-приложения и его звучание этим отчётом не подтверждаются.

## Оставшиеся ограничения

На Linux для функционального дерева выполнены Debug и ASan/UBSan CTest: 27/29. `background_wav_import` и `aiff_import` падают на non-48kHz импорте: ветка всё ещё использует non-macOS resampler stub. Эти тесты не отключались и не переводились в success; Linux CI нельзя считать зелёным. Внесены только необходимые portable compile fixes, не перенос реализации ресемплинга из другой ветки.

Ручными гейтами остаются работа полного GUI с реальными AU/VST3-инструментами, слуховой контроль, физический MIDI, VoiceOver и длительные сессии. CC, expression, pitch bend, aftertouch/MPE и звучащее audition клавиатуры не объявляются реализованными. PR остаётся Draft и автоматически не мержится.

Контракт редактирования: [Piano Roll edit safety](piano-roll-edit-safety.md).
