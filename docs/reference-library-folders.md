# Библиотека: запоминание папки и фоновое сканирование

Workspace хранит одну явно выбранную папку WAV/AIFF. Выбор через системный
NSOpenPanel работает как sheet; сканирование, создание и разрешение bookmark
выполняются в utility task, сериализованной отдельным actor. UI и session/C ABI
остаются на MainActor. Новых библиотек, DSP или формата проекта нет.

## Поведение

- Кнопка папки выбирает и заменяет текущий корень; вложенные каталоги читаются
  через FileManager.DirectoryEnumerator. Никакого сканирования домашней папки.
- В библиотеке показаны имя папки, прогресс, rescan/cancel/forget. Забыть папку
  не удаляет аудиофайлы, дорожки и избранное. Фокус этих кнопок не удаляет клипы.
- Новый snapshot публикуется целиком; до этого работает предыдущий каталог.
  Cancel и поколение запроса не дают запоздавшему результату заменить новый.
- Во время записи/MIDI capture или импорта готовый результат ждёт. Старый
  security-scope сохраняется, пока импорт использует прежние файлы. Публикация
  останавливает audition, заменяет карту команд и только затем строки таблицы.
- Избранное и выбранный ресурс сохраняют существующую идентичность по пути.
  Перенос папки может изменить путь и не мигрирует автоматически favorite keys.
- Bookmark записывается локально в versioned UserDefaults после успешного scan.
  При запуске он разрешается без UI и без подключения отсутствующего тома.
  Устаревший bookmark обновляется. Ошибка разрешения не заменяется сырым путём
  и не вызывает сканирование другой папки: показывается запрос выбрать заново.

## Границы

До 10 000 файлов, 100 000 просмотренных записей и 15 секунд между проверками
метаданных. Достижение лимита явно помечено; это неполный каталог. Скрытые
файлы, содержимое packages, symlinks исключаются. Частичные ошибки считаются;
ошибка корня не выдаётся за успешный пустой каталог. Аудиосодержимое не читается
сканером; формат и исправность файла проверяет прежний importer/preview.

Cancel кооперативный: системный I/O или bookmark resolver, зависший внутри OS,
нельзя прервать посередине. Одна actor-очередь ограничивает одновременный I/O;
новый запрос ждёт выхода старого из OS. Нет file watcher, multi-root и индекса
на диске. Публикация и фильтрация 10 000 строк всё ещё выполняются на UI actor.

Предпочитается read-only security-scoped bookmark; текущая development-сборка
не sandboxed, поэтому возможен обычный bookmark. Обычный bookmark сохраняет
местоположение, но не предоставляет постоянного разрешения. Повторная выдача
прав/смена подписи/недоступный внешний диск могут потребовать выбора папки.
Существующие разрешения macOS не обходятся; настройки приватности не меняются.

Активная LibraryBrowserView использует координатор, подключённый при первой публикации каталога.
Прежний onAddFolder callback сохранён только как fallback для не настроенного
отдельного browser host; новое окно не вызывает старый синхронный enumerator.

## Проверка и первоисточники

`tests/library_folder_scanner_tests.swift` работает с настоящей временной
файловой системой; `tests/workspace_folder_tests.swift` проверяет production
координатор и карту команд, cancel/latest-wins, deferred publication, rescan,
реальное bookmark round-trip, повреждённые preferences, focus, UI snapshots.
`bash scripts/test-workspace-ui.sh` запускает обе группы на macOS.

Использованы системные API, без собственной реализации traversal/bookmarks:
[FileManager enumerator](https://developer.apple.com/documentation/foundation/filemanager/enumerator(at:includingpropertiesforkeys:options:errorhandler:)),
[URL bookmarks](https://developer.apple.com/documentation/foundation/nsurl/bookmarkdata(options:includingresourcevaluesforkeys:relativeto:)),
[без UI](https://developer.apple.com/documentation/foundation/nsurl/bookmarkresolutionoptions/withoutui),
[баланс scope](https://developer.apple.com/documentation/foundation/nsurl/stopaccessingsecurityscopedresource()).
