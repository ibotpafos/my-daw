# Installed AU scanner and parameters — 1.11.0

15 сентября 2026. Продолжение B-012 убирает фиксированный каталог как единственный путь. Системные Apple AU остаются быстрым стартовым набором, а кнопка `Сканировать AU` запускает полный поиск установленных effect и music-effect компонентов в фоне.

## Изоляция сканирования

`daw_au_scan_helper` устанавливается рядом с executable приложения. Main process запускает helper один раз для enumeration, затем новый helper process для каждого component tuple. На каждый probe действует timeout 3 секунды. Helper проверяет instantiate, stereo Float32 48 kHz configuration, initialize, latency и state. Crash, timeout, ненулевой exit status и повреждённый ответ помещают только этот компонент в quarantine; остальные продолжают сканироваться.

C bridge оформляет scan как фоновую job. UI опрашивает её без блокировки главного потока, после завершения заменяет session catalog списком прошедших probe и показывает количество доступных и quarantined компонентов. Результат атомарно сохраняется в bounded cache внутри Application Support и используется при следующем запуске. Формат cache уже предусматривает bundle path/version, но текущий helper пока не заполняет эти поля, поэтому автоматическая invalidation после обновления конкретного AU остаётся следующим шагом.

Успешно просканированный сторонний AU при добавлении пока размещается в общем in-process Renderer. Scanner защищает discovery и probe, но не изолирует дальнейший DSP. Полная crash isolation активного плагина требует отдельного host service и shared audio buffers.

## Параметры и project state

Кнопка `Параметры…` читает global Audio Unit parameter list из временного instance, восстановленного из сохранённого state. UI показывает native range и значение каждого параметра; read-only параметры отключены. После изменения параметра control path:

1. восстанавливает предыдущий AU state во временный instance;
2. применяет новое native value;
3. заново получает полный binary property-list state и latency;
4. одной domain-командой заменяет state insert и создаёт Undo revision;
5. сбрасывает prepared transport, чтобы playback и export получили одинаковое состояние.

Native vendor editor пока не встроен. Generic controls дают рабочий и сохраняемый путь для AU, которые корректно публикуют parameter metadata.

## Missing plugin

При открытии проекта component tuple сохраняется даже без установленного AU. UI показывает `MISSING`, отключает parameter/bypass controls и оставляет удаление. Renderer пропускает unavailable insert, увеличивает наблюдаемый `pluginErrors` и продолжает сухим сигналом. Это позволяет открыть, пересохранить и экспортировать проект без потери сохранённого plugin state.

## Tracktion comparison

Изолированный Tracktion spike использует общий `b011-routing-fixture.json`: source track с post-fader send -6 dB, `AuxSendPlugin`, `AuxReturnPlugin` и implicit master. Это подтверждает переносимость базовой routing-семантики. Nested buses, pre-fader mute/pan, automation, PDC и third-party hosting parity ещё не покрыты.

## Границы текущего шага

- format проекта остаётся v10: структура plugin state не изменилась;
- latency видна, но ещё не компенсируется;
- cache хранится между запусками, но ещё не инвалидируется по версии bundle;
- helper не является runtime sandbox для активного DSP;
- сборка создана без отдельного QA-прогона по текущей установке пользователя.
