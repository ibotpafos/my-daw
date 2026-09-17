#!/usr/bin/env bash
# Offscreen integration harness. Normal app launch remains script/build_and_run.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
bash scripts/test-mix-export-policy.sh
mkdir -p build/mix-export-ui
sources=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  sources+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -swift-version 6 -warnings-as-errors -D DAW_MIX_EXPORT_TESTS \
  -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header apps/macos/DAWBridge.h "${sources[@]}" tests/mix_export_ui_tests.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers \
  -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/mix-export-ui/export-tests
python3 - <<'PY'
import subprocess
import sys
try:
    result = subprocess.run(["build/mix-export-ui/export-tests"], timeout=120, check=False)
except subprocess.TimeoutExpired:
    print("Mix export integration timed out; inspect dialogs and jobs.", file=sys.stderr)
    sys.exit(124)
sys.exit(result.returncode if result.returncode >= 0 else 128 - result.returncode)
PY
