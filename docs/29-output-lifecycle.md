# Наблюдаемый lifecycle аудиовыхода — 1.4.0

15 сентября 2026. Core Audio adapter теперь хранит состояние `idle`, `running`, `stopped`, `deviceLost`, `stalled` или `callbackError`. Новый `daw_get_output_status` возвращает это состояние, текущий `AudioDeviceID`, монотонное поколение запуска, число callbacks и число некорректных callback buffers.

## Потоки и остановка

Render callback только пишет звук, счётчики и при неверном `AudioBufferList` заполняет доступные buffers нулями. Он не освобождает AudioUnit и не вызывает UI. Проверка устройства и 2-секундного stall остаётся на owner/control thread. Остановка идёт в порядке: `renderer.playing=false`, `AudioOutputUnitStop`, `AudioUnitUninitialize`, `AudioComponentInstanceDispose`; только после этого владелец может уничтожить Renderer и его immutable playback plan.

При смене или потере default output adapter останавливается и сохраняет причину. macOS UI показывает отдельный текст для device change, stall и buffer error, после чего новый Play создаёт запуск на текущем системном выходе.

## Доказательство и открытый gate

Локальный hardware smoke три раза подряд выполнил silent start → callbacks → stop → отсутствие callbacks после teardown → restart. Во всех трёх проходах пришло 33 callback и 16 896 кадров; поколения были 1, 2 и 3. Тест не открывает микрофон и не является loopback или listening proof.

Физическое отключение/переключение устройства во время playback пока не выполнено. Property listener также ещё не установлен: прототип обнаруживает изменение при 100 мс control poll. Поэтому B-005 отмечен частично готовым, а не закрытым.
