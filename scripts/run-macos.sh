#!/bin/bash
# Единственная точка «остановить → собрать → запустить» для macOS-приложения.
# Смысл в том, чтобы не пересобирать вручную и не запускать бинарь в обход
# бандла: GUI-приложение вне .app не получает ни Dock-активации, ни bundle id,
# ни корректных permission-промптов.
#
#   ./scripts/run-macos.sh              kill + build + open
#   ./scripts/run-macos.sh --verify     то же, затем проверить, что процесс жив
#   ./scripts/run-macos.sh --logs       запустить и стримить логг процесса
#   ./scripts/run-macos.sh --telemetry  запустить и стримить unified log приложения
#   ./scripts/run-macos.sh --debug      запустить бинарь под lldb
#   ./scripts/run-macos.sh --kill       только остановить запущенные копии
#
# Флаг --logs/--telemetry блокируют терминал (это стриминг), Ctrl-C их закрывает.
set -euo pipefail
DAW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DAW_ROOT"
APP_NAME="My DAW"
BUNDLE_ID="dev.mydaw.prototype"
MODE="${1:-run}"

kill_running() {
  # build-macos.sh сам уходит в слоты Next/Candidate, когда копия запущена;
  # для чистого запуска гасим все три, иначе экранов станет троица.
  for slot in "My DAW" "My DAW Next" "My DAW Candidate"; do
    pkill -x "$slot" 2>/dev/null || true
  done
  pkill -f "build/My DAW.*\\.app/Contents/MacOS/My DAW" 2>/dev/null || true
}

case "$MODE" in
  --kill|kill)
    kill_running
    printf 'Запущенные копии остановлены.\n'
    exit 0
    ;;
esac

kill_running
BUILD_LOG="$(mktemp)"
# Собираем, показывая вывод, и забираем путь к актуальному бандлу из
# финальной строки «Built <path>» — это единственный надёжный способ узнать,
# какой из трёх слотов использовался.
if ./scripts/build-macos.sh 2>&1 | tee "$BUILD_LOG" | grep -vE '^Built ' >&2; then : ; else
  rm -f "$BUILD_LOG"
  printf 'Сборка не прошла — запуск отменён. Первые строки ошибки выше; см. также ./scripts/run-macos.sh --logs после успешной сборки.\n' >&2
  exit 1
fi
APP_PATH="$(sed -n 's/^Built //p' "$BUILD_LOG" | tail -1)"
rm -f "$BUILD_LOG"
if [ -z "$APP_PATH" ] || [ ! -d "$APP_PATH" ]; then
  printf 'Не удалось определить путь к собранному бандлу (см. вывод build-macos.sh).\n' >&2
  exit 1
fi
BIN="$APP_PATH/Contents/MacOS/$APP_NAME"

case "$MODE" in
  run)
    /usr/bin/open -n "$APP_PATH"
    printf 'Запущено: %s\n' "$APP_PATH"
    ;;
  --verify|verify)
    /usr/bin/open -n "$APP_PATH"
    sleep 2
    if pgrep -f "^$APP_PATH/Contents/MacOS/$APP_NAME$" >/dev/null; then
      printf 'VERIFY OK: процесс «%s» жив через 2 с после запуска (%s).\n' "$APP_NAME" "$APP_PATH"
    else
      printf 'VERIFY FAIL: процесс не запущен или умер сразу после старта.\n' >&2
      printf 'Похоже на ранний падёж: загляни в ~/Library/Logs/DiagnosticReports и в ./scripts/run-macos.sh --logs.\n' >&2
      exit 1
    fi
    ;;
  --logs|logs)
    /usr/bin/open -n "$APP_PATH"
    exec /usr/bin/log stream --info --style compact --predicate "process == \"$APP_NAME\""
    ;;
  --telemetry|telemetry)
    /usr/bin/open -n "$APP_PATH"
    exec /usr/bin/log stream --info --style compact --predicate "subsystem == \"$BUNDLE_ID\""
    ;;
  --debug|debug)
    exec lldb -- "$BIN"
    ;;
  *)
    printf 'usage: %s [run|--verify|--logs|--telemetry|--debug|--kill]\n' "$0" >&2
    exit 2
    ;;
esac
