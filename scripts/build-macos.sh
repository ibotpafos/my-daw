#!/bin/bash
set -euo pipefail
DAW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DAW_ROOT"
export MACOSX_DEPLOYMENT_TARGET=14.0
VST3_SDK_DIR="$DAW_ROOT/build/dependencies/vst3sdk"
VST3_HELPER_OPTION=OFF
if [ -f "$VST3_SDK_DIR/CMakeLists.txt" ] && [ -f "$DAW_ROOT/tools/vst3_scan_helper.cpp" ]; then
  VST3_HELPER_OPTION=ON
fi
cmake --preset debug -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DDAW_BUILD_VST3_SCAN_HELPER="$VST3_HELPER_OPTION"
BUILD_TARGETS=(daw_core daw_au_scan_helper)
if [ "$VST3_HELPER_OPTION" = ON ]; then
  BUILD_TARGETS+=(daw_vst3_scan_helper)
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
cp apps/macos/Info.plist "$DAW_APP/Contents/Info.plist"
cp build/debug/daw_au_scan_helper "$DAW_APP/Contents/MacOS/daw_au_scan_helper"
if [ "$VST3_HELPER_OPTION" = ON ]; then
  cp build/debug/daw_vst3_scan_helper "$DAW_APP/Contents/MacOS/daw_vst3_scan_helper"
  cp third_party/notices/VST3-SDK.txt "$DAW_APP/Contents/Resources/ThirdPartyNotices/VST3-SDK.txt"
fi
cp modules/vocal-preparation/module.json "$DAW_APP/Contents/Resources/Workflows/vocal-preparation/module.json"
xcrun swiftc -swift-version 6 -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header engine/bridge/daw.h apps/macos/main.swift apps/macos/DAWWindow.swift apps/macos/WaveformView.swift apps/macos/PinnedTrackHeaderView.swift apps/macos/MixerWorkspaceView.swift apps/macos/InspectorBrowserView.swift apps/macos/StorageUI.swift apps/macos/WorkflowUI.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers -framework AVFoundation -framework AudioToolbox -framework CoreAudio \
  -o "$DAW_APP/Contents/MacOS/My DAW"
# Set to an installed Apple Development/Developer ID identity to preserve the
# designated requirement across updates. Never weaken the requirement to a
# bundle identifier alone or modify the user's privacy database.
DAW_SIGNING_IDENTITY="${DAW_SIGNING_IDENTITY:--}"
codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP/Contents/MacOS/daw_au_scan_helper"
if [ "$VST3_HELPER_OPTION" = ON ]; then
  codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP/Contents/MacOS/daw_vst3_scan_helper"
fi
codesign --force --sign "$DAW_SIGNING_IDENTITY" "$DAW_APP"
codesign --verify --strict "$DAW_APP"
if [ "$DAW_SIGNING_IDENTITY" = "-" ]; then
  printf 'Local ad-hoc build: macOS may request file permissions again after an update.\n' >&2
fi
export DAW_SIGNING_IDENTITY
python3 scripts/build_manifest.py > build/build-manifest.json
printf 'Built %s\n' "$DAW_APP"
