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
"${SWIFTC[@]}" "$ROOT/apps/macos/CommandPaletteSearch.swift" "$ROOT/apps/macos/CommandAliases.swift" \
  "$ROOT/tests/macos_command_palette_search_tests.swift" -o "$WORK/model-tests"
"$WORK/model-tests"
"${SWIFTC[@]}" "$ROOT/apps/macos/CommandPaletteSearch.swift" "$ROOT/apps/macos/CommandAliases.swift" \
  "$ROOT/tests/command_alias_tests.swift" -o "$WORK/alias-tests"
"$WORK/alias-tests"

if [ "$(uname -s)" = Darwin ]; then
  # A raw command-line executable can show a window without establishing the
  # application's real key/main responder chain. Exercise a proper .app launch.
  APP="$WORK/Command Palette Tests.app"
  mkdir -p "$APP/Contents/MacOS"
  "${SWIFTC[@]}" "$ROOT/apps/macos/CommandPaletteSearch.swift" "$ROOT/apps/macos/CommandAliases.swift" \
    "$ROOT/apps/macos/CommandPalette.swift" "$ROOT/apps/macos/CommandAliasEditor.swift" "$ROOT/apps/macos/DAWWindow.swift" \
    "$ROOT/tests/macos_command_palette_appkit_tests.swift" \
    -framework AppKit -o "$APP/Contents/MacOS/PaletteTests"
  python3 - "$APP/Contents/Info.plist" <<'PY'
import plistlib, sys
with open(sys.argv[1], "wb") as handle:
    plistlib.dump({
        "CFBundleIdentifier": "app.mydaw.command-palette-tests",
        "CFBundleExecutable": "PaletteTests",
        "CFBundleName": "Command Palette Tests",
        "CFBundlePackageType": "APPL",
        "CFBundleVersion": "1",
        "LSMinimumSystemVersion": "14.0",
        "NSPrincipalClass": "NSApplication",
    }, handle)
PY
  codesign --force --sign - "$APP"
  touch "$WORK/appkit.log" "$WORK/appkit.err"
  OPEN_STATUS=0
  /usr/bin/open -n -W --stdout "$WORK/appkit.log" --stderr "$WORK/appkit.err" \
    "$APP" --args "$WORK/appkit-result" "${DAW_PALETTE_EVIDENCE_DIR:-}" || OPEN_STATUS=$?
  cat "$WORK/appkit.log" "$WORK/appkit.err"
  # open's exit status is NOT the child's test status. A success marker written
  # only after every assertion/cleanup is mandatory; crashes/timeouts fail.
  test "$OPEN_STATUS" -eq 0 && test -f "$WORK/appkit-result" && grep -qx PASS "$WORK/appkit-result"
else
  printf 'AppKit integration not run: requires macOS.\n'
fi
