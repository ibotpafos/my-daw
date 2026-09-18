#!/usr/bin/env bash
# Offscreen integration harness. Normal app launch remains script/build_and_run.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -m unittest discover -s tests -p test_native_test_runner.py -v
bash scripts/test-mix-export-policy.sh
mkdir -p build/mix-export-ui
sources=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  sources+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -g -swift-version 6 -warnings-as-errors -D DAW_MIX_EXPORT_TESTS \
  -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header apps/macos/DAWBridge.h "${sources[@]}" tests/mix_export_ui_tests.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers \
  -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/mix-export-ui/export-tests
# Preserve symbols for the exact binary that ran, not a later reconstruction.
xcrun dsymutil build/mix-export-ui/export-tests -o build/mix-export-ui/export-tests.dSYM
# A stale proof from a previous invocation must never certify this run.
rm -f build/mix-export-ui/proof.json
python3 scripts/native_test_runner.py --timeout 180 \
  --evidence build/mix-export-ui/diagnostics -- build/mix-export-ui/export-tests
