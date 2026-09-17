#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export MACOSX_DEPLOYMENT_TARGET=14.0
SDK="$(xcrun --show-sdk-path)"
APP_SOURCES=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  [[ -f "$source" ]] || { printf 'Missing Swift source: %s\n' "$source" >&2; exit 1; }
  APP_SOURCES+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -typecheck -swift-version 6 -warnings-as-errors \
  -target arm64-apple-macosx14.0 -sdk "$SDK" \
  -import-objc-header apps/macos/DAWBridge.h \
  "${APP_SOURCES[@]}"
printf 'AppKit Swift typecheck passed.\n'
