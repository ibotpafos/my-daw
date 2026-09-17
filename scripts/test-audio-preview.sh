#!/bin/bash
set -euo pipefail

DAW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_BINARY="$(mktemp -t my-daw-audio-preview.XXXXXX)"
trap 'rm -f "$TEST_BINARY"' EXIT

xcrun swiftc -swift-version 6 -target arm64-apple-macosx14.0 \
  -sdk "$(xcrun --show-sdk-path)" \
  "$DAW_ROOT/apps/macos/AudioPreviewController.swift" \
  "$DAW_ROOT/tests/macos_audio_preview_tests.swift" \
  -framework AVFoundation \
  -o "$TEST_BINARY"
"$TEST_BINARY"
