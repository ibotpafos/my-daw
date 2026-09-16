#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/tools/au_music_device_probe"
SRC="$ROOT/tools/au_music_device_probe.cpp"

mkdir -p "$(dirname "$BIN")"

xcrun clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  "$SRC" \
  -framework AudioToolbox -framework CoreAudio -framework CoreFoundation \
  -o "$BIN"

if [ "$#" -eq 0 ]; then
  "$BIN" --list
  exit $?
fi

if [ "$#" -eq 2 ]; then
  "$BIN" --probe "$1" "$2"
  exit $?
fi

if [ "$#" -eq 3 ]; then
  "$BIN" --probe "$1" "$2" "$3"
  exit $?
fi

cat >&2 <<'EOF'
Usage:
  ./scripts/test-au-music-device.sh
  ./scripts/test-au-music-device.sh <subtype-hex> <manufacturer-hex> [seconds]

The first form lists installed AU MusicDevice components.
The second initializes one instrument, sends middle-C Note On/Off and renders it
offline at 48 kHz stereo. A silent render is reported separately from an
initialization or render failure.
EOF
exit 2
