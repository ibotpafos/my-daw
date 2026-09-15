# Full-duplex loop recording — 1.8.0

15 сентября 2026. Если включён loop и аудиодорожка armed, Record запускает один AUHAL AudioUnit с включёнными input bus 1 и output bus 0. Один realtime callback вызывает `AudioUnitRender` для mono input и `Renderer::render` для stereo output с общим `AudioTimeStamp`. Так transport и число принятых входных кадров продвигаются одним hardware clock.

## Ограничение устройства

Точный режим требует, чтобы default input и default output указывали на один Core Audio device. Для раздельных интерфейсов пользователь должен создать aggregate device в Audio MIDI Setup и выбрать его для входа и выхода. Приложение отклоняет запуск до создания recovery writer, если устройства различаются, и требует nominal rate 48 кГц. Автоматический realtime resampling и drift correction пока не реализованы.

## Проходы и commit

Запись всегда начинается с `loop_start`. Capture writer принимает линейную последовательность входных кадров до 60 секунд. На Stop `splitLoopPasses` режет её по точной длине `[loop_start, loop_end)`, включая последний неполный проход. Domain-команда `addTakes` добавляет все полученные sources одной ревизией: Undo/Redo действует на всю запись целиком. Число проходов заранее ограничено свободными слотами дорожки, общим лимитом 32 sources и 64 MiB decoded PCM.

Format v8 уже хранит несколько take rows и их PCM, поэтому схема проекта не менялась. Тест roundtrip проверяет два прохода после batch commit. Старый `.mydawtake` v1 остаётся линейным безопасным recovery-файлом: после аварии он восстанавливается новой дорожкой, а не угадывает исходную armed lane. Project identity, target track и loop geometry войдут в recovery metadata v2.

## Интерфейс

Во время записи transport показывает длительность и число уже захваченных проходов. Playhead движется по активному циклу на основе счётчика входных кадров. После Stop новые lanes появляются вместе, выбирается последний созданный дубль, а comp остаётся неизменным до явного действия пользователя.

## Проверка

Debug-тесты покрывают разбиение десяти кадров в проходы `4 + 4 + 2`, сохранение исходных sample offsets, ошибочную нулевую длину, batch Undo/Redo и format v8 roundtrip. macOS target компилирует единый AUHAL adapter и отдельный `daw_duplex_hardware_smoke`; он намеренно не входит в CTest, потому что открывает микрофон и реальный output.

## Преролл (1.58.0)

`daw_set_record_preroll(session, frames)` — до 30 секунд (сверх — «Pre-roll must be 0-30 seconds»), геттер-зеркало `daw_get_record_preroll` с NULL-гейтом. Применяется только луп-записи: `makeDuplex(..., prerollFrames)` сдвигает старт транспорта на `min(preroll, startFrame)` назад от punch-in — ведущий кусок слушается с кликом (метроном дуплекса), а `RecordingWriter` с `skipFrames` выбрасывает ровно эти кадры из потока: лейбл тейка остаётся на `startFrame`, так что `splitLoopPasses` и границы проходов не меняются вовсе. Одиночный входной захват преролл игнорирует — там нет воспроизведения, и тихий ведущий кусок был бы обманом. UI: меню «Проект → Преролл записи (луп-режим)» (Выключен/1/2/4/8 с галочкой), значение живёт в UserDefaults (`transport.prerollSeconds.v1`) и применяется к мосту при старте. Тест writer-каскада в `recording_recovery`: из 4×1000 кадров со skip 1500 остаются ровно 2500, первый сэмпл — середина второго блока, recover-заголовок хранит punch-фрейм; C-гейты — в `pure_c_bridge`. Живые уши на «старт раньше, запись вовремя» — physical-гейт, открыт.

Ручной запуск после явного выбора одного duplex/aggregate device:

```sh
./build/debug/daw_duplex_hardware_smoke
```

В этой итерации команда не запускалась. Поэтому фактическая работа конкретного устройства, round-trip latency, отсутствие xruns, качество записи и listening acceptance не доказаны.
