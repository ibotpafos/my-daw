#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
# Reuse the complete native application's Debug core; never fetch a dependency.
test -f build/debug/libdaw_core.a || { echo 'Run scripts/build-macos.sh first' >&2; exit 1; }
xcrun swiftc -swift-version 6 -D MIXER_GROUP_ABI -import-objc-header engine/bridge/daw.h -sdk "$(xcrun --show-sdk-path)" \
  apps/macos/MixerModels.swift apps/macos/MixerScale.swift apps/macos/MixerControls.swift \
  apps/macos/MixerLinkedLevels.swift apps/macos/MixerGroupBinding.swift apps/macos/MixerConsoleState.swift apps/macos/MixerStripView.swift apps/macos/MixerWorkspaceView.swift \
  apps/macos/MixerRoutingMatrixView.swift apps/macos/MixerRoutingPresenter.swift \
  apps/macos/DesignSystem/DAWDesignTokens.swift apps/macos/DesignSystem/DAWDataVisuals.swift \
  tests/mixer_routing_tests.swift tests/mixer_send_tests.swift tests/mixer_linked_levels_tests.swift tests/mixer_ui_tests.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a -Xlinker -lc++ -lsqlite3 \
  -framework AppKit -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/mixer-group-integration-tests
./build/mixer-group-integration-tests
