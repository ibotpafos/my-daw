#!/usr/bin/env python3
"""One-shot migration for the reviewed alpha snapshot; removed after application."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    'apps/macos/main.swift': 'f333231184864ad91940bc41e6783e9835387f7e',
    'CMakeLists.txt': 'd1b0e64af18bca9ff6b82b6129f4431f21bb8fce',
    'scripts/build-macos.sh': '4830e62f3d69f14a3b92850091fd1603d09450d7',
    'engine/storage/storage.cpp': '3e2d569a251ea227142410369a4380d5eb5cd7e2',
    'engine/bridge/daw.cpp': '31787a2734f2bf71d1096a69c7a9db3bbfcedde7',
    'engine/platform/macos/vst3_scanner.hpp': '19a0f63374684e0365f5b53033d387e5226f4080',
    'engine/platform/macos/vst3_scan_cache.hpp': 'e5eab64cd9c9fff4fb0fe24c3be87bfb251abc03',
}
for name, expected in EXPECTED.items():
    actual = subprocess.check_output(['git', 'hash-object', name], cwd=ROOT, text=True).strip()
    if actual != expected:
        raise SystemExit(f'Refusing changed input {name}: expected {expected}, got {actual}')

changes = {}
def once(text, before, after):
    if text.count(before) != 1:
        raise SystemExit(f'Expected exactly one replacement: {before[:100]!r}')
    return text.replace(before, after, 1)

def read(name):
    return (ROOT / name).read_text(encoding='utf-8')

# Extract intact declarations, preserving their access levels and implementations.
main = read('apps/macos/main.swift')
start = main.index('// MARK: - Темпо-карта проекта:')
views = main.index('@MainActor\nfinal class DraftCanvas', start)
app = main.index('@MainActor\nfinal class DraftApp:', views)
changes['apps/macos/ProjectTime.swift'] = 'import Foundation\n\n' + main[start:views]
changes['apps/macos/TimelineRulerView.swift'] = 'import AppKit\n\n' + main[views:app]
main = main[:start] + main[app:]
redundant = r'''        status.stringValue = revision != before ? "Тейк записано в клип · нот \(counters.recorded), отброшено \(counters.dropped + counters.unmatched)" : "Пустой тейк · проект не изменён"'''
main = once(main, redundant + '\n', '')
changes['apps/macos/main.swift'] = main

# Catalog metadata is platform-neutral; only scanning/loading belongs to macOS.
scanner = read('engine/platform/macos/vst3_scanner.hpp')
cache = read('engine/platform/macos/vst3_scan_cache.hpp')
class_start = scanner.index('// A VST3 class identifier')
class_end = scanner.index('struct Vst3QuarantineRecord', class_start)
entry_start = cache.index('struct Vst3ScanCacheEntry {')
entry_end = cache.index('struct Vst3ScanCache {', entry_start)
changes['engine/plugins/vst3_catalog.hpp'] = (
    '#pragma once\n\n#include <cstdint>\n#include <string>\n\nnamespace daw {\n\n'
    + scanner[class_start:class_end] + cache[entry_start:entry_end] + '} // namespace daw\n'
)
changes['engine/platform/macos/vst3_scanner.hpp'] = once(
    scanner[:class_start] + scanner[class_end:], '#include <chrono>',
    '#include "plugins/vst3_catalog.hpp"\n#include <chrono>')
changes['engine/platform/macos/vst3_scan_cache.hpp'] = cache[:entry_start] + cache[entry_end:]
changes['engine/bridge/daw.cpp'] = once(read('engine/bridge/daw.cpp'),
    '#include "plugins/plugin_descriptor.hpp"',
    '#include "plugins/plugin_descriptor.hpp"\n#include "plugins/vst3_catalog.hpp"')
changes['engine/storage/storage.cpp'] = once(read('engine/storage/storage.cpp'),
    '#include <sqlite3.h>', '#include <sqlite3.h>\n#include <algorithm>')

# CMake includes execute in the caller's directory; target paths are unchanged.
cmake = read('CMakeLists.txt')
platform = cmake.index('if(APPLE)\n')
tests = cmake.index('include(CTest)\n', platform)
changes['cmake/DawPlatform.cmake'] = cmake[platform:tests]
changes['cmake/DawTests.cmake'] = cmake[tests + len('include(CTest)\n'):]
root = cmake[:platform]
root = once(root, 'project(MyDAW VERSION 1.47.0 LANGUAGES C CXX)', '''file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DAW_VERSION)
string(STRIP "${DAW_VERSION}" DAW_VERSION)
if(NOT DAW_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain X.Y.Z")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(MyDAW VERSION ${DAW_VERSION} LANGUAGES C CXX)''')
root = once(root, 'target_include_directories(daw_core PUBLIC engine/bridge PRIVATE engine)',
    'target_include_directories(daw_core PUBLIC engine/bridge PRIVATE engine)\ntarget_compile_features(daw_core PUBLIC cxx_std_20)')
match = re.search(r'add_library\(daw_core STATIC ([^\n]+)\)', root)
if match is None:
    raise SystemExit('Cannot identify the explicit core source list')
root = root[:match.start()] + 'add_library(daw_core STATIC\n  ' + '\n  '.join(match.group(1).split()) + '\n)' + root[match.end():]
changes['CMakeLists.txt'] = root + '\ninclude(cmake/DawPlatform.cmake)\ninclude(CTest)\ninclude(cmake/DawTests.cmake)\n'

sources = sorted({p.relative_to(ROOT).as_posix() for p in (ROOT / 'apps/macos').rglob('*.swift')}
                 | {'apps/macos/ProjectTime.swift', 'apps/macos/TimelineRulerView.swift'})
changes['apps/macos/sources.txt'] = '# Explicit Swift application sources; tests are compiled separately.\n' + '\n'.join(sources) + '\n'
build = read('scripts/build-macos.sh')
start = build.index('  -import-objc-header engine/bridge/daw.h ')
end = build.index('\n', start)
build = build[:start] + '  -import-objc-header engine/bridge/daw.h "${SWIFT_SOURCES[@]}" \\' + build[end:]
build = once(build, 'xcrun swiftc -swift-version 6', '''SWIFT_SOURCES=()
while IFS= read -r source || [ -n "$source" ]; do
  case "$source" in ''|\\#*) continue ;; esac
  if [ ! -f "$source" ]; then
    printf 'Missing Swift source: %s\\n' "$source" >&2
    exit 1
  fi
  SWIFT_SOURCES+=("$source")
done < apps/macos/sources.txt
xcrun swiftc -swift-version 6''')
changes['scripts/build-macos.sh'] = build

for name, text in changes.items():
    path = ROOT / name
    if name not in EXPECTED and path.exists():
        raise SystemExit(f'Refusing to overwrite new output {name}')
for name, text in changes.items():
    path = ROOT / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')
    print(name)
for name in ['scripts/test-au-music-device.sh', 'scripts/test-midi-hardware.sh', 'scripts/test-audio-preview.sh']:
    path = ROOT / name
    path.chmod(path.stat().st_mode | 0o111)
print('Applied bounded layout migration. Build and regression tests are still required.')
