#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/my-daw-piano-appkit.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
cd "$ROOT"
export MACOSX_DEPLOYMENT_TARGET=14.0
SDK="$(xcrun --show-sdk-path)"
SOURCES=(
  apps/macos/DesignSystem/DAWDesignTokens.swift
  apps/macos/PianoRoll/PianoRollModel.swift
  apps/macos/PianoRoll/PianoRollTiming.swift
  apps/macos/PianoRoll/PianoRollEdits.swift
  apps/macos/PianoRoll/PianoRollClipboard.swift
  apps/macos/PianoRoll/PianoRollTransforms.swift
  apps/macos/PianoRoll/PianoRollHarmony.swift
  apps/macos/PianoRoll/PianoRollNoteIndex.swift
  apps/macos/PianoRoll/PianoRollProState.swift
  apps/macos/PianoRoll/PianoRollProCommands.swift
  apps/macos/PianoRoll/PianoRollProPreview.swift
  apps/macos/PianoRoll/PianoRollProDrawing.swift
  apps/macos/PianoRoll/PianoRollProCanvas.swift
  apps/macos/PianoRoll/PianoRollProSurfaces.swift
  apps/macos/PianoRoll/PianoRollProWorkspace.swift
  apps/macos/PianoRoll/PianoRollProWindow.swift
  tests/piano_roll/AppKitSmokeTests.swift
)
xcrun swiftc -swift-version 6 -warnings-as-errors -O \
  -target arm64-apple-macosx14.0 -sdk "$SDK" \
  "${SOURCES[@]}" -framework AppKit -o "$BUILD/piano-roll-appkit-tests"
"$BUILD/piano-roll-appkit-tests"
