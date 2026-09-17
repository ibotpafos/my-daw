# 79. Hardware acceptance probes

Этот слой не заменяет автоматические CTest/e2e проверки. Он закрывает то, что
headless CI принципиально не может доказать без реального macOS hardware/runtime:
живой CoreMIDI-вход и установленный AU MusicDevice, который действительно
принимает MIDI и рендерит звук.

## CoreMIDI input

Список доступных входов:

```sh
./scripts/test-midi-hardware.sh
```

Прослушивание конкретного `uniqueID` в течение 10 секунд:

```sh
./scripts/test-midi-hardware.sh <unique-id> 10
```

Probe печатает число CoreMIDI packets/bytes и отдельно считает Note On/Note Off.
Ненулевой exit code означает один из двух разных отказов:

- пакетов нет вообще — неверный source, кабель/Bluetooth session, питание или системный MIDI path;
- MIDI-трафик есть, но Note On не пришёл — устройство посылает другой класс сообщений либо тест не сыграл ноты.

Это standalone CoreMIDI proof: он намеренно не зависит от `daw_core`, чтобы
отделить проблему устройства/OS от ошибок bridge/recording path.

## AU MusicDevice

Список установленных инструментов типа `kAudioUnitType_MusicDevice`:

```sh
./scripts/test-au-music-device.sh
```

Строка содержит FourCC subtype/manufacturer и их hex-значения. Для прогона
конкретного инструмента передай hex-пару:

```sh
./scripts/test-au-music-device.sh <subtype-hex> <manufacturer-hex>
```

Опционально третьим аргументом задаётся длительность офлайн-рендера в секундах:

```sh
./scripts/test-au-music-device.sh <subtype-hex> <manufacturer-hex> 3
```

Probe:

1. находит именно `kAudioUnitType_MusicDevice`;
2. создаёт Audio Unit instance;
3. конфигурирует stereo float32 / 48 kHz output и block limit 512 frames;
4. инициализирует unit;
5. отправляет middle-C Note On, затем Note Off;
6. вызывает `AudioUnitRender` блоками;
7. считает peak и non-silent samples;
8. отдельно различает host/init/render error и корректный, но полностью тихий render.

Тихий render не доказывает, что plug-in сломан: некоторые инструменты требуют
preset/sample content или не поддерживают такой headless/offline сценарий. Поэтому
этот код остаётся acceptance probe, а не compatibility verdict.

## Что считается закрытым доказательством

Для живого MIDI gate сохранить вывод одного прогона с реальным контроллером,
где `packets > 0` и `note_on > 0`.

Для standalone AU MusicDevice gate сохранить вывод хотя бы одного системного или
vendor instrument, где probe завершился `ok` и `peak > 0`.

Интеграционный code path внутри My DAW реализован следующим срезом и описан в
[80 AU MusicDevice instrument source](80-au-music-device.md): scanner catalog
принимает `aumu`, renderer уже передаёт frame-aware MIDI lane, а AU host вызывает
`MusicDeviceMIDIEvent` и `AudioUnitRender`. Открытым остаётся физический
пользовательский acceptance: выбрать AU instrument в My DAW, услышать playback
через реальное output device, сохранить/открыть проект и повторить playback.
