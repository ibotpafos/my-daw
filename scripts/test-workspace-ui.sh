#!/usr/bin/env bash
# Offscreen test executable, not the application's normal launch path.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/workspace-ui
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/WorkspaceLayout.swift tests/workspace_layout_tests.swift \
  -o build/workspace-ui/layout-tests
build/workspace-ui/layout-tests
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/TimelineRangeEditing.swift tests/timeline_range_tests.swift \
  -o build/workspace-ui/range-tests
build/workspace-ui/range-tests
sources=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  sources+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -swift-version 6 -D DAW_WORKSPACE_TESTS \
  -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header engine/bridge/daw.h "${sources[@]}" tests/workspace_ui_tests.swift tests/workspace_timeline_tests.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers \
  -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/workspace-ui/workspace-tests
build/workspace-ui/workspace-tests
