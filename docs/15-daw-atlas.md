# Атлас DAW и музыкальных рабочих сред

## Охват и чтение таблиц

Атлас охватывает **50 продуктов и семейств**: основные desktop DAW, открытые проекты, трекеры, редакторы для mastering/post, mobile и web studios. Это широкая карта для проектирования, не исчерпывающий каталог всех когда-либо выпущенных музыкальных программ. Аппаратные workstation и groovebox firmware исключены; смежное музыкальное ПО и исторические свидетельства отмечены отдельно.

Дата проверки — 14 сентября 2026 года. «Сильная сторона» означает характерный поддержанный сценарий, а не результат сравнительного теста. Столбец «урок» — наша интерпретация. Для закрытой DAW не выдумывается внутренний стек по внешнему виду. Язык SDK не равен языку ядра. Название framework в build file относится к проверенной ветке, не автоматически ко всем релизам.

Уровни свидетельств: **A** — исходники/build manifest; **B** — официальная документация SDK или рассказ производителя; **H** — историческое свидетельство, не текущая полная архитектура; **U** — внутренний стек не установлен по использованным первичным источникам. Цена и полный список OS по версиям не входят в эту таблицу: они меняются и не определяют архитектуру.

## Универсальные и творческие desktop DAW

| № / Продукт | Для чего и сильная сторона | Что известно о реализации | Урок для My DAW и компромисс |
|---|---|---|---|
| 1. Logic Pro | Интегрированная Mac-студия, инструменты и AI-assisted production | **U:** закрытое ядро; product page не раскрывает language breakdown. Нельзя утверждать «написана на Swift» | Изучить целостный опыт и quality bar; широта функций увеличивает сложность. [Apple](https://www.apple.com/logic-pro/) |
| 2. GarageBand | Быстрый старт записи и создания музыки на Mac | **U:** языки ядра/GUI публичной страницей не определены | Дать полезный результат до знакомства со всеми настройками; простота имеет границы. [Apple](https://www.apple.com/mac/garageband/) |
| 3. Studio One / Fender Studio Pro | Единый маршрут производства; современная версия продолжает платформу Studio One | **B/U:** Fender подтверждает происхождение платформы; полный стек не раскрыт | Проверить действия drag/drop и переход к финальному миксу в реальной сессии, не копировать бренд/UI. [Fender](https://www.fender.com/products/fender-studio-pro) |
| 4. Ableton Live | Session/Arrangement, импровизация, клипы, comp | **B:** Max for Live; новый workflow Extensions SDK — JavaScript/Node.js, public beta. Это не описание языка ядра | Отделять создание звука от преобразования структуры проекта. [Live concepts](https://www.ableton.com/en/manual/live-concepts/), [SDK](https://www.ableton.com/en/blog/introducing-extensions-sdk/) |
| 5. Bitwig Studio | Модуляция, Grid, гибкое hosting и controller API | **B:** документированы host modes; controller scripts Java/JavaScript. Утверждение о полном Java UI/C++ engine здесь не верифицировано | Проект открывается независимо от готовности plugins; recovery — часть UX. [Architecture](https://www.bitwig.com/modern-foundations/), [controllers](https://www.bitwig.com/userguide/latest/midi_controllers/) |
| 6. FL Studio | Pattern/beat workflow, развитая система инструментов и native plugins | **H/B:** исторический case study подтверждает Delphi; native plugin SDK поддерживает Delphi/C++. Современный состав ядра не установлен | Выбирать язык под ownership и экосистему, а не моду; pattern модель не обязана определять запись вокала. [Case study](https://www.embarcadero.com/case-study/image-line-software-case-study), [SDK](https://www.image-line.com/developers) |
| 7. REAPER | Настраиваемая workstation, автоматизация пользовательских процессов | **B:** C/C++ Extension SDK, ReaScript, WDL C/C++ toolkit. Это подтверждение доступных слоёв, не открытость всего REAPER | Команды и API могут быть главным активом экосистемы; настройка не должна быть обязательной для старта. [SDK](https://www.reaper.fm/sdk/plugin/plugin.php), [WDL](https://www.cockos.com/wdl/) |
| 8. Cubase | Композиция, MIDI, Expression Maps, VariAudio | **B/U:** официальный VST3 C++ SDK открыт, полный стек Cubase не раскрыт этим фактом | Музыкальное время/артикуляции требуют собственной модели, не набора случайных MIDI events. [Features](https://www.steinberg.net/cubase/features/) |
| 9. Nuendo | Audio post для кино, ТВ и игр, ADR и delivery | **U:** продуктовые возможности раскрыты, полный внутренний стек нет | Post — самостоятельная область требований; не включать её в первый vocal MVP. [Features](https://www.steinberg.net/nuendo/features/) |
| 10. Pro Tools | Запись, монтаж и профессиональные audio-post процессы | **U:** product page не раскрывает достоверный состав ядра/GUI | Приоритет точного монтажа, session interchange и совместимости важнее числа AI-функций. [Avid](https://www.avid.com/pro-tools) |
| 11. Reason | Виртуальный rack, кабели, единое поведение устройств | **B:** Rack Extension SDK — C++ DSP и host-supplied GUI components; это не весь стек DAW | Самый прямой ориентир для «модуль ощущается частью станции»; единые правила ограничивают произвольный UI. [SDK guide](https://developer.reasonstudios.com/documentation/rack-extension-sdk/4.1.0/rack-extension-dev-guide), [Rack](https://www.reasonstudios.com/reason-rack) |
| 12. Digital Performer | Sequencing и организация нескольких музыкальных частей через Chunks | **H/U:** Chunks описаны в официальном руководстве; языки реализации не установлены | Позднее изучить варианты/сцены проекта. Руководство историческое, текущие details перепроверять. [MOTU manual](https://cdn-data.motu.com/manuals/software/Digital%20Performer%20User%20Guide.pdf) |
| 13. Waveform | Музыкальное производство в продуктовой экосистеме Tracktion | **A/B:** публичный Tracktion Engine — C++20/JUCE; это не публикация всего Waveform UI | Изучить готовый engine как способ ускорить старт. [Product](https://www.tracktion.com/products/waveform-pro), [Engine](https://github.com/Tracktion/tracktion_engine) |
| 14. Cakewalk Sonar | Традиционная полнофункциональная запись/сведение | **U:** внутренний стек не установлен; не смешивать с Next | Сложившиеся профессиональные сценарии могут жить отдельно от облегчённого продукта. [Sonar](https://www.cakewalk.com/sonar) |
| 15. Cakewalk Next | Быстрое создание песни, простое расположение инструментов, lyrics | **U:** scalable UI не доказывает конкретный framework | Ближайший контрольный конкурент тезису «минимум лишнего». Не считать простоту свободной нишей. [Next](https://www.cakewalk.com/next) |
| 16. Samplitude | Object-based editing: обработка на уровне аудиообъекта | **U:** текущий framework не раскрыт | Clip processing как first-class feature; меньше дублирования треков ради одной фразы. [Boris FX](https://samplitude.com/products/samplitude/), [Objects](https://cdn.borisfx.com/borisfx/Documentation/samplitude-suite-2025/en/Content/Arbeitstechniken%20mit%20Objekten.htm) |
| 17. Sequoia | Профессиональные audio production/mastering процессы семейства Boris FX | **U:** подробный внутренний стек не установлен | Reliability/delivery — отдельная ценность. Старые страницы MAGIX не описывают текущую продуктовую принадлежность. [Boris FX announcement](https://www.samplitude.com/news/three-pro-audio-tools-join-the-boris-fx-lineup/) |
| 18. Harrison Mixbus | Микшер с выраженной консольной логикой и встроенной обработкой | **H/B:** Ardour lineage подтверждён разработчиками; собственные mixer/DSP слои Harrison | Готовая логика канала уменьшает setup. Отличие может быть в signal flow, а не новом движке. [Ardour developer explanation](https://discourse.ardour.org/t/harrison-mixbus32c/109165) |
| 19. Mixcraft | Доступное по структуре all-in-one music production | **U:** точный стек не раскрыт product page | Проверить onboarding и готовые musical assets; простота — самостоятельная продуктовая работа. [Acoustica](https://acoustica.com/products/mixcraft) |
| 20. Renoise | Tracker-подход, детальное pattern/sample редактирование | **B/U:** публичная extensibility через scripting; весь внутренний стек не установлен | Keyboard-first workflow может быть быстрым без привычного горизонтального UI. [Renoise](https://www.renoise.com/products/renoise) |
| 21. n-Track Studio | Multitrack production, эффекты и работа на разных устройствах | **U:** платформенность не доказывает общий framework | Сравнить «начать запись» на небольшом экране; не выбирать его стек по догадке. [n-Track](https://ntrack.com/) |
| 22. MultitrackStudio | Последовательная домашняя запись и live multitrack | **U:** implementation languages не установлены | Очень ясный сценарий «добавлять партии по очереди» помогает не раздувать интерфейс. [Bremmers Audio](https://www.multitrackstudio.com/) |

## Открытые DAW и sequencers

| № / Продукт | Сильная сторона / назначение | Подтверждённый технический слой | Что изучить |
|---|---|---|---|
| 23. Ardour | Multitrack audio/MIDI и открытая архитектура | **A:** C++/C, GTK/gtkmm UI lineage, waf build; GPL-family | Session/engine разделение, маршрутизацию, automation, реальные edge cases. [Repo](https://github.com/Ardour/ardour), [UI build](https://github.com/Ardour/ardour/blob/master/gtk2_ardour/wscript) |
| 24. LMMS | Pattern composition, piano roll, встроенные синтезаторы | **A:** C++/Qt, CMake; GPL-2.0 family | Структуру музыкальных редакторов. Не использовать как доказанный backend вокальной записи. [Repo](https://github.com/LMMS/lmms), [build](https://github.com/LMMS/lmms/blob/master/CMakeLists.txt) |
| 25. Qtractor | Linux audio/MIDI multitrack | **A/B:** C++/Qt, JACK audio, ALSA MIDI, GPL-2.0-or-later | Небольшую ясную sequencer архитектуру и integration boundaries. [Repo/README](https://github.com/rncbc/qtractor) |
| 26. MusE | Audio/MIDI sequencing на Linux | **A:** C++17/Qt5 в проверенном build manifest | MIDI/audio domain и традиционный редактор; не путать с Muse Group. [Repo](https://github.com/muse-sequencer/muse), [build](https://github.com/muse-sequencer/muse/blob/master/src/CMakeLists.txt) |
| 27. Rosegarden | Композиция с notation/MIDI и audio | **B:** C++/Qt по developer documentation | Связь нотного представления и musical time. [Product](https://www.rosegardenmusic.com/), [development](https://www.rosegardenmusic.com/wiki/dev:contributing) |
| 28. Zrythm | Автоматизация, современный UI, развивающаяся DAW | **A/B:** v2 alpha — C++23, Qt6/QML и JUCE; старый GTK stack относится к прежнему поколению | Цена смены framework и отличие alpha от production. Лицензию конкретной версии читать отдельно. [v2 announcement](https://forum.zrythm.org/t/zrythm-v2-0-0-alpha-1-released/596), [build](https://github.com/zrythm/zrythm/blob/master/CMakeLists.txt) |
| 29. Helio | Минималистичный музыкальный sequencer | **A:** C++ source и JUCE submodule; не full recording DAW | Лёгкость композиционного интерфейса и границы узкого продукта. [Repo](https://github.com/helio-fm/helio-sequencer), [JUCE dependency](https://github.com/helio-fm/helio-sequencer/blob/develop/.gitmodules) |
| 30. ossia score | Интерактивный intermedia sequencer | **A:** C++/Qt6, отдельная экосистема processing/integration | Временные сценарии и расширения за пределами обычного mixer. [Repo](https://github.com/ossia/score), [build](https://github.com/ossia/score/blob/master/CMakeLists.txt) |

Открытость исходников полезна для изучения, но не делает копирование совместимым с выбранной лицензией My DAW. До заимствования кода проверять license exact files, transitive dependencies и версию. Для Zrythm особенно не переносить историческую лицензионную метку без проверки текущего `LICENSE`.

## Редакторы, mastering/post и mobile/web

| № / Продукт | Класс и сильная сторона | Стек и достоверность | Урок |
|---|---|---|---|
| 31. Audacity | Audio editor/recorder, analysis, обработка файлов | **A:** current master C++20 + Qt6/QML integration в build; описание только как wxWidgets устаревает | Изучить хранение/обработку аудио; не приравнивать к универсальной MIDI DAW. [Product](https://www.audacityteam.org/), [build](https://github.com/audacity/audacity/blob/master/CMakeLists.txt) |
| 32. Adobe Audition | Waveform/multitrack и audio post | **U:** конкретный core/UI stack не установлен | Разделение destructive file editing и multitrack требует ясного UX. [Adobe](https://www.adobe.com/products/audition.html) |
| 33. WaveLab | Mastering, Audio Montage, анализ и batch | **U:** public features не раскрывают engine language | Mastering — контроль delivery/измерений, а не только limiter preset. [Steinberg](https://www.steinberg.net/wavelab/) |
| 34. DaVinci Resolve Fairlight | Audio post внутри video workflow | **U:** полный stack закрыт; здесь не выводится из Resolve UI | Общая временная модель с видео удобна, но существенно расширяет scope. [Blackmagic](https://www.blackmagicdesign.com/products/davinciresolve/fairlight) |
| 35. Cubasis | Mobile audio/MIDI production | **U:** language mix не раскрыт product page | Touch-first не означает просто уменьшенный desktop UI. [Steinberg](https://www.steinberg.net/cubasis/) |
| 36. FL Studio Mobile | Mobile composition/recording/mix | **U:** нельзя переносить Delphi desktop lineage на mobile implementation | Обмен между устройствами проектировать как отдельный workflow. [Image-Line](https://www.image-line.com/fl-studio-mobile) |
| 37. Audio Evolution Mobile | Mobile multitrack с недеструктивным монтажом | **B/U:** reference-based editing описан производителем; язык не установлен | Даже mobile продукту нужны правильные references и Undo. [eXtream](https://www.extreamsd.com/index.php/products/audio-evolution-mobile-for-ios) |
| 38. Soundtrap | Web studio и совместное создание музыки | **U:** браузерный продукт; конкретные engine/backend языки не подтверждены | Collaboration/onboarding полезны, но не доказывают пригодность web shell для low-latency native hosting. [Soundtrap](https://www.soundtrap.com/musicmakers) |

## Дополнительные архитектурные семейства

| № / Продукт | Класс и сильная сторона | Стек и достоверность | Урок |
|---|---|---|---|
| 39. LUNA | Recording/mixing, интеграция UA, расширения консольной обработки; заявлен voice control | **U:** языки ядра не установлены | Голосовое управление уже встречается в DAW; это полезный вход к командам, не уникальность. [Universal Audio](https://www.uaudio.com/products/luna) |
| 40. Pyramix | Профессиональные recording/editing/mastering workflows | **B/U:** подробная функциональная модель в официальном manual, внутренний стек не раскрыт | Изучить source/destination editing и delivery позже; не переносить весь post scope. [Merging manual 15](https://www.merging.com/uploads/assets/Installers/LABET_X.0.5/Pyramix_15.0.5/Pyramix_15_User_Manual.pdf) |
| 41. SoundBridge | Desktop DAW с акцентом на доступность recording workflow | **U:** язык и engine internals не установлены | Изучить компактную рабочую поверхность; маркетинговое «zero latency» не использовать как физический benchmark. [SoundBridge](https://www.soundbridge.io/) |
| 42. Maschine | Groovebox-style beat making, sampling и связь с контроллером | **B/U:** standalone/plugin режимы описаны; языки не раскрыты | Телесное управление и быстрый цикл ideas→patterns; это смежная production-среда. [Native Instruments](https://www.native-instruments.com/products/maschine-3) |
| 43. MPC Software | Desktop sequencing и pad/sampling workflow | **B/U:** изучена страница MPC2; не утверждается, что это новейшая версия всей линейки | Hardware/software workflow может диктовать модель проекта. [Akai](https://www.akaipro.com/mpc-software-2/) |
| 44. SunVox | Компактный модульный synth + tracker | **B:** опубликована библиотека engine для разработчиков; API bindings не определяют полный UI stack | Отделяемый engine и маленькие проекты; не заменяет вокальную DAW. [Product](https://www.warmplace.ru/soft/sunvox/), [library](https://www.warmplace.ru/soft/sunvox/sunvox_lib.php) |
| 45. Radium | Graphical tracker/music editor | **A:** открытые native C/C++/Qt слои и embedded Scheme присутствуют в repo | Альтернативная модель editing; фиксировать release, а не произвольный master. [Repo](https://github.com/kmatheussen/radium), [product](https://www.radium.dog/) |
| 46. Auria / Auria Pro | Mobile multitrack production | **U:** внутренние языки не установлены | Проверять плотный audio workflow на touch, не уменьшать desktop controls механически. [Auria](https://auriaapp.com/auria) |
| 47. BandLab Studio | Cloud DAW в браузере/телефоне | **B/U:** delivery и recording/mixing подтверждены; engine/backend language неизвестны | Низкий барьер начала и collaboration, но локальность требует отдельного решения. [BandLab Help](https://help.bandlab.com/hc/en-us/articles/115002945153-Getting-Started-with-the-BandLab-Studio) |
| 48. Audiotool | Browser music studio и модульная среда | **B/U:** browser delivery, полный стек не установлен | Изучить граф устройств и community workflows. Native bridge требует своей проверки. [Audiotool](https://www.audiotool.com/?authuser=0) |
| 49. Soundation | Online studio/tools | **B/U:** web platform, реализации DSP/backend по этим источникам неизвестны | Быстрый доступ через web пригоден для совместной работы и эскизов. [Soundation tools](https://next.soundation.com/studio-tools) |
| 50. Podium | Desktop sequencer; исторический источник по UI/editing | **H/U:** доступно старое официальное руководство; актуальный release/support и стек не установлены | Пример долгоживущего компактного продукта; не делать вывод о нынешней зрелости по старому PDF. [Zynewave manual](https://zynewave.com/files/PODIUM_Guia_de_usuario_de_Podium_v1.0.pdf) |

За пределами этой версии: SAWStudio, REAPER-based custom distributions, исторические energyXT/Orion и многочисленные небольшие sequencers. Они не объявляются исчезнувшими или действующими без индивидуальной проверки. Некоторые исходные URL перенаправлялись или не отдавали проверяемое описание; отсутствие ответа сайта не является доказательством прекращения продукта.

## Что особенно важно для нашей архитектуры

### 1. Расширяемость уже развивается в сторону workflow

Ableton 2 июня 2026 года описал Extensions SDK как публичную бету для Live Suite: JavaScript-инструменты работают с tracks/clips/parameters и запускаются из контекста. Это очень близко к исходной идее My DAW. Отличие нужно доказывать качеством command semantics, preview, локальности и UX, а не утверждением, что никто не умеет менять проект расширениями.[^1]

REAPER и Reason показывают два других пути: широкий программный доступ и строго интегрированные устройства. Для My DAW выбираем ограниченный workflow API с host-rendered controls. Возможность произвольного внешнего кода приходит после реализации прав и failure handling.

### 2. Разделение процессов полезнее языка как маркетинга

Bitwig документирует варианты plug-in hosting. Его обзор использует слово threads для разделения application/engine/plugins, а руководство описывает режимы hosting; из этого нельзя писать собственную спецификацию isolation без проверки реальных process boundaries. В нашей архитектуре process, thread и queue названы отдельно.[^2]

Язык C++ сам по себе не гарантирует realtime, а Swift/Java UI сам по себе не означает плохой звук. Важны deadline, блокировки, ownership, failure containment и долговременное тестирование.

### 3. Unified experience не требует нового алгоритма для всего

Mixbus демонстрирует продуктовую специализацию поверх существующего Ardour lineage: меняются mixer и signal flow. Это аргумент серьёзно проверить Tracktion/Ardour как alternatives, если собственный engine не оправдает стоимость. Нативный новый UI остаётся отдельной работой даже с готовым backend.

### 4. Новая UI-технология может стоить переписывания

Zrythm документирует переход к Qt6/QML/JUCE; объявление v2 прямо помечает alpha. Audacity current source также отличается от исторического стека. Поэтому в нашей команде название framework не заменяет доказательство production maturity.[^3]

### 5. Переносимость — часть доверия

DAWproject описывает audio/note/automation/plugin data interchange. Это основание для будущего экспорта, не обещание безупречного round-trip между всеми DAW. Наш exporter должен показывать потери представления и давать rendered stems fallback.[^4]

## Первые пять сравнительных разборов

| Порядок | Станция | Что измерять на одной сессии |
|---|---|---|
| 1 | Fender Studio Pro | Текущий знакомый маршрут: запись, comp, send, export |
| 2 | Logic Pro | Нативная навигация, контекст, визуальная ясность |
| 3 | REAPER | Один custom action/скрипт, стоимость настройки, Undo |
| 4 | Bitwig | Plugin failure, загрузка проекта, device interactions |
| 5 | Cakewalk Next или GarageBand | Начало без обучения, пределы простоты |

Далее Ableton Extensions SDK и Reason SDK исследуются как независимые API prototypes. Только после task tests заполнять рейтинг неудобств и comparative performance. В этом атласе таких измерений нет.

## Снимки открытых репозиториев

Ниже — HEAD default branch, полученный GitHub API при подготовке; это provenance snapshot, не dependency lock. Отдельные ссылки `master/develop` выше обозначают реально прочитанные build manifests. При воспроизведении исследования использовать соответствующий файл на сохранённом commit, проверяя branch/path.

| Repository | Observed HEAD |
|---|---|
| Ardour/ardour | `9a4b9e048d5d5cea40b7461fbc0714edee3a0f16` |
| LMMS/lmms | `518a7e8ef525a276ba9702df87c613ea0ff25c47` |
| rncbc/qtractor | `006a4ac4f4377fdf53115e55b06463062037f685` |
| muse-sequencer/muse | `732c5543e3dd89ffb5c1387436906ce1bf87fcce` |
| zrythm/zrythm | `a6ac5800f29dbed14d84bc39479665e286b42c8a` |
| helio-fm/helio-sequencer | `3fd7a39bd60282003ffaed900eb648ac3fcdb710` |
| ossia/score | `8591aa0a34d532158ba0b4b7f7a42ec9613fb934` |
| audacity/audacity | `b51ae6bddaffd719c60686ff121e10e8f0850a0a` |

## Источники

Первичные источники для каждой строки указаны в её ссылках: официальные product/manual pages и репозитории авторов. Они относятся к названным возможностям и слоям; не подтверждают неизвестные internals. Архитектурные API и лицензии собраны отдельно в [общем реестре](sources.md).

[^1]: Ableton, [Introducing Extensions SDK](https://www.ableton.com/en/blog/introducing-extensions-sdk/), 2 июня 2026; доступность указана как public beta, не general availability для всех редакций.
[^2]: Bitwig, [Modern Foundations](https://www.bitwig.com/modern-foundations/), [VST Plug-in Handling and Options](https://www.bitwig.com/userguide/latest/vst_plug-in_handling_and_options/), живые страницы.
[^3]: Zrythm, [v2 alpha announcement](https://forum.zrythm.org/t/zrythm-v2-0-0-alpha-1-released/596), 1 июня 2026; Audacity, [current CMakeLists](https://github.com/audacity/audacity/blob/master/CMakeLists.txt).
[^4]: Bitwig, [DAWproject](https://github.com/bitwig/dawproject), specification/repository.
