#!/usr/bin/env python3
"""One-shot application of locally tested PR #8 edits. Removed before merging.

Every input AND result is pinned by SHA256; no fuzzy patching, downloads,
subprocess interpolation or writes outside the explicitly enumerated files.
"""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / '.github/maintenance/reviewed-edits.json'
EXPECTED_MANIFEST = '9ff7c3d3de2834cccfd09154ecbbe327e61860a7b53440b501d6086f4b155aa0'
ALLOWED = {
    '.github/workflows/core.yml', 'CMakeLists.txt', 'README.md', 'VERSION',
    'apps/macos/CommandPalette.swift', 'apps/macos/CommandPaletteSearch.swift',
    'apps/macos/DAWWindow.swift', 'apps/macos/Info.plist', 'apps/macos/main.swift',
    'docs/11-roadmap.md', 'docs/79-command-palette.md',
    'engine/audio/duplex_stub.cpp', 'engine/audio/recording.cpp',
    'engine/bridge/daw.cpp', 'engine/domain/session.cpp', 'engine/storage/storage.cpp',
    'scripts/test-command-palette.sh', 'tests/aiff_import_tests.cpp',
    'tests/import_jobs.cpp', 'tests/macos_command_palette_appkit_tests.swift',
    'tests/macos_command_palette_search_tests.swift',
}
def digest(data):
    return hashlib.sha256(data).hexdigest()

def require(condition, message):
    if not condition:
        raise SystemExit(message)

raw = MANIFEST.read_bytes()
require(digest(raw) == EXPECTED_MANIFEST, 'Manifest checksum mismatch')
manifest = json.loads(raw)
require(len(manifest) == len(ALLOWED) and {x['path'] for x in manifest} == ALLOWED,
        'Unexpected edit paths')
pending = []
for item in manifest:
    path = ROOT / item['path']
    require(path.is_file() and not path.is_symlink() and path.resolve().is_relative_to(ROOT),
            'Unsafe path: ' + item['path'])
    original = path.read_bytes()
    require(digest(original) == item['before'], 'Input moved: ' + item['path'])
    text = original.decode('utf-8')
    previous_end = 0
    for start, end, replacement in item['edits']:
        require(isinstance(start, int) and isinstance(end, int) and isinstance(replacement, str)
                and previous_end <= start <= end <= len(text), 'Invalid edit interval')
        previous_end = end
    for start, end, replacement in reversed(item['edits']):
        text = text[:start] + replacement + text[end:]
    data = text.encode('utf-8')
    require(digest(data) == item['after'], 'Result differs from tested source: ' + item['path'])
    pending.append((path, data))
# All preconditions are checked before any source file is changed.
for path, data in pending:
    path.write_bytes(data)
    print('Verified:', path.relative_to(ROOT), digest(data))
subprocess.run(['git', 'diff', '--check'], cwd=ROOT, check=True)
subprocess.run(['git', 'add', '--', *sorted(ALLOWED)], cwd=ROOT, check=True)
