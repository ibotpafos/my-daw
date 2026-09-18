# General Digits — UI references

Набор визуальных концептов для My DAW из обсуждения дизайна интерфейса.
Это референсы компоновки и визуального языка, а не скриншоты работающего
приложения и не подтверждение реализации показанных функций.

**Статус переноса:** завершён. Все 17 PNG сверены с manifest исходного архива по SHA-256, размеру и PNG-заголовку. Оригинальные байты сохранены без обрезки или перекодирования.

## Комплект

Источник: `General_Digits_DAW_UI_Screens.zip`.
16 экранов и один общий обзор. Размер каждого PNG: 1600 × 1000.
Имена и SHA-256 оригиналов зафиксированы в [manifest.json](manifest.json).
Изображения при переносе не должны пересоздаваться, обрезаться или пережиматься.

| Файл | Экран |
| --- | --- |
| `00_contact_sheet.png` | Общий обзор |
| `01_arrange.png` | Аранжировка |
| `02_mixer.png` | Микшер |
| `03_piano_roll.png` | Piano Roll / MIDI editor |
| `04_vocal_comping.png` | Запись вокала и comping |
| `05_audio_editor.png` | Аудиоредактор |
| `06_pitch_editor.png` | Редактор высоты тона |
| `07_drum_editor.png` | Ударные / step sequencer |
| `08_sampler.png` | Сэмплер |
| `09_automation_editor.png` | Автоматизация |
| `10_plugin_rack.png` | Plugin rack / channel strip |
| `11_routing.png` | Маршрутизация сигнала |
| `12_browser.png` | Браузер контента |
| `13_mastering.png` | Мастеринг |
| `14_export_center.png` | Экспорт |
| `15_session_creative.png` | Session / creative mode |
| `16_settings.png` | Настройки |

## Как использовать

Общее направление: тёмные нейтральные поверхности, понятная иерархия панелей,
цветовая идентификация дорожек, приоритет редактируемого материала и контекстные
инструменты. Ориентиры пользователя — Logic Pro и Studio One.

Референсы задают направление, но не заменяют проектирование поведения:
размеры зон нажатия, клавиатурное управление, фокус, VoiceOver, масштабирование,
hover/selected/disabled/error-состояния и действия drag & drop проверяются
в реализации отдельно. Названия сторонних продуктов и числовые параметры
в концептах не являются обязательным техническим заданием.

Эта папка относится только к документации. Не включать изображения в ресурсы
приложения, сборку или runtime; исходный ZIP не дублировать в Git.


## Галерея

![Обзор всех экранов](00_contact_sheet.png)

### Arrange

![Arrange](01_arrange.png)

### Mixer

![Mixer](02_mixer.png)

### Piano Roll

![Piano Roll](03_piano_roll.png)

### Vocal Recording / Comping

![Vocal Recording / Comping](04_vocal_comping.png)

### Audio Editor

![Audio Editor](05_audio_editor.png)

### Pitch Editor

![Pitch Editor](06_pitch_editor.png)

### Drum Editor / Step Sequencer

![Drum Editor / Step Sequencer](07_drum_editor.png)

### Sampler

![Sampler](08_sampler.png)

### Automation Editor

![Automation Editor](09_automation_editor.png)

### Plugin Rack / Channel Strip

![Plugin Rack / Channel Strip](10_plugin_rack.png)

### Routing / Signal Flow

![Routing / Signal Flow](11_routing.png)

### Browser

![Browser](12_browser.png)

### Mastering

![Mastering](13_mastering.png)

### Export Center

![Export Center](14_export_center.png)

### Session / Creative Mode

![Session / Creative Mode](15_session_creative.png)

### Settings / Preferences

![Settings / Preferences](16_settings.png)

Ход внедрения реальных экранов: [IMPLEMENTATION.md](IMPLEMENTATION.md).
