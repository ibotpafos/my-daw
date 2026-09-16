#!/usr/bin/env bash
# Compatibility alias; keep launch, shutdown and verification in one place.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$ROOT_DIR/script/build_and_run.sh" "$@"
