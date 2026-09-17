# 80. AU MusicDevice instrument source

## Задача

MIDI-only дорожка уже является голосом renderer: для неё создаётся нулевой
stereo buffer, `MidiTrackPlan` выдаёт bounded `PreparedMidiEvent` с offset внутри
текущего блока, а track insert chain получает эти события вместе с аудио. До
этого среза Apple AU host принимал только effect-style processing и возвращал
ошибку при любом непустом MIDI span, поэтому Audio Unit instruments не могли
стать источником звука.

Этот срез подключает `kAudioUnitType_MusicDevice` к уже существующему
instrument-source пути без новой модели дорожки и без отдельного MIDI graph.

## Каталог и isolated scan

`daw_au_scan_helper --list` теперь перечисляет три допустимых класса:

- `kAudioUnitType_Effect`;
- `kAudioUnitType_MusicEffect`;
- `kAudioUnitType_MusicDevice`.

Формат строки scanner helper не меняется: `type/subtype/manufacturer`, имя,
bundle path и version. Поэтому parser/cache сохраняют прежний контракт и
MusicDevice проходит через тот же quarantine/available lifecycle.

Probe MusicDevice не пытается настроить input bus: инструмент является
генератором. Output остаётся stereo float32 non-interleaved / 48 kHz, block
limit — тот же bounded host maximum.

## Runtime

`prepareAudioUnit()` выбирает реализацию по durable `PluginInsert.type`:

- Effect/MusicEffect → `AppleEffect`;
- MusicDevice → `AppleMusicDevice`.

`AppleMusicDevice`:

1. создаёт AU instance в выбранном hosting mode;
2. восстанавливает `ClassInfo` state;
3. настраивает stereo output и maximum frames per slice;
4. инициализирует unit и кэширует latency;
5. принимает ту же parameter automation lane, что обычный AU;
6. преобразует каждый `PreparedMidiEvent` в Note On/Note Off через
   `MusicDeviceMIDIEvent`, сохраняя `sampleOffset` внутри блока;
7. вызывает `AudioUnitRender` с абсолютным host sample time;
8. суммирует generated output с входным dry signal;
9. при ошибке восстанавливает dry input и best-effort отправляет All Notes Off.

Аддитивная семантика важна не только для MIDI-only дорожки, где dry buffer
изначально нулевой. Общий C ABI хранит AU как обычный insert и исторически не
запрещает выбрать scanned component для bus/master. MusicDevice в таком месте
не должен стирать уже собранный микс: без MIDI он оставляет dry signal как был,
а с MIDI добавляет generated output поверх него.

Обычный `AppleEffect` теперь игнорирует track MIDI span. Это обязательно для
последовательной цепочки `instrument → effect`: renderer по контракту передаёт
MIDI lane всем inserts дорожки, а эффект после синтезатора не должен превращать
сам факт наличия MIDI в ошибку блока.

## Что не меняется

- Domain `PluginInsert` уже хранит AU component triple, state, hosting mode и
  parameter automation — новый durable type не нужен.
- Renderer уже создаёт instrument voice для MIDI/instrument track и уже
  передаёт frame-aware MIDI в `processChain`.
- Project schema и C ABI не меняются.
- AU effect allow-list `supportedAudioUnits()` остаётся отдельным маленьким
  встроенным каталогом; пользовательский scanner catalog расширяется MusicDevice
  независимо от него.

## Проверки

Автоматически/без железа:

- `tests/au_host_probe.cpp` передаёт MIDI span обычным Apple effects и требует,
  чтобы они продолжали успешно рендерить;
- `e2e_plugin_catalog` запускает deterministic fake helper с component type
  `aumu` и проверяет, что isolated scan, apply и C ABI catalog сохраняют
  MusicDevice type/subtype/manufacturer без фильтрации;
- scanner parser/cache по-прежнему используют прежний six-field helper protocol;
- macOS CI компилирует standalone CoreMIDI/MusicDevice probes с `-Werror`.

На физическом Mac:

```sh
./scripts/test-au-music-device.sh
./scripts/test-au-music-device.sh <subtype-hex> <manufacturer-hex> 3
```

После merge отдельным acceptance gate остаётся пользовательский прогон внутри
My DAW: добавить MIDI clip, выбрать найденный AU instrument, услышать его через
реальный output device, сохранить/открыть проект и повторить playback. Vendor
compatibility matrix и native editor UI этим срезом не объявляются закрытыми.
