#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="$(mktemp -d "${TMPDIR:-/tmp}/mydaw-export-policy.XXXXXX")"
trap 'rm -rf "$BUILD"' EXIT
swiftc -swift-version 6 -warnings-as-errors -O -parse-as-library \
  apps/macos/MixExportPolicy.swift tests/mix_export_policy_tests.swift \
  -o "$BUILD/policy-tests"
"$BUILD/policy-tests"
