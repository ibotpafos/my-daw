#!/bin/bash
set -euo pipefail
DAW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DAW_ROOT"
export MACOSX_DEPLOYMENT_TARGET=14.0
VST3_SDK_DIR="$DAW_ROOT/build/dependencies/vst3sdk"
VST3_HELPER_OPTION=OFF
VST3_RUNTIME_HELPER_OPTION=OFF
if [ -f "$VST3_SDK_DIR/CMakeLists.txt" ] && [ -f "$DAW_ROOT/tools/vst3_scan_helper.cpp" ]; then
  VST3_HELPER_OPTION=ON
fi
if [ -f "$VST3_SDK_DIR/CMakeLists.txt" ] && [ -f "$DAW_ROOT/tools/vst3_runtime_helper.cpp" ]; then
  VST3_RUNTIME_HELPER_OPTION=ON
fi
cmake --preset debug -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DDAW_BUILD_VST3_SCAN_HELPER="$VST3_HELPER_OPTION" -DDAW_BUILD_VST3_RUNTIME_HELPER="$VST3_RUNTIME_HELPER_OPTION"
BUILD_TARGETS=(daw_core daw_au_scan_helper)
if [ "$VST3_HELPER_OPTION" = ON ]; then
  BUILD_TARGETS+=(daw_vst3_scan_helper)
fi
if [ "$VST3_RUNTIME_HELPER_OPTION" = ON ]; then
  BUILD_TARGETS+=(daw_vst3_runtime_helper)
fi
cmake --build --preset debug --target "${BUILD_TARGETS[@]}"
DAW_APP="$DAW_ROOT/build/My DAW.app"
# Do not replace a running bundle: its ad-hoc signing identity and mapped code must stay stable.
if pgrep -f "^$DAW_APP/Contents/MacOS/My DAW$" >/dev/null; then
  DAW_APP="$DAW_ROOT/build/My DAW Next.app"
  if pgrep -f "^$DAW_APP/Contents/MacOS/My DAW$" >/dev/null; then
    DAW_APP="$DAW_ROOT/build/My DAW Candidate.app"
    if pgrep -f "^$DAW_APP/Contents/MacOS/My DAW$" >/dev/null; then
      printf 'All three app build slots are running. Save and close one before rebuilding.\n' >&2
      exit 1
    fi
  fi
fi
mkdir -p "$DAW_APP/Contents/MacOS"
mkdir -p "$DAW_APP/Contents/Resources/Workflows/vocal-preparation"
mkdir -p "$DAW_APP/Contents/Resources/ThirdPartyNotices"
mkdir -p "$DAW_APP/Contents/Resources/DesignSystem"
cp apps/macos/Info.plist "$DAW_APP/Contents/Info.plist"
DAW_VERSION="$(tr -d '[:space:]' < VERSION)"
DAW_COMMIT="$(git -C "$DAW_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
python3 - "$DAW_APP/Contents/Info.plist" "$DAW_VERSION" "$DAW_COMMIT" <<'PY'
import plistlib, sys
path, version, commit = sys.argv[1:4]
with open(path, "rb") as handle:
    data = plistlib.load(handle)
data["CFBundleShortVersionString"] = version
data["CFBundleVersion"] = version
data["DAWBuildCommit"] = commit
with open(path, "wb") as handle:
    plistlib.dump(data, handle)
PY
cp build/debug/daw_au_scan_helper "$DAW_APP/Contents/MacOS/daw_au_scan_helper"
if [ "$VST3_HELPER_OPTION" = ON ]; then
  cp build/debug/daw_vst3_scan_helper "$DAW_APP/Contents/MacOS/daw_vst3_scan_helper"
  cp third_party/notices/VST3-SDK.txt "$DAW_APP/Contents/Resources/ThirdPartyNotices/VST3-SDK.txt"
fi
if [ "$VST3_RUNTIME_HELPER_OPTION" = ON ]; then
  cp build/debug/daw_vst3_runtime_helper "$DAW_APP/Contents/MacOS/daw_vst3_runtime_helper"
fi
cp modules/vocal-preparation/module.json "$DAW_APP/Contents/Resources/Workflows/vocal-preparation/module.json"
cp apps/macos/DesignSystem/ui-kit-manifest.json "$DAW_APP/Contents/Resources/DesignSystem/ui-kit-manifest.json"
cp apps/macos/DesignSystem/Assets/*.svg "$DAW_APP/Contents/Resources/DesignSystem/"
xcrun swiftc -swift-version 6 -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header engine/bridge/daw.h apps/macos/Diagnostics.swift apps/macos/main.swift apps/macos/DAWWindow.swift apps/macos/WaveformView.swift apps/macos/PinnedTrackHeaderView.swift apps/macos/MixerScale.swift apps/macos/MixerConsoleState.swift apps/macos/MixerModels.swift apps/macos/MixerControls.swift apps/macos/MixerStripView.swift apps/macos/MixerWorkspaceView.swift apps/macos/MixerRoutingMatrixView.swift apps/macos/MixerRoutingPresenter.swift apps/macos/MixerConsoleController.swift apps/macos/InspectorBrowserView.swift apps/macos/AudioPreviewController.swift apps/macos/StorageUI.swift apps/macos/WorkflowUI.swift apps/macos/PianoRollView.swift apps/macos/DesignSystem/DAWDesignTokens.swift apps/macos/DesignSystem/DAWIcon.swift apps/macos/DesignSystem/DAWDataVisuals.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o "$DAW_APP/Contents/MacOS/My DAW"
DAW_SIGNING_IDENTITY="${DAW_SIGNING_IDENTITY:--}"
codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP/Contents/MacOS/daw_au_scan_helper"
if [ "$VST3_HELPER_OPTION" = ON ]; then
  codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP/Contents/MacOS/daw_vst3_scan_helper"
fi
if [ "$VST3_RUNTIME_HELPER_OPTION" = ON ]; then
  codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP/Contents/MacOS/daw_vst3_runtime_helper"
fi
codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP"
codesign --verify --strict "$DAW_APP"
if [ "$DAW_SIGNING_IDENTITY" = "-" ]; then
  printf 'Local ad-hoc build: macOS may request file permissions again after an update.\n' >&2
fi
export DAW_SIGNING_IDENTITY
python3 scripts/build_manifest.py > build/build-manifest.json
printf 'Built %s\n' "$DAW_APP"
