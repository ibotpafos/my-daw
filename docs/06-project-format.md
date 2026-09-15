# Формат проекта и сохранение

## Контейнер

Рабочее расширение `.mydaw` — директория, которую macOS app позднее регистрирует как document package. Не ZIP во время редактирования.

```text
Song.mydaw/
  manifest.json
  session.sqlite
  Media/                 # неизменяемые завершённые файлы
  PluginStates/          # непрозрачные состояния по content hash
  ModuleStates/          # versioned opaque state
  Recovery/              # незавершённые записи и sidecars
  Cache/                 # peaks; можно удалить
  Backups/               # ограниченное число snapshots
```

Manifest хранит идентификатор, format version и имя базы. Актуальные tempo, revision и список объектов — в SQLite. Manifest не дублирует часто меняющееся состояние и не требует атомарной транзакции одновременно с каждым SQL commit.

## Модель

Project → Tracks → Clips; Clips ссылаются на Assets, а не владеют копиями samples. PluginInstance относится к Track/Bus и хранит идентификатор формата/производителя/компонента и ссылку на state blob. AutomationLane относится к стабильному parameter ID. Takes и CompSegments добавляются миграцией S1.

В [SQL-схеме](../specs/project-v0.sql) определён минимальный исторический S0: project, tracks, assets, clips, command_log. Текущий runtime format v15 хранит ordered track/bus/master insert chains в `channel_plugins`, включая `hosting_mode` (`InProcess` или `OutOfProcess`), а normalized lanes их параметров — в `plugin_parameter_automation`. Owner kind и owner ID задают цепь, plugin/parameter ID — стабильную automation target, position — порядок. Reader v1–v14 читает совместимо и назначает отсутствующему `hosting_mode` значение `InProcess`; writer всегда создаёт v15. Связи и общие лимиты проверяет domain validator.

Asset ID не равен пути. Относительный путь нормализуется; `..`, абсолютные пути и symlink escape за package отклоняются при импорте непроверенного проекта. В первой версии импорт всегда копирует медиа; external references будут отдельной возможностью с relink.

## Commit медиа и состояния

1. Записать новый файл во временное имя на том же filesystem.
2. Закрыть, проверить формат/длину, flush по durability policy, вычислить hash.
3. Atomic rename в Media или PluginStates.
4. В одной SQL-транзакции добавить ссылки и изменить revision/history.

Crash между 3 и 4 создаёт orphan, но не ссылку на недописанное медиа. Orphan не удалять сразу: он может относиться к recoverable recording. Garbage collection учитывает history и backups, выполняется отдельной явной операцией.

SQLite transactions не делают произвольные соседние WAV-файлы атомарными. Сохранение аудиофайла и ссылка на него требуют описанного порядка.

## SQLite policy

Для активного локального проекта: один writer, WAL, `foreign_keys=ON`, исходная durability policy `synchronous=FULL`; latency commit измеряется вне RT. WAL sidecars — часть актуального состояния базы. Обычное копирование одного `session.sqlite` во время работы не считается backup.[^1]

Save As использует SQLite Backup API для согласованного snapshot и копирует только referenced immutable media, затем проверяет целостность и финализирует пакет.[^2] Пока идёт копирование, snapshot revision не меняется. Отмена не заменяет существующий destination. Существующий проект не перезаписывается без выбора пользователя.

Открытые проекты на сетевой FS и в синхронизируемых облаком директориях в S0/S1 не поддержаны. Предлагать локальную рабочую копию и closed-project export. Не обещать надёжность WAL поверх произвольного NAS/cloud sync.

## Undo и recovery

`command_log`: уникальный command ID, revision, forward/inverse payload и timestamp. Повтор того же ID и того же payload возвращает прежний результат; тот же ID с другим payload — conflict. Linear Undo/Redo в S0, без ветвления истории.

Две разные гарантии: завершённая SQL-транзакция восстанавливается по SQLite; ещё записываемое аудио восстанавливается по chunk/sidecar протоколу. Цель потери последнего незафиксированного участка — не более 1 секунды при process crash, но она должна быть доказана kill-тестом. Power loss и неисправный накопитель — отдельные испытания, не покрытые обычным kill.

При recovery показывать что восстановлено, сколько кадров доступно, какой диапазон сомнителен. Открывать исходный повреждённый пакет только для чтения и создавать recovery copy.

## Миграции

`formatVersion` — integer schema generation; `PRAGMA user_version` сверяется с ней. Миграция на копии, integrity check, затем переключение. Не переписывать более новый неизвестный формат. Незнакомые optional module state сохраняются непрозрачно. Неизвестная required feature блокирует редактирование до поддержанного reader.

## Обмен

S1 экспортирует stereo mix и отдельные stems с общим frame origin; README экспорта содержит sample rate, tempo, начало и применённые эффекты. Следующий формат — DAWproject, открытый XML/ZIP exchange format. Авторы прямо отделяют его от native DAW file format; поэтому он дополняет SQLite package, а не заменяет внутреннее хранилище.[^3] При экспорте нужен loss report для непереносимых операций и отсутствующих plugins.

[^1]: SQLite, [Write-Ahead Logging](https://sqlite.org/wal.html).
[^2]: SQLite, [Online Backup API](https://sqlite.org/backup.html).
[^3]: Bitwig, [DAWproject specification](https://github.com/bitwig/dawproject).
