#!/usr/bin/env bash
# Dedicated native recording harness. Hardware is link-time fixture ONLY;
# the shipped app and existing workspace tests still use the actual HAL.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/recording-ui
sources=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  sources+=("$source")
done < apps/macos/sources.txt
xcrun clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -Iengine \
  -target arm64-apple-macosx14.0 -c tests/support/recording_device_fixture.cpp \
  -o build/recording-ui/device-fixture.o
xcrun swiftc -swift-version 6 -warnings-as-errors -D DAW_WORKSPACE_TESTS \
  -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header tests/RecordingTestBridge.h "${sources[@]}" tests/recording_ui_tests.swift \
  build/recording-ui/device-fixture.o build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers \
  -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/recording-ui/recording-tests
python3 - <<'PY'
import subprocess
import sys
try:
    result = subprocess.run(["build/recording-ui/recording-tests"], timeout=120, check=False)
except subprocess.TimeoutExpired:
    print("Recording UI tests timed out: check for modal dialogs or an I/O deadlock.", file=sys.stderr)
    sys.exit(124)
sys.exit(result.returncode if result.returncode >= 0 else 128 - result.returncode)
PY
