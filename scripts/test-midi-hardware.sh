#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ "$(uname -s)" != "Darwin" ]; then
  printf 'CoreMIDI hardware probe is macOS-only.\n' >&2
  exit 2
fi

OUT="${TMPDIR:-/tmp}/my-daw-midi-input-probe"
xcrun clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  tools/midi_input_probe.cpp -framework CoreMIDI -framework CoreFoundation -o "$OUT"

if [ "$#" -eq 0 ]; then
  "$OUT" --list
  printf '\nChoose a unique-id from the list, then run:\n  %s <unique-id> [seconds]\n' "$0"
  exit 0
fi

SOURCE_ID="$1"
SECONDS_TO_LISTEN="${2:-8}"
exec "$OUT" --source "$SOURCE_ID" --seconds "$SECONDS_TO_LISTEN"
