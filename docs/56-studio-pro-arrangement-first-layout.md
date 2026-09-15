# Studio Pro-inspired arrangement-first layout — 1.29.0

Версия 1.29.0 уточняет направление Studio Pro reference: главным рабочим местом является arrangement, а не одновременно открытые панели. My DAW сохраняет собственные элементы, названия и поведение; чужие assets, branding и исходный код не используются. Формат музыкального проекта остаётся v14.

## Компоновка

Верхняя часть окна содержит один плотный toolbar: project/edit actions, import, создание дорожки и bus, workflow, range/grid и file actions. Повторного текстового ряда transport controls нет.

Transport и статус расположены в нижней компактной панели. Запись, Play, Stop, rewind и loop используют тот же action boundary, что и keyboard commands; позиция, tempo, save state и подсказки остаются видимыми без сокращения arrangement.

Слева закреплена 195-point колонка track headers. Она вертикально синхронизирована с timeline и остаётся на месте при horizontal scroll. В центральной области arrangement получает высоту и ширину, достаточные для одновременной работы с клипами, waveforms, grid и ruler.

| Режим | Arrangement | Inspector | Mixer |
|---|---|---|---|
| Создание | Основная область окна | Скрыт | Скрыт |
| Запись | Основная область окна | Скрыт | Скрыт |
| Сведение | Остаётся видимым | Показан | Показан расширенным |
| Мастеринг | Остаётся видимым; выбран Master | Показан | Показан расширенным |

Выбор режима — локальное состояние окна. Он не меняет клипы, routing, automation, revision или `.mydawdraft`.

## UX и accessibility

Импорт WAV по-прежнему использует явный `NSOpenPanel`: приложение не запрашивает постоянный доступ ко всей папке «Документы». Новый track/clip выбирается автоматически; его параметры доступны в Inspector после перехода к подходящему рабочему режиму.

Mixer strips сохраняют отдельные AX roles: stereo meter — level indicator, громкость — slider, pan — slider, R/M/S — channel-specific buttons. Accessibility increment/decrement и стрелки вверх/вниз у fader продолжают менять gain с теми же begin/change/end callbacks, что и mouse gesture.

## Локальная проверка

`script/build_and_run.sh` собирает и запускает локальный bundle для ручной проверки. Visual QA выполняется на пустом draft, на arrangement с дорожками и импортированным WAV, а также после переключения в Сведение и Мастеринг. Проверяется, что Создание/Запись не оставляют permanent Inspector/Mixer, track headers имеют 195 pt, а Сведение/Мастеринг возвращают оба dock.

AX QA повторяет проверку channel meter/fader/pan: в accessibility tree видны самостоятельные controls, Increment на fader меняет gain на 0,5 dB, revision растёт один раз, а Inspector и channel strip остаются синхронизированы. Это локальный UI/AX proof, не слуховая оценка.

Прослушивание, внешний hardware input/output, полный VoiceOver spoken audit, физическая клавиатура и пользовательская acceptance-сессия в этот срез не входят.
