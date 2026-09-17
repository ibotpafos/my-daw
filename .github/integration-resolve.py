from pathlib import Path
import subprocess,re
root=Path.cwd()
def ours(path): return subprocess.check_output(['git','show',f'HEAD:{path}'],cwd=root,text=True)
for path in ['.github/workflows/core.yml','engine/audio/duplex_stub.cpp','engine/audio/recording.cpp','scripts/build-macos.sh']:
    (root/path).write_text(ours(path))
p=root/'apps/macos/StorageUI.swift'
s=p.read_text(); s=re.sub(r'<<<<<<< HEAD\n(.*?)=======\n(.*?)>>>>>>> source/main\n',lambda m:m[1]+m[2],s,flags=re.S);p.write_text(s)
p=root/'engine/bridge/daw.cpp';s=p.read_text()
s=re.sub(r'<<<<<<< HEAD\n(.*?)=======\n(.*?)>>>>>>> source/main\n',lambda m:m[2] if ('void validateMidiNoteArray' in m[1] or 'validateMidiNoteArray(notes, noteCount)' in m[1]) else m[1],s,flags=re.S);p.write_text(s)
p=root/'tests/midi_bridge_tests.cpp';s=p.read_text();s=re.sub(r'<<<<<<< HEAD\n(.*?)=======\n(.*?)>>>>>>> source/main\n',lambda m:m[1].splitlines(keepends=True)[0]+m[2],s,flags=re.S);p.write_text(s)
p=root/'apps/macos/InspectorBrowserView.swift';s=ours('apps/macos/InspectorBrowserView.swift')
s=s.replace('    var editable = false\n','    var editable = false\n    var context: PRClipContext?\n',1)
s=s.replace('    var onMidiNotesChange: (([PianoRollNote]) -> Void)?\n','    var onMidiNotesChange: (([PianoRollNote]) -> Void)?\n    var onMidiCommitRequest: ((PRCommitRequest) -> Void)?\n',1)
s=s.replace('        midiEditor.onNotesChange = { [weak self] in self?.onMidiNotesChange?($0) }\n','        midiEditor.onNotesChange = { [weak self] in self?.onMidiNotesChange?($0) }\n        midiEditor.onCommitRequest = { [weak self] in self?.onMidiCommitRequest?($0) }\n',1)
a='''        midiEditor.editorEnabled = editingEnabled && midi?.editable == true
        midiEditor.clips = midi?.clips ?? []; midiEditor.selectedClip = midi?.selectedClip
        midiEditor.notes = midi?.notes ?? []'''
b='''        var model = midi
        model?.editable = editingEnabled && midi?.editable == true
        midiEditor.apply(model: model)'''
assert a in s;s=s.replace(a,b,1);p.write_text(s)
p=root/'apps/macos/sources.txt';s=p.read_text();names=[str(x.relative_to(root)) for x in sorted((root/'apps/macos/PianoRoll').glob('*.swift'))];s+='\n'.join(names)+'\n';p.write_text(s)
p=root/'scripts/typecheck-macos-ui.sh';s=p.read_text();start=s.index('PIANO_ROLL_SOURCES=');end=s.index('xcrun swiftc',start)
s=s[:start]+r'''APP_SOURCES=()
while IFS= read -r source || [[ -n "$source" ]]; do
  case "$source" in ''|\#*) continue ;; esac
  [[ -f "$source" ]] || { printf 'Missing Swift source: %s\n' "$source" >&2; exit 1; }
  APP_SOURCES+=("$source")
done < apps/macos/sources.txt
'''+s[end:];s=s.replace('-import-objc-header engine/bridge/daw.h','-import-objc-header apps/macos/DAWBridge.h');p.write_text(s)
(root/'.github/workflows/integration-source-evidence.yml').unlink()
