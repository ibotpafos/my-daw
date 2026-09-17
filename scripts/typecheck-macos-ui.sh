#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
export MACOSX_DEPLOYMENT_TARGET=14.0
SDK="$(xcrun --show-sdk-path)"
PIANO_ROLL_SOURCES=(apps/macos/PianoRoll/*.swift)
APP_SOURCES=(
  apps/macos/Diagnostics.swift
  apps/macos/main.swift
  apps/macos/CommandPaletteSearch.swift
  apps/macos/CommandPalette.swift
  apps/macos/DAWWindow.swift
  apps/macos/WaveformView.swift
  apps/macos/PinnedTrackHeaderView.swift
  apps/macos/MixerWorkspaceView.swift
  apps/macos/InspectorBrowserView.swift
  apps/macos/AudioPreviewController.swift
  apps/macos/StorageUI.swift
  apps/macos/WorkflowUI.swift
  "${PIANO_ROLL_SOURCES[@]}"
  apps/macos/PianoRollView.swift
  apps/macos/DesignSystem/DAWDesignTokens.swift
  apps/macos/DesignSystem/DAWIcon.swift
  apps/macos/DesignSystem/DAWDataVisuals.swift
)
xcrun swiftc -typecheck -swift-version 6 -warnings-as-errors \
  -target arm64-apple-macosx14.0 -sdk "$SDK" \
  -import-objc-header engine/bridge/daw.h \
  "${APP_SOURCES[@]}"
printf 'AppKit Swift typecheck passed.\n'
