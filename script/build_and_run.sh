#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-run}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_BUNDLE="$ROOT_DIR/build/My DAW.app"
APP_BINARY="$APP_BUNDLE/Contents/MacOS/My DAW"
APP_NAME="My DAW"
BUNDLE_ID="dev.mydaw.prototype"

stop_running_app() {
  local process_ids attempt
  process_ids="$(pgrep -f "^$APP_BINARY$" || true)"
  if [[ -z "$process_ids" ]]; then
    return
  fi

  # The command is anchored to this bundle's executable, so a Run action does
  # not terminate another My DAW build slot or any unrelated process.
  kill $process_ids
  for attempt in {1..50}; do
    if ! pgrep -f "^$APP_BINARY$" >/dev/null; then
      return
    fi
    sleep 0.1
  done

  printf 'Timed out waiting for the previous %s process to exit.\n' "$APP_NAME" >&2
  return 1
}

case "$MODE" in
  run|--debug|debug|--logs|logs|--telemetry|telemetry|--verify|verify)
    ;;
  *)
    echo "usage: $0 [run|--debug|--logs|--telemetry|--verify]" >&2
    exit 2
    ;;
esac

stop_running_app
"$ROOT_DIR/scripts/build-macos.sh"

launch_app() {
  /usr/bin/open -n "$APP_BUNDLE"
}

case "$MODE" in
  run)
    launch_app
    ;;
  --debug|debug)
    lldb -- "$APP_BINARY"
    ;;
  --logs|logs)
    launch_app
    /usr/bin/log stream --info --style compact --predicate 'process == "My DAW"'
    ;;
  --telemetry|telemetry)
    launch_app
    /usr/bin/log stream --info --style compact --predicate "subsystem == \"$BUNDLE_ID\""
    ;;
  --verify|verify)
    launch_app
    sleep 1
    pgrep -x "$APP_NAME" >/dev/null
    ;;
esac
