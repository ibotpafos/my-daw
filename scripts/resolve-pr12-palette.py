from pathlib import Path
import shutil, subprocess, sys
root=Path(sys.argv[1]); repo=Path.cwd(); head=root/'pr12'; main=root/'main'
def write(path,text):
    p=repo/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
def copy(path,base=head): shutil.copy2(base/path,repo/path)
def replace(text,old,new):
    assert text.count(old)==1, (old[:100],text.count(old))
    return text.replace(old,new)
assert subprocess.check_output(['git','rev-parse','HEAD^{tree}'],text=True).strip() == '05850ed6939f44e6d061b6eb2f0b10aa6753bfac'
for path in ['README.md', 'VERSION', 'apps/macos/CommandPalette.swift',
             'apps/macos/CommandPaletteSearch.swift', 'apps/macos/Info.plist',
             'docs/11-roadmap.md', 'docs/79-command-palette.md',
             'scripts/test-command-palette.sh', 'tests/macos_command_palette_appkit_tests.swift',
             'tests/macos_command_palette_search_tests.swift']:
    copy(path, main)
for path in ['CMakeLists.txt','scripts/build-macos.sh','scripts/typecheck-macos-ui.sh','tests/aiff_import_tests.cpp','tests/import_jobs.cpp']:
    copy(path)
window=(main/'apps/macos/DAWWindow.swift').read_text()
window=replace(window,'    var onPlayStop:', '    var onFocusedKeyDown: ((NSEvent) -> Bool)?\n    var shouldHandleClipDelete: (() -> Bool)?\n    var onPlayStop:')
window=replace(window,'           handleUnmodifiedGlobalCommand(event) {','           (onFocusedKeyDown?(event) == true || handleUnmodifiedGlobalCommand(event)) {')
window=replace(window,'        if super.performKeyEquivalent(with: event) { return true }','        if onFocusedKeyDown?(event) == true { return true }\n        if super.performKeyEquivalent(with: event) { return true }')
window=replace(window,'        case 51, 117: return invoke(onDeleteSelectedClip)','        case 51, 117:\n            guard shouldHandleClipDelete?() ?? true else { return false }\n            return invoke(onDeleteSelectedClip)')
write('apps/macos/DAWWindow.swift',window)
app=(head/'apps/macos/main.swift').read_text()
anchor='        let edit = NSMenuItem(); edit.title = "Текст"; let submenu = NSMenu(title: "Текст")'
app=replace(app,anchor,'''        if let viewMenu = main.items.last?.submenu {
            viewMenu.insertItem(.separator(), at: 0)
            viewMenu.insertItem(DAWWindow.makeCommandPaletteMenuItem(), at: 0)
        }
'''+anchor)
write('apps/macos/main.swift',app)
manifest=(head/'apps/macos/sources.txt').read_text()
manifest=replace(manifest,'apps/macos/AudioPreviewController.swift\n','apps/macos/AudioPreviewController.swift\napps/macos/CommandPalette.swift\napps/macos/CommandPaletteSearch.swift\n')
write('apps/macos/sources.txt',manifest)
imports=(head/'tests/import_jobs.cpp').read_text()
imports=replace(imports,'    daw::cancelImport(*projectRate);','    CHECK(daw::importClip(*projectRate)->frames() == 44100);\n    daw::cancelImport(*projectRate);\n    CHECK(projectRate->status.load(std::memory_order_acquire) == daw::ImportJobStatus::Canceled && !daw::importClip(*projectRate));')
write('tests/import_jobs.cpp',imports)
workflow=(head/'.github/workflows/core.yml').read_text()
workflow=replace(workflow,'jobs:\n','env:\n  UBSAN_OPTIONS: halt_on_error=1:print_stacktrace=1\njobs:\n')
workflow=replace(workflow,'      - name: Build\n        run: cmake --build --preset sanitizers --parallel 2','''      - name: Build
        shell: bash
        run: |
          set -o pipefail
          cmake --build --preset sanitizers --parallel 2 2>&1 | tee "$RUNNER_TEMP/core-build.log"''')
workflow=replace(workflow,'      - name: Build AppKit prototype','''      - name: Typecheck complete AppKit UI
        if: ${{ !cancelled() && runner.os == 'macOS' }}
        run: bash scripts/typecheck-macos-ui.sh
      - name: Build AppKit prototype''')
mainwf=(main/'.github/workflows/core.yml').read_text()
extras=mainwf[mainwf.index('      - name: Package reproducible build input'):mainwf.index('  command-palette:')]
workflow=replace(workflow,'  source-quality:\n',extras+'  source-quality:\n')
workflow+='\n'+mainwf[mainwf.index('  command-palette:'):]
workflow=workflow.replace('actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683 # v4.2.2','actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1').replace('actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02 # v4.6.2','actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a # v7.0.1')
write('.github/workflows/core.yml',workflow)
test=(main/'tests/macos_command_palette_appkit_tests.swift').read_text()
anchor='        expect(host.performKeyEquivalent(with: key(code: 40, characters: "k", flags: .command, window: host)), "main window shortcut opens palette")'
test=replace(test,anchor,'''        var clipDeletes = 0
        var trackDeletes = 0
        var plays = 0
        var focusedEvents = 0
        host.onPlayStop = { plays += 1 }
        host.onDeleteSelectedClip = { clipDeletes += 1 }
        host.onDeleteSelectedTrack = { trackDeletes += 1 }
        host.onFocusedKeyDown = { event in
            guard [UInt16(49), 51, 117].contains(event.keyCode) else { return false }
            focusedEvents += 1
            return true
        }
        host.sendEvent(key(code: 49, characters: " ", flags: [], window: host))
        host.sendEvent(key(code: 51, characters: "\\u{7f}", flags: [], window: host))
        expect(host.performKeyEquivalent(with: key(code: 51, characters: "\\u{7f}", flags: .command, window: host)), "focused library owns Command-Delete")
        expect(focusedEvents == 3 && plays == 0 && clipDeletes == 0 && trackDeletes == 0,
               "library keyboard actions cannot leak into arrangement or transport")
        host.onFocusedKeyDown = nil
        host.shouldHandleClipDelete = { false }
        host.sendEvent(key(code: 51, characters: "\\u{7f}", flags: [], window: host))
        expect(clipDeletes == 0, "clip deletion gate survives palette integration")
        host.shouldHandleClipDelete = { true }
        host.sendEvent(key(code: 51, characters: "\\u{7f}", flags: [], window: host))
        host.sendEvent(key(code: 49, characters: " ", flags: [], window: host))
        expect(clipDeletes == 1 && plays == 1, "global commands still work outside library")
        host.onFocusedKeyDown = { _ in true }
'''+anchor)
write('tests/macos_command_palette_appkit_tests.swift',test)
doc=(repo/'docs/79-command-palette.md').read_text()
doc=replace(doc,'В Linux/GCC устранены неоднозначные', '**Историческая проверка PR #8 до объединения с #12.** В Linux/GCC устранены неоднозначные')
doc+='''\n## Совместная интеграция с workspace (#12)

При объединении с `main` после PR #8 сохранены `onFocusedKeyDown` и
`shouldHandleClipDelete`: библиотека владеет своими Space/Delete/Command-Delete,
а палитра — `⌘K`. Видимое меню workspace сохранено; в него добавлен пункт палитры.
Оба Swift-файла палитры входят в общий `sources.txt`, используемый приложением,
полным typecheck и native workspace/export harness. Добавлены проверки маршрутизации
клавиш в настоящий `DAWWindow`; прежние проверки палитры не удалены.

Текущий Linux-контракт после #1/#12 — успешный ресемплинг через libsamplerate,
а не исторический отказ заглушки. WAV/AIFF positive-conversion tests сохранены
на обеих платформах. Отдельный macOS job палитры, все семь validation workflows,
ASan/UBSan, обязательные схемы и source-quality checks также сохранены.
Точные результаты объединённого дерева фиксируются в PR #12 после завершения CI.
'''
write('docs/79-command-palette.md',doc)
doc=(head/'docs/89-main-integration.md').read_text()
doc+='''\n## Integration with merged Command Palette #8

The follow-up joins `main` at `fdcd21842c017257e1b41ce47eaa7445af590c0d`
and PR #12 at `f95a8c9b62a91c3ba01eff1892f8c7e2c65f0603`. Both real histories
are retained. Palette menu/history/validation are combined with workspace-local
keyboard handling, the shared Swift manifest, portable resampling, and all prior
Piano Roll/Mixer/export guards. Version 1.74.0 is retained.

The old non-Apple failure expectations from #8 are not applied over the working
libsamplerate adapter. No core API, DSP, draft version, existing test or sanitizer
gate is removed. The independent native palette job and full UI typecheck remain
alongside the seven workflows already used by this integration. New keyboard
regressions exercise the actual window boundary for library and palette ownership.

Current validation and actual merge status are recorded in PR #12; old successful
runs above do not prove this newer tree. Physical audio/MIDI and manual VoiceOver
acceptance remain open, irrespective of automated checks or merge status.
'''
write('docs/89-main-integration.md',doc)
copy('docs/index.html')
subprocess.run(['git','add','-A'],check=True)
assert subprocess.check_output(['git','write-tree'],text=True).strip() == 'd8a6817947ad4149c841162f37b9fd2343e45a60', 'Resolved tree differs from locally reviewed content'
print(subprocess.check_output(['git','diff','--cached','--stat'],text=True))
