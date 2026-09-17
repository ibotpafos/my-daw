#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/my-daw-piano-bridge.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CORE="${DAW_CORE_BUILD:-build/debug}"
if [[ ! -f "$CORE/libdaw_core.a" ]]; then
  printf 'Build daw_core with the Debug preset before running this test.\n' >&2
  exit 1
fi
SOURCES=()
for FILE in Model Timing Edits Transforms Harmony NoteIndex ProState Bridge; do
  SOURCES+=("apps/macos/PianoRoll/PianoRoll$FILE.swift")
done
LINK=("$CORE/libdaw_core.a" -lsqlite3)
if [[ "$(uname -s)" == Darwin ]]; then
  LINK+=("$CORE/libdaw_au_scanner.a" -Xlinker -lc++ -framework Foundation
    -framework AudioToolbox -framework CoreAudio -framework CoreMIDI -framework AVFoundation)
else
  LINK+=(-Xlinker -lstdc++ -Xlinker -lpthread -Xlinker -lm -Xlinker -lsamplerate)
fi
swiftc -swift-version 6 -warnings-as-errors -O -import-objc-header engine/bridge/daw.h \
  "${SOURCES[@]}" tests/piano_roll/BridgeIntegrationTests.swift "${LINK[@]}" -o "$BUILD/bridge-tests"
"$BUILD/bridge-tests"
