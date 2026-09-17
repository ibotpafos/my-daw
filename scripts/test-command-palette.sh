#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/my-daw-command-palette.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

export MACOSX_DEPLOYMENT_TARGET=14.0
xcrun swiftc -swift-version 6 \
  -target arm64-apple-macosx14.0 \
  -sdk "$(xcrun --show-sdk-path)" \
  "$ROOT/apps/macos/CommandPaletteSearch.swift" \
  "$ROOT/tests/macos_command_palette_search_tests.swift" \
  -o "$TMP_DIR/command-palette-search-tests"

"$TMP_DIR/command-palette-search-tests"
