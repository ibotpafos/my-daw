#!/usr/bin/env bash
# Offscreen test executable, not the application's normal launch path.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/workspace-ui
xcrun clang -std=c11 -Wall -Wextra -Werror tests/macos_bridge_tests.c -o build/workspace-ui/bridge-tests
build/workspace-ui/bridge-tests
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/WorkspaceLayout.swift tests/workspace_layout_tests.swift \
  -o build/workspace-ui/layout-tests
build/workspace-ui/layout-tests
xcrun swiftc -swift-version 6 -warnings-as-errors -parse-as-library \
  apps/macos/Workspace/WorkspaceLayout.swift apps/macos/Workspace/WorkspaceScreen.swift \
  tests/workspace_screen_model_tests.swift -o build/workspace-ui/screen-model-tests
build/workspace-ui/screen-model-tests
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/TimelineRangeEditing.swift tests/timeline_range_tests.swift \
  -o build/workspace-ui/range-tests
build/workspace-ui/range-tests
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/RackParameter.swift tests/rack_parameter_tests.swift \
  -o build/workspace-ui/parameter-tests
build/workspace-ui/parameter-tests
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/LibraryCatalogModel.swift tests/library_catalog_tests.swift \
  -o build/workspace-ui/library-tests
build/workspace-ui/library-tests
xcrun swiftc -swift-version 6 -parse-as-library \
  apps/macos/Workspace/LibraryFolderScanner.swift tests/library_folder_scanner_tests.swift \
  -o build/workspace-ui/folder-scanner-tests
build/workspace-ui/folder-scanner-tests
xcrun swiftc -swift-version 6 -parse-as-library apps/macos/Workspace/ArrangementOverviewGeometry.swift tests/arrangement_overview_tests.swift -o build/workspace-ui/overview-model-tests
build/workspace-ui/overview-model-tests
xcrun swiftc -swift-version 6 -warnings-as-errors -parse-as-library apps/macos/AudioDevicePreferences.swift tests/audio_device_preferences_tests.swift -o build/workspace-ui/audio-preferences-tests
build/workspace-ui/audio-preferences-tests
xcrun swiftc -swift-version 6 -warnings-as-errors -parse-as-library \
  apps/macos/Workspace/TimelineRangeEditing.swift apps/macos/Workspace/RecordCompSelection.swift \
  tests/record_comp_selection_tests.swift -o build/workspace-ui/comp-model-tests
build/workspace-ui/comp-model-tests
sources=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  sources+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -swift-version 6 -D DAW_WORKSPACE_TESTS \
  -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header apps/macos/DAWBridge.h "${sources[@]}" tests/workspace_ui_tests.swift tests/workspace_timeline_tests.swift tests/workspace_signal_chain_tests.swift tests/workspace_parameter_tests.swift tests/workspace_library_tests.swift tests/workspace_folder_tests.swift tests/workspace_mixer_tests.swift tests/workspace_screen_ui_tests.swift tests/workspace_overview_tests.swift tests/workspace_audio_device_tests.swift tests/workspace_audio_hardware_tests.swift tests/record_comp_workspace_tests.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers \
  -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/workspace-ui/workspace-tests
# An unexpected modal dialog or deadlock must fail CI, not occupy a Mac runner
# until the entire job limit. Preserve the child failure and collect its logs.
python3 - <<'PY'
import subprocess
import sys

try:
    result = subprocess.run(["build/workspace-ui/workspace-tests"], timeout=120, check=False)
except subprocess.TimeoutExpired:
    print("Native workspace tests timed out after 120 seconds; check for a modal dialog or deadlock.", file=sys.stderr)
    sys.exit(124)
sys.exit(result.returncode if result.returncode >= 0 else 128 - result.returncode)
PY
