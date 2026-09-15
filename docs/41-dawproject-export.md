# DAWproject export

Версия 1.15.0 реализует B-016 в code scope. Команда «Экспорт DAWproject…» берёт неизменяемый снимок текущей revision и создаёт один `.dawproject` файл в фоновом job. Редактирование проекта во время экспорта не меняет уже зафиксированное содержимое.

## Контейнер и совместимость

Экспорт следует DAWproject 1.0: ZIP содержит UTF-8 `project.xml` и `metadata.xml`. Реализация опирается на официальный schema revision `ee4dcdde75940f30e14e55401a26955a58b8322b`, закреплённый в `dependencies.lock.json`.[^1] Встроенный ZIP32 writer не запускает внешние процессы, проверяет относительные пути и дубликаты, записывает CRC32 и публикует готовый файл через temporary file, `fsync` и rename.

Структура проекта переносится следующим образом:

- audio tracks, buses и master становятся Track/Channel с сохранением main routing, pre/post sends, mute, solo, static gain и pan;
- Regions становятся Clips в секундной шкале, включая source offset, duration и linear fades;
- используемые источники comp materialize в `audio/*.wav` как 48 kHz stereo float32; неиспользуемые take lanes не раздувают пакет;
- track volume/pan, bus gain и master gain automation становятся Points с прямыми ссылками на параметры;
- Audio Unit и VST3 на track, bus и master становятся device соответствующего Channel; state находится внутри owner-qualified `plugins/track-*`, `plugins/bus-*` или `plugins/master-*` paths, а VST3 использует стандартный `.vstpreset` container;
- format v14 plug-in lanes становятся `RealParameter` внутри device и sibling `Points` в Arrangement с `Target` IDREF; normalized value сохраняется, а loss report отмечает зависимость от mapping vendor parameter ID;
- tempo берётся из текущего UI, текущая time signature — 4/4.

## Loss report

`loss-report.json` находится в корне пакета. Он отделяет сохранённые данные от преобразований: flatten take-lane provenance, materialization внутреннего аудио, переносимость Audio Unit между hosts, повторное вычисление latency, а также исключение window layout, selections, transport/loop и Undo history. После экспорта интерфейс показывает число warnings и сообщает, где лежит подробный отчёт.

Это не обещание безупречного round-trip. Проверка импорта в Bitwig Studio, Studio One, Cubase и других hosts остаётся отдельной compatibility matrix. В этом срезе выполнена compile/build проверка; QA, запуск приложения, физическое прослушивание и импорт в стороннюю DAW не выполнялись по текущему режиму разработки.

[^1]: Bitwig, [DAWproject README](https://github.com/bitwig/dawproject/blob/main/README.md), [Project XSD](https://github.com/bitwig/dawproject/blob/main/Project.xsd) и [MetaData XSD](https://github.com/bitwig/dawproject/blob/main/MetaData.xsd).
