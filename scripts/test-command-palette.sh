#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/my-daw-command-palette.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

if [ "$(uname -s)" = Darwin ]; then
  SWIFTC=(xcrun swiftc -swift-version 6 -warnings-as-errors -target "$(uname -m)-apple-macosx14.0" -sdk "$(xcrun --show-sdk-path)")
else
  SWIFTC=(swiftc -swift-version 6 -warnings-as-errors)
fi
"${SWIFTC[@]}" "$ROOT/apps/macos/CommandPaletteSearch.swift" \
  "$ROOT/tests/macos_command_palette_search_tests.swift" -o "$WORK/model-tests"
"$WORK/model-tests"

if [ "$(uname -s)" = Darwin ]; then
  "${SWIFTC[@]}" "$ROOT/apps/macos/CommandPaletteSearch.swift" \
    "$ROOT/apps/macos/CommandPalette.swift" "$ROOT/apps/macos/DAWWindow.swift" \
    "$ROOT/tests/macos_command_palette_appkit_tests.swift" \
    -framework AppKit -o "$WORK/appkit-tests"
  "$WORK/appkit-tests"
else
  printf 'AppKit integration not run: requires macOS.\n'
fi
