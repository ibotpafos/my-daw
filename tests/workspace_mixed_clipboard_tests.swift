import AppKit

/// Real window/menu commands against the production bridge, not mocked callbacks.
@MainActor
func runArrangementMixedClipboardTests(_ app: DraftApp) -> Int {
    guard let editor = app.window.arrangementEditing as? ArrangementEditingController,
          let root = app.window.contentView else { fatalError("Mixed clipboard is not mounted") }
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ text: String) {
        checks += 1
        if !value() { fatalError("Native mixed clipboard: \(text)") }
    }
    func revision() -> UInt64 { editor.currentRevision()! }
    func settle() {
        root.layoutSubtreeIfNeeded(); app.fitTimelineViewport()
        root.layoutSubtreeIfNeeded(); editor.bindProjection()
    }
    func addTrack(_ title: String) -> UInt64 {
        expect(daw_add_track(app.session, title, revision()) == 0, "Add fixture track")
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(app.session, &snapshot) == 0, "Read track count")
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(app.session, snapshot.track_count - 1, &track) == 0, "Read new ID")
        return track.id
    }
    func board() -> daw_clipboard_info {
        var info = daw_clipboard_info(); info.struct_size = UInt32(MemoryLayout<daw_clipboard_info>.size)
        expect(daw_get_clipboard(app.session, &info) == 0, "Read real clipboard"); return info
    }
    func audio(_ track: UInt64, _ index: UInt32 = 0) -> daw_clip {
        var clip = daw_clip(); clip.struct_size = UInt32(MemoryLayout<daw_clip>.size)
        expect(daw_get_clip(app.session, track, index, &clip) == 0, "Read audio"); return clip
    }
    func midi(_ track: UInt64, _ index: UInt32 = 0) -> daw_midi_clip {
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        expect(daw_get_midi_clip(app.session, track, index, &clip, 0, nil, 0, nil) == 0, "Read MIDI"); return clip
    }
    func count(_ track: UInt64) -> Int { editor.items.filter { $0.key.track == track }.count }
    func lane(_ track: UInt64) -> ArrangementEditingController.Lane { editor.lanes.first { $0.track == track }! }
    func focus(_ track: UInt64) {
        let view = lane(track).view; view.scrollToVisible(view.bounds)
        root.layoutSubtreeIfNeeded(); editor.focus(lane(track))
    }
    func key(_ code: UInt16, flags: NSEvent.ModifierFlags = [.command]) {
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, characters: "", charactersIgnoringModifiers: "",
            isARepeat: false, keyCode: code)!
        app.window.sendEvent(event)
    }
    func menuItem(_ action: Selector) -> NSMenuItem {
        func find(_ menu: NSMenu) -> NSMenuItem? {
            for item in menu.items {
                if item.action == action { return item }
                if let submenu = item.submenu, let result = find(submenu) { return result }
            }
            return nil
        }
        guard let item = NSApp.mainMenu.flatMap(find) else { fatalError("Missing clipboard menu") }
        item.menu?.update(); return item
    }
    func menu(_ action: Selector) {
        let item = menuItem(action); expect(item.isEnabled, "Native menu enables supported clipboard action")
        item.menu?.performActionForItem(at: item.menu!.index(of: item))
    }
    func click(_ track: UInt64, frame: UInt64, additive: Bool = false) {
        focus(track)
        let view = lane(track).view
        let point = view.convert(NSPoint(x: CGFloat(Double(frame) / Double(app.timelineRuler.projectFrames)) * view.bounds.width,
                                         y: view.bounds.midY), to: nil)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            app.window.sendEvent(NSEvent.mouseEvent(with: type, location: point, modifierFlags: additive ? [.control] : [],
                timestamp: 0, windowNumber: app.window.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!)
        }
    }
    guard let seed = editor.items.first(where: { $0.key.kind == .audio }) else { fatalError("Missing fixture audio") }
    let sourceA = addTrack("Clipboard audio source"), sourceGap = addTrack("Clipboard source gap"), sourceM = addTrack("Clipboard MIDI source")
    let targetA = addTrack("Clipboard audio target"), targetGap = addTrack("Clipboard target gap"), targetM = addTrack("Clipboard MIDI target")
    let last = addTrack("Clipboard insufficient target")
    expect(daw_copy_clip_to_track(app.session, seed.key.track, UInt32(seed.key.index), sourceA, 11000, revision()) == 0, "Seed audio")
    expect(daw_edit_clip_full(app.session, sourceA, 0, 11000, 1000, 10001, 64, 96, revision()) == 0, "Normalize audio")
    expect(daw_copy_clip_to_track(app.session, seed.key.track, UInt32(seed.key.index), last, 300000, revision()) == 0, "Seed incompatible later target")
    var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
    clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.start = 6000; clip.length = 8000; clip.note_count = 1
    var note = daw_midi_note(); note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
    note.version = UInt32(DAW_MIDI_NOTE_VERSION); note.start = 1000; note.length = 3000; note.pitch = 67; note.velocity = 91
    expect(daw_add_midi_clip(app.session, sourceM, &clip, &note, 1, revision()) == 0, "Seed MIDI")
    app.refresh(); app.setTimelineZoom(1); settle(); editor.selectTool(.pointer)
    editor.selection.removeAll(); editor.syncSelection()
    click(sourceA, frame: 16000); click(sourceM, frame: 10000, additive: true)
    expect(editor.selection.count == 2, "Real clicks create mixed selection across an unselected gap")
    let copiedAt = revision(); menu(#selector(DraftApp.menuCopyClip))
    expect(revision() == copiedAt && board().kind == 3 && board().clip_count == 2 && board().length == 15001, "Menu Copy captures whole mixed group without dirtying project")
    expect(editor.focusTrack == sourceA && Set(editor.clipboard!.entries.map(\.rowOffset)) == [0, 2], "Paste anchor is top row, gap is preserved")
    expect(daw_transpose_midi_clip(app.session, sourceM, 0, 12, revision()) == 0, "Edit original after Copy")
    expect(daw_remove_track(app.session, sourceM, revision()) == 0, "Delete original MIDI track")
    expect(daw_remove_track(app.session, sourceA, revision()) == 0, "Delete original audio track")
    app.refresh(); settle(); focus(targetA); app.seekAudio(0)
    let pastedAt = revision(); key(9); settle()
    expect(revision() == pastedAt + 1 && count(targetA) == 1 && count(targetM) == 1, "Cmd-V pastes whole mixed group in one transaction after source deletion")
    expect(audio(targetA).start == 5000 && midi(targetM).start == 0 && count(targetGap) == 0 && count(sourceGap) == 0, "Common time anchor preserves offsets and gaps")
    expect(audio(targetA).source_offset == 1000 && audio(targetA).fade_in == 64 && audio(targetA).fade_out == 96, "Audio source window and fades are unchanged")
    var readMeta = midi(targetM), readNote = daw_midi_note(), written: UInt32 = 0
    expect(daw_get_midi_clip(app.session, targetM, 0, &readMeta, 0, &readNote, 1, &written) == 0 &&
        written == 1 && readNote.pitch == 67, "Clipboard holds original MIDI, not edited notes")
    expect(editor.selection.count == 2 && app.playheadFrame == 15001, "All pasted clips selected; playhead reaches whole group end")
    menu(#selector(DraftApp.menuPasteClip)); settle()
    expect(audio(targetA, 1).start == 20001 && midi(targetM, 1).start == 15001 && app.playheadFrame == 30002, "Repeated menu Paste bypasses backward grid rounding")
    app.undo(); settle(); expect(count(targetA) == 1 && count(targetM) == 1 && board().kind == 3, "One Undo restores both tracks without clearing clipboard")
    app.redo(); settle(); expect(count(targetA) == 2 && count(targetM) == 2, "Redo restores whole group")
    focus(targetA); app.seekAudio(0); let rejectedAt = revision()
    key(9); settle()
    expect(revision() == rejectedAt && app.playheadFrame == 0 && count(targetA) == 2 && count(targetM) == 2 && board().kind == 3, "Failed overlap paste keeps whole project, playhead and board")
    focus(last)
    expect(!menuItem(#selector(DraftApp.menuPasteClip)).isEnabled, "Paste menu rejects insufficient target rows")
    key(9); expect(revision() == rejectedAt && board().clip_count == 2, "Keyboard rejection matches menu")
    focus(targetGap)
    expect(!menuItem(#selector(DraftApp.menuPasteClip)).isEnabled, "Paste menu checks later target types, not just first row")
    // Move the anchor to the MIDI track: the first destination is incompatible.
    focus(targetM); expect(!menuItem(#selector(DraftApp.menuPasteClip)).isEnabled, "Audio cannot paste into MIDI row")
    editor.selection.removeAll(); editor.syncSelection()
    click(targetA, frame: 10000); click(targetM, frame: 4000, additive: true)
    let cutAt = revision(); key(7); settle()
    expect(revision() == cutAt + 1 && count(targetA) == 1 && count(targetM) == 1 && board().clip_count == 2, "Cmd-X removes mixed originals immediately with one Undo")
    app.undo(); settle(); expect(count(targetA) == 2 && count(targetM) == 2 && board().kind == 3, "Undo Cut retains reusable mixed buffer")
    focus(targetA); let lockedAt = revision(); app.isRecording = true
    key(8); key(7); key(9)
    expect(revision() == lockedAt && board().kind == 3, "Copy/Cut/Paste respect recording lock")
    app.isRecording = false; app.midiTakeArmed = true; key(9)
    expect(revision() == lockedAt, "MIDI capture blocks paste")
    app.midiTakeArmed = false
    expect(daw_set_track_color(app.session, targetGap, 0x123456, revision()) == 0, "External revision change fixture")
    let staleAt = revision()
    app.copyArrangementClipboard(cut: true)
    expect(revision() == staleAt && board().kind == 3, "Stale menu callback never recaptures changed indices")
    app.refresh(); settle()
    let field = NSTextField(string: "artist name")
    field.frame = NSRect(x: 10, y: 10, width: 130, height: 24); root.addSubview(field)
    app.window.makeFirstResponder(field)
    let textAt = revision()
    expect(!menuItem(#selector(DraftApp.menuCutClip)).isEnabled, "Clip Cut is disabled while editing text")
    app.copyArrangementClipboard(cut: true)
    expect(revision() == textAt && board().kind == 3, "Text editing cannot cut the remembered arrangement selection")
    app.window.makeFirstResponder(nil); field.removeFromSuperview()
    let oldDocument = app.midiDocumentID; app.midiDocumentID = UUID(); editor.bindProjection()
    expect(editor.clipboard == nil && board().clip_count == 0, "Document replacement clears geometry and real mixed clipboard")
    app.midiDocumentID = oldDocument; editor.bindProjection()
    app.setTimelineZoom(1); app.restoreArrangementViewport(.zero)
    print("PASS: \(checks) native mixed clipboard assertions")
    return checks
}
