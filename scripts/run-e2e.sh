#!/usr/bin/env bash
# Run the end-to-end scenario suite (tests/e2e/, CTest label "e2e").
#
#   ./scripts/run-e2e.sh                # debug preset, headless-safe
#   DAW_E2E_PRESET=sanitizers ./scripts/run-e2e.sh
#   DAW_E2E_DEVICE=1 ./scripts/run-e2e.sh   # additionally run the live
#                                           # hardware transport arc (plays
#                                           # a quiet tone through the default
#                                           # output device)
set -euo pipefail
cd "$(dirname "$0")/.."
PRESET="${DAW_E2E_PRESET:-debug}"

cmake --preset "${PRESET}" >/dev/null
cmake --build --preset "${PRESET}" -j "${DAW_BUILD_JOBS:-8}"

# The transport scenario self-gates its live branch on DAW_E2E_DEVICE=1; ctest
# forwards the environment as-is.
ctest --preset "${PRESET}" -L e2e --output-on-failure

# ABI coverage ledger: every public function must be driven by an e2e
# scenario, a white-box test, or a named manual gate.
python3 scripts/e2e_coverage.py
