#!/usr/bin/env bash
# Offscreen integration harness. Normal app launch remains script/build_and_run.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -m unittest discover -s tests -p test_native_test_runner.py -v
bash scripts/test-mix-export-policy.sh
mkdir -p build/mix-export-ui
sources=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  sources+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -g -swift-version 6 -warnings-as-errors -D DAW_MIX_EXPORT_TESTS \
  -target arm64-apple-macosx14.0 -sdk "$(xcrun --show-sdk-path)" \
  -import-objc-header apps/macos/DAWBridge.h "${sources[@]}" tests/mix_export_ui_tests.swift \
  build/debug/libdaw_core.a build/debug/libdaw_au_scanner.a \
  -Xlinker -lc++ -lsqlite3 -framework AppKit -framework UniformTypeIdentifiers \
  -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework CoreMIDI \
  -o build/mix-export-ui/export-tests
# swiftc -g already creates the dSYM before deleting temporary Swift objects.
# A second dsymutil invocation loses their DWARF. Keep the driver's output and
# verify both UUID identity and actual Swift line tables, not merely existence.
python3 - <<'PY_SYMBOLS'
import json, re, subprocess
from pathlib import Path
root = Path('build/mix-export-ui')
binary, symbols = root / 'export-tests', root / 'export-tests.dSYM'
uuids = subprocess.run(['xcrun', 'dwarfdump', '--uuid', str(binary), str(symbols)],
                       check=True, capture_output=True, text=True).stdout
identities = re.findall(r'UUID: ([0-9A-Fa-f-]+) \(([^)]+)\)', uuids)
assert len(identities) == 2 and identities[0] == identities[1], uuids
lines = subprocess.run(['xcrun', 'dwarfdump', '--debug-line', str(symbols)],
                       check=True, capture_output=True, text=True).stdout
required = ['mix_export_ui_tests.swift', 'MixExportUI.swift', 'StorageUI.swift']
assert all(name in lines for name in required), 'Swift line tables missing from dSYM'
evidence = root / 'diagnostics'; evidence.mkdir(parents=True, exist_ok=True)
(evidence / 'symbols.json').write_text(json.dumps({
    'uuid': identities[0][0], 'architecture': identities[0][1],
    'swift_line_tables': required, 'source': 'swiftc -g generated dSYM'
}, indent=2) + '\n')
print('PASS: exact native UUID and original Swift DWARF line tables')
PY_SYMBOLS
# A stale proof from a previous invocation must never certify this run.
rm -f build/mix-export-ui/proof.json
python3 scripts/native_test_runner.py --timeout 180 \
  --evidence build/mix-export-ui/diagnostics -- build/mix-export-ui/export-tests
