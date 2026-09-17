#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
xcrun swiftc -swift-version 6 -sdk "$(xcrun --show-sdk-path)" \
  apps/macos/MixerModels.swift apps/macos/MixerScale.swift apps/macos/MixerControls.swift \
  apps/macos/MixerConsoleState.swift apps/macos/MixerStripView.swift apps/macos/MixerWorkspaceView.swift \
  apps/macos/MixerRoutingMatrixView.swift apps/macos/MixerRoutingPresenter.swift \
  apps/macos/DesignSystem/DAWDesignTokens.swift apps/macos/DesignSystem/DAWDataVisuals.swift \
  tests/mixer_routing_tests.swift tests/mixer_ui_tests.swift \
  -framework AppKit -o build/mixer-ui-tests
./build/mixer-ui-tests
