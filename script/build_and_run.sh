#!/usr/bin/env bash
# Canonical local entry point. Only this checkout's primary bundle is controlled.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_BUNDLE="$ROOT_DIR/build/My DAW.app"
APP_BINARY="$APP_BUNDLE/Contents/MacOS/My DAW"
MODE="${1:-run}"
usage() { printf 'usage: %s [run|--debug|--logs|--telemetry|--verify|--kill]\n' "$0"; }
if [[ "$#" -gt 1 ]]; then usage >&2; exit 2; fi
case "$MODE" in
  --help|-h) usage; exit 0 ;;
  run|--debug|debug|--logs|logs|--telemetry|telemetry|--verify|verify|--kill|kill) ;;
  *) usage >&2; exit 2 ;;
esac
if [[ "$(uname -s)" != Darwin ]]; then
  printf 'The native application requires macOS. Use CMake presets for portable core tests.\n' >&2
  exit 2
fi
# AppKit termination honors the app's save/cancel handling. No pkill, SIGKILL,
# matching by application name, or changing another checkout's running bundle.
xcrun swift -swift-version 6 "$ROOT_DIR/scripts/macos-app-control.swift" --quit "$APP_BUNDLE"
case "$MODE" in --kill|kill) exit 0 ;; esac
"$ROOT_DIR/scripts/build-macos.sh"
if [[ ! -x "$APP_BINARY" ]]; then
  printf 'The expected application executable is missing: %s\n' "$APP_BINARY" >&2
  exit 1
fi
case "$MODE" in
  --debug|debug) exec lldb -- "$APP_BINARY" ;;
esac
/usr/bin/open -n "$APP_BUNDLE"
case "$MODE" in
  --verify|verify)
    exec xcrun swift -swift-version 6 "$ROOT_DIR/scripts/macos-app-control.swift" --verify "$APP_BUNDLE"
    ;;
  --logs|logs)
    exec /usr/bin/log stream --info --style compact --predicate 'process == "My DAW"'
    ;;
  --telemetry|telemetry)
    exec /usr/bin/log stream --info --style compact --predicate 'subsystem == "dev.mydaw.prototype"'
    ;;
esac
printf 'Launched %s\n' "$APP_BUNDLE"
