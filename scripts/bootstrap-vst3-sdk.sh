#!/bin/bash
# Fetch the exact SDK tree recorded in dependencies.lock.json. This is an
# explicit developer action: CMake and build-macos.sh never download sources.
set -euo pipefail

DAW_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK_DIR="${1:-$DAW_ROOT/build/dependencies/vst3sdk}"
SDK_REPOSITORY="https://github.com/steinbergmedia/vst3sdk.git"
SDK_REVISION="3cdf9ca5d1f5b1b21e0a86832aa4abe55607bd96"

verify_revision() {
  local repository_path="$1"
  local expected_revision="$2"
  local label="$3"
  local actual_revision
  actual_revision="$(git -C "$repository_path" rev-parse HEAD)"
  if [ "$actual_revision" != "$expected_revision" ]; then
    printf '%s revision mismatch: expected %s, got %s\n' "$label" "$expected_revision" "$actual_revision" >&2
    exit 1
  fi
}

if [ -e "$SDK_DIR" ]; then
  if [ ! -d "$SDK_DIR/.git" ]; then
    printf 'Refusing to use non-git VST3 SDK directory: %s\n' "$SDK_DIR" >&2
    exit 1
  fi
else
  mkdir -p "$(dirname "$SDK_DIR")"
  git clone --no-checkout "$SDK_REPOSITORY" "$SDK_DIR"
  git -C "$SDK_DIR" checkout --detach "$SDK_REVISION"
  git -C "$SDK_DIR" submodule update --init base cmake pluginterfaces public.sdk
fi

verify_revision "$SDK_DIR" "$SDK_REVISION" "vst3sdk"
verify_revision "$SDK_DIR/base" "fcf9da0bd27a16f7f03773a3a39822f28f5c8477" "vst3sdk/base"
verify_revision "$SDK_DIR/cmake" "054c9143cbb8d47fc4694e473f2ee3b4d951a8f5" "vst3sdk/cmake"
verify_revision "$SDK_DIR/pluginterfaces" "4f547e8e102b47de4a8b8aaf343c73b700786372" "vst3sdk/pluginterfaces"
verify_revision "$SDK_DIR/public.sdk" "586dc5e6c8012c3e4b01c79389375cbe96bdb1da" "vst3sdk/public.sdk"

printf 'Pinned VST3 SDK is ready at %s\n' "$SDK_DIR"
