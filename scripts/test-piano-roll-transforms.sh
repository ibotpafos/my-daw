#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/my-daw-transforms.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
SOURCES=()
for FILE in Model Timing Edits Transforms Harmony; do
  SOURCES+=("$ROOT/apps/macos/PianoRoll/PianoRoll$FILE.swift")
done
swiftc -swift-version 6 -warnings-as-errors -O "${SOURCES[@]}" \
  "$ROOT/tests/piano_roll/TransformCoreTests.swift" -o "$BUILD/transform-tests"
"$BUILD/transform-tests"
swiftc -swift-version 6 -warnings-as-errors -O "${SOURCES[@]}" \
  "$ROOT/tests/piano_roll/HarmonyCoreTests.swift" -o "$BUILD/harmony-tests"
"$BUILD/harmony-tests"
