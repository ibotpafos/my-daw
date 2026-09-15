# Доступные controls микшера — 1.28.0

Кастомный channel fader раньше рисовался и принимал mouse drag, но не публиковал отдельную accessibility роль. В AX tree был виден стандартный pan slider, поэтому VoiceOver и keyboard users не получали прямого управления громкостью канала.

`MixerFaderView` теперь является accessibility slider с динамической подписью канала, форматированным значением в dB и help. Accessibility increment/decrement и стрелки вверх/вниз меняют gain на 0,5 dB через те же begin/change/end callbacks, что и mouse gesture; это сохраняет command/Undo и automation boundary. `MixerMeterView` публикует роль level indicator, название канала и текущий максимум stereo peak в dBFS. R/M/S и pan получают channel-specific labels при каждом обновлении модели.

В реальном приложении AX tree показал отдельные `Пиковый уровень MASTER`, `Громкость MASTER` и `Панорама MASTER`. Вызов accessibility Increment на master fader изменил видимое и опубликованное значение с `+0.0 dB` на `+0.5 dB`, revision вырос ровно один раз, а Inspector и master control синхронизировались. Фактическое прослушивание VoiceOver речи, полный tab order и проверка с внешней клавиатурой остаются отдельным пользовательским QA gate.
