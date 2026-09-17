# 85. Настоящий VST3-инструмент как источник MIDI-дорожки

## Место в плане

Это продолжение [instrument-source](70-instrument-source.md) и
[проверки настоящего SDK](84-vst3-sdk-verification.md), ограниченный срез
B-012/B-017 из [roadmap](11-roadmap.md). ADelay доказывает эффект, но не синтезатор:
раньше VST3-хост безусловно требовал аудиовход, передавал `numInputs=1` и не
активировал event input. Поэтому корректный инструмент без аудиовхода отвергался.
Собственных тембров/DSP нет, используется существующий `PreparedEffect` и SDK.

## Контракт хоста

- Эффект: первый main audio input + stereo main output; инструмент-источник:
  **ноль audio inputs**, первый main event input + stereo main output.
- Состав шин читается через `IComponent`, конфигурация согласуется через
  `setBusArrangements`, после согласования проверяются два аудиоканала.
  Поддержка float32 проверяется явно. Остальные шины не активируются.
- Существующие audio/event inputs и output активируются явно, включая event bus:
  `kDefaultActive` не заменяет `activateBus`. Уничтожение/отказ подготовки
  деактивирует только то, что было успешно активировано этим экземпляром.
- Источник получает `numInputs=0`, `inputs=nullptr` как в обычной обработке,
  так и при применении сохранённого параметра. Его выход не наследует старое
  содержимое in-place буферов; учитываются выходные `silenceFlags`.
- MIDI доставляется только на существующий event bus. Эффект без него продолжает
  обрабатывать звук синтезатора, даже когда получает тот же MIDI span цепочки.
  Сохраняются прежние границы 512 событий/блок и валидация полей/смещений.
- Нет нового протокола IPC, формата проекта, ABI, зависимости или второго
  синтезатора. Изолированный helper использует тот же исправленный хост.

## Готовый инструмент для доказательства

Используется **mda DX10** из `mda-vst3` в уже закреплённом VST3 SDK 3.8.1_build_84:
SDK `3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96`, public.sdk
`586dc5e6c8012c3e4b01c79389375cbe96bdb1da`.
Исходники примера не меняются; mda имеет собственное MIT-уведомление Paul Kellett.
Пример собирается без VSTGUI, не устанавливается в пользовательские каталоги,
не копируется в приложение. Новая регистрация CMake
`DAW_VST3_TEST_INSTRUMENT` требует настоящий bundle и оба SDK helpers.

CTest `vst3_instrument_source` через общий scanner launcher проверяет FUID,
отпечаток, Instrument flag и probe, затем запускает настоящий хост. Проверяются:

1. Нота на кадре 137 при 48 кГц, блоки 64/257/4096, тишина до неё, тот же
   звуковой фрагмент со сдвигом ровно 137 кадров и конечный ненулевой результат.
2. Note Off на кадре 12031: decay удерживается бесконечно, release — короткий.
   Тишина в конце проверяет доставку Note Off, а не естественное затухание.
3. Недопустимые смещения/канал/высота/velocity и переполнение списка не меняют
   входные буферы и не оставляют активную ноту внутри инструмента.
4. Настоящий MIDI-only граф без WAV-источников, Undo/Redo добавления вставки,
   SQLite save/read с теми же нотами/состоянием и повторным звуком.
5. Открытие проекта и фоновый float32 WAV-экспорт через публичный C ABI;
   экспортированный файл декодируется и проверяется, а не только существует.
6. Настоящий isolated helper: параметры и восстановление, MIDI→audio с
   известной дополнительной задержкой 4096 кадров, без dry fallback.

В изоляционном тесте **тестовый поток** оставляет 150 мс между запросами.
Это доказательство правильного IPC и звука, не измерение realtime deadline.
SDK-пример не объявляется полностью ASan/UBSan-инструментированным; санитайзеры
охватывают наш хост/helpers и тест, как в предыдущем SDK-срезе.

## Запуск и артефакты

Workflow `vst3-sdk.yml` собирает цели `adelay` и `mda-vst3` из закреплённого SDK,
передаёт оба пути CMake и требует новый CTest в JUnit. Ошибка, skip или отсутствие
обязательного теста не превращаются в зелёную проверку. Обычный CI без SDK
по-прежнему независим и ничего не скачивает автоматически.

```sh
cmake --build build/vst3-fixtures --target adelay mda-vst3 --parallel 2
cmake -S . -B build/vst3-ci -DDAW_SANITIZERS=ON \
  -DDAW_BUILD_VST3_SCAN_HELPER=ON -DDAW_BUILD_VST3_RUNTIME_HELPER=ON \
  -DDAW_VST3_TEST_PLUGIN=/absolute/path/adelay.vst3 \
  -DDAW_VST3_TEST_INSTRUMENT=/absolute/path/mda-vst3.vst3
cmake --build build/vst3-ci --parallel 2
ctest --test-dir build/vst3-ci -R vst3_instrument_source --output-on-failure --no-tests=error
```

Диагностический artifact содержит `vst3-instrument.wav` и
`vst3-instrument-proof.json`, а также исходники, JUnit и логи. Успех конкретной
сборки публикуется отдельно в PR с точной ревизией: сам этот документ не является
отчётом об исполнении. Runtime-пути плагина внутри тестового проекта не переносятся
на машину пользователя; файл проекта в приложение не встраивается.

## Что остаётся открытым

Совместимость с каждым сторонним VST3, mono/multi-out/sidechain, MPE/note
expression, живой MIDI input monitoring, GUI плагинов, loop/seek поведения
каждого инструмента, realtime performance и физическое прослушивание требуют
отдельных проверок. Весь B-012/B-017 и physical gates этим срезом не закрываются.
UI не перерисован: существующий браузер/инспектор использует исправленный хост.

## Первоисточники

- [mda DX10 processor](https://github.com/steinbergmedia/vst3_public_sdk/blob/586dc5e6c8012c3e4b01c79389375cbe96bdb1da/samples/vst/mda-vst3/source/mdaDX10Processor.cpp).
- [mda fixture build](https://github.com/steinbergmedia/vst3_public_sdk/blob/586dc5e6c8012c3e4b01c79389375cbe96bdb1da/samples/vst/mda-vst3/CMakeLists.txt).
- [VST3 bus activation](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Change%2BHistory/3.0.0/Multiple%2BDynamic%2BIO.html).
- [VST3 processing / inactive trailing buses](https://steinbergmedia.github.io/vst3_dev_portal/pages/FAQ/Processing.html).
