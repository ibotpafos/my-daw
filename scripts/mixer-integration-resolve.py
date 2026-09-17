#!/usr/bin/env python3
"""One-time, pinned conflict resolution. Each output is verified before staging."""
from pathlib import Path
import subprocess
import re

ROOT = Path.cwd()
BASE = '176aeb314983295fcf234915ac97de362d567607'
MIXER = '0c8034aae50d69db4ac7e07bca1f685d25d28057'

def read(revision, path):
    return subprocess.check_output(['git', 'show', f'{revision}:{path}'], text=True)

def replace(text, old, new):
    if text.count(old) != 1:
        raise SystemExit(f'Expected unique preimage: {old[:100]}')
    return text.replace(old, new)

def function(text, name):
    match = re.search(r'^(?:int|void) ' + re.escape(name) + r'\(', text, re.M)
    if not match:
        raise SystemExit(f'Missing function: {name}')
    start = text.index('{', match.start())
    depth = 0
    # Skip comments and string/character literals before counting braces.
    tokens = r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|[{}]'
    for token in re.finditer(tokens, text[start:]):
        if token[0] == '{':
            depth += 1
        elif token[0] == '}':
            depth -= 1
            if depth == 0:
                return text[match.start():start + token.end()]
    raise SystemExit(f'Unbalanced function: {name}')

# Preserve main's MIDI transactions and compiler-clean formatting. Add mixer APIs
# from the exact already tested feature revision, not a moving remote branch.
path = 'engine/bridge/daw.cpp'
s = read(BASE, path)
ours = read(MIXER, path)
header = '#include "platform/macos/vst3_scan_cache.hpp"'
if header not in s:
    s = replace(s, '#include "plugins/plugin_descriptor.hpp"', '#include "plugins/plugin_descriptor.hpp"\n' + header)
for name in ['beginPlaybackPreparation', 'startRecording']:
    old = function(s, name)
    offset = old.index('{') + 1
    new = old[:offset] + '\n    if (s->model.mixerGestureActive())\n        throw daw::Error("Finish the mixer gesture before starting playback or recording");' + old[offset:]
    s = replace(s, old, new)
names = ['daw_begin_mixer_gesture', 'daw_begin_track_gain_group', 'daw_write_mixer_gesture',
         'daw_end_mixer_gesture', 'daw_cancel_mixer_gesture', 'daw_set_solo_exclusive',
         'daw_get_send_controls', 'daw_set_send_muted', 'daw_set_send_pan']
for name in names:
    if name + '(' in s:
        raise SystemExit(f'Unexpected duplicate API: {name}')
offset = re.search(r'^int daw_set_gain\(', s, re.M).start()
s = s[:offset] + '\n'.join(function(ours, name) for name in names) + '\n' + s[offset:]
s = replace(s, function(s, 'daw_upsert_send'), function(ours, 'daw_upsert_send'))
old = function(s, 'daw_delete_bus')
s = replace(s, old, replace(old, 'cancelStalePlaybackPreparation(s);', 'resetTransport(s);'))
old = function(s, 'daw_open_draft')
s = replace(s, old, replace(old, 'auto loaded = daw::readDraft(required(path));', 'if (s->model.mixerGestureActive())\n            throw daw::Error("Finish the mixer gesture before opening a project");\n        auto loaded = daw::readDraft(required(path));'))
(ROOT / path).write_text(s)

path = 'engine/domain/session.cpp'
s = read(BASE, path)
s = replace(s, 'throw Error("Send gain outside -120…24 dB");', 'throw Error("Send gain outside -120…24 dB");\n            if (!std::isfinite(send.pan) || send.pan < -1 || send.pan > 1)\n                throw Error("Send balance outside -1…1");')
for signature in ['void Session::check(uint64_t expected) const {', 'void Session::replace(State state) {']:
    s = replace(s, signature, signature + '\n    if (mixerGesture)\n        throw Error("Mixer gesture is active");')
(ROOT / path).write_text(s)

path = 'engine/storage/storage.cpp'
s = read(BASE, path)
ours = read(MIXER, path)
s = replace(s, 'PRAGMA user_version=21;', 'PRAGMA user_version=22;')
s = replace(s, 'formatVersion > 21', 'formatVersion > 22')
s = replace(s, re.search(r'"CREATE TABLE sends[^\n]*', s)[0], re.search(r'"CREATE TABLE sends[^\n]*', ours)[0])
s = replace(s, 'INSERT INTO sends VALUES(?,?,?,?,?)', 'INSERT INTO sends VALUES(?,?,?,?,?,?,?,?)')
s = replace(s, 'sqlite3_bind_int(sendRow.get(), 5, send.preFader);', 'sqlite3_bind_int(sendRow.get(), 5, send.preFader);\n                sqlite3_bind_double(sendRow.get(), 6, send.pan);\n                sqlite3_bind_int(sendRow.get(), 7, send.muted);\n                sqlite3_bind_int(sendRow.get(), 8, send.independentPan);')
start = s.index('        auto sends = prepare(')
end = s.index('        if (rc != SQLITE_DONE)\n            throw Error("Cannot read draft sends");', start)
a = ours.index('        auto sends=prepare(db.get(),formatVersion>=22')
b = ours.index('        if(rc!=SQLITE_DONE)throw Error("Cannot read draft sends");', a)
s = s[:start] + ours[a:b] + s[end:]
(ROOT / path).write_text(s)

path = 'engine/audio/recording.cpp'
(ROOT / path).write_text(read(BASE, path))
for path in ['scripts/build-macos.sh', 'scripts/typecheck-macos-ui.sh']:
    s = read(BASE, path)
    s = replace(s, 'PIANO_ROLL_SOURCES=(apps/macos/PianoRoll/*.swift)', 'PIANO_ROLL_SOURCES=(apps/macos/PianoRoll/*.swift)\nMIXER_SOURCES=(apps/macos/Mixer*.swift)')
    s = replace(s, 'apps/macos/MixerWorkspaceView.swift', '"${MIXER_SOURCES[@]}"')
    (ROOT / path).write_text(s)

# Exact local review outputs, including files automatically merged by Git.
expected = {
    'engine/audio/recording.cpp': 'ba2b12f5ca7a932e4a8406e7a86f82e544098dff',
    'engine/bridge/daw.cpp': 'b6ee0079afb14e7601ec12f1b11c309ff5c67ac3',
    'engine/domain/session.cpp': '454ae8d3a668a519593ba0d63de246b755f03d81',
    'engine/storage/storage.cpp': 'd43f799a4d15890fcfb9d14b79995c107061dd14',
    'scripts/build-macos.sh': '72bee0a7f6da4fa65811871535655d1680eec0ee',
    'scripts/typecheck-macos-ui.sh': 'f4ba86017f08d709c3cea4b6c1740663711a6483',
    'tests/e2e/e2e_mixer_groups.cpp': '56a6e48ac5e4f9287e75b419f79729158e7bc36d',
    'apps/macos/main.swift': '5f80807358945dfb92c1d99d8ca8c62f3612f496',
    'engine/bridge/daw.h': '9b27387a9a902231c1139edda45208ff29f53d7e',
}
for path, sha in expected.items():
    actual = subprocess.check_output(['git', 'hash-object', path], text=True).strip()
    if actual != sha:
        raise SystemExit(f'Resolution output mismatch: {path}: {actual} != {sha}')
subprocess.run(['git', 'add', *expected], check=True)
print('Pinned source resolution matches all reviewed file hashes.')
