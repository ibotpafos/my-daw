#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/my-daw-transforms.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
CORE_SOURCES=()
for FILE in Model Timing Edits Transforms Harmony NoteIndex; do
  CORE_SOURCES+=("$ROOT/apps/macos/PianoRoll/PianoRoll$FILE.swift")
done
swiftc -swift-version 6 -warnings-as-errors -O "${CORE_SOURCES[@]}" \
  "$ROOT/tests/piano_roll/TransformCoreTests.swift" -o "$BUILD/transform-tests"
"$BUILD/transform-tests"
swiftc -swift-version 6 -warnings-as-errors -O "${CORE_SOURCES[@]}" \
  "$ROOT/tests/piano_roll/HarmonyCoreTests.swift" -o "$BUILD/harmony-tests"
"$BUILD/harmony-tests"
swiftc -swift-version 6 -warnings-as-errors -O "${CORE_SOURCES[@]}" \
  "$ROOT/apps/macos/PianoRoll/PianoRollProState.swift" \
  "$ROOT/tests/piano_roll/ProStateCoreTests.swift" -o "$BUILD/pro-state-tests"
"$BUILD/pro-state-tests"
swiftc -swift-version 6 -warnings-as-errors -O "${CORE_SOURCES[@]}" \
  "$ROOT/apps/macos/PianoRoll/PianoRollProState.swift" \
  "$ROOT/apps/macos/PianoRoll/PianoRollProPreview.swift" \
  "$ROOT/tests/piano_roll/PreviewCoreTests.swift" -o "$BUILD/preview-tests"
"$BUILD/preview-tests"
