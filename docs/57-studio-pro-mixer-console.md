# Studio Pro-inspired mixer console — 1.30.0–1.31.0

Версия 1.30.0 развивает B-018 по предоставленному Studio Pro reference: arrangement остаётся сверху, а в нижней половине режимов Сведение и Мастеринг расположен единый console bank. My DAW использует собственные компоненты, цвета и поведение; формат проекта остаётся v14.

## Структура консоли

После уточнения по второму Studio Pro reference в 1.31.0 каждая дорожка, bus и master представлены плотной вертикальной полосой шириной 74–80 pt. Полосы заполняют высоту нижней панели и прокручиваются по горизонтали, поэтому проект с большим числом каналов сохраняет привычную console-модель.

Порядок внутри полной полосы:

1. Inserts и Sends summaries с bypass, destination, gain и pre/post состоянием, когда такие элементы существуют.
2. Текущий output route.
3. Pan и доступные channel controls.
4. Числовой gain, длинный stereo meter и вертикальный fader.
5. Automation state и цветной footer с номером и именем канала; полное имя остаётся в tooltip и accessibility label.

Пустые Inserts/Sends показывают явный слот `—`, чтобы геометрия соседних полос не прыгала. Между группами track, bus и master добавлен увеличенный отступ; bus получил верхний rail, master — усиленный боковой rail и маршрут `MAIN`.

Track показывает R/M/S. Bus показывает M и pan; Solo пока скрыт, потому что domain/C ABI ещё не предоставляет bus-solo command. Master показывает output, inserts, meter, fader и automation; неработающие R/M/S и pan не рисуются.

## Размер и подробные операции

В Сведении и Мастеринге arrangement и console по умолчанию получают примерно половину высоты окна. Пользователь может менять границу split view. При уменьшении mixer ниже 286 pt полосы переходят в compact state и скрывают Inserts/Sends summaries, сохраняя основные channel controls.

Существующие редактируемые routing, sends, inserts, take/comp и plug-in actions не дублируются постоянно под полосами. Кнопка «Детали канала» раскрывает прежнюю полную панель операций на 180 pt; повторное нажатие возвращает полноразмерные strips.

## Поведение и accessibility

Track/bus/master получают фактическое имя output из routing snapshot; master показывает `Output 1–2`. Meter updates меняют существующие meter views без пересоздания полос во время polling. Resize только пересчитывает frames, сохраняя горизонтальную позицию и focus.

Каждый доступный meter, fader, pan, R/M/S и footer имеет самостоятельный AX role/label. Скрытые или неработающие controls исключены из accessibility tree. Tooltip и AX label сохраняют полное имя длинного канала, даже когда видимый footer сокращён.

## Локальная проверка

Visual QA выполнен в отдельных `My DAW Next.app` и `My DAW Candidate.app`, пока основной bundle оставался открытым. Проверены пустой mixer, 3 tracks + bus + master, 8 tracks + 2 buses + master, полноразмерное состояние, горизонтальная плотность и раскрытие/скрытие «Детали канала».

AX QA подтвердил отдельные controls для track/bus/master, отсутствие R на bus/master, отсутствие неработающих Solo на bus и pan/M/S/R на master. Mute первой дорожки синхронно изменил track header, Inspector и strip, увеличив revision с 10 до 11. Accessibility Increment на её fader изменил gain с `+0.0 dB` на `+0.5 dB` и увеличил revision ровно один раз, с 11 до 12.

Повторная QA 1.31.0 выполнена в чистом несохранённом проекте основной локальной сборки: 4 tracks + 2 buses + master одновременно видны в нижней половине окна, sections выровнены, группы отделены, а справа остаётся место для следующих каналов. AX tree подтвердил нумерацию, Inserts/Sends/output/automation labels и отдельные fader/meter controls. Mute первой дорожки переключился в `on`, Accessibility Increment изменил её gain с `+0.0 dB` на `+0.5 dB`, а revision вырос с 6 до 8 за две операции.

Эта проверка подтверждает локальную геометрию, actions и AX-семантику. Прослушивание, внешний hardware input/output, VoiceOver spoken audit и физическая пользовательская сессия остаются отдельными gates.
