import AppKit

@MainActor
func runArrangementClipboardTests(_ app: DraftApp) -> Int {
    guard let editor = app.window.arrangementEditing as? ArrangementEditingController,
          let root = app.window.contentView else { fatalError("Clipboard editor not mounted") }
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        if !condition() { fatalError("Native clipboard: \(message)") }
    }
    func revision() -> UInt64 { editor.currentRevision()! }
    func settle() {
        root.layoutSubtreeIfNeeded(); app.fitTimelineViewport()
        root.layoutSubtreeIfNeeded(); editor.bindProjection()
    }
    func addTrack(_ title: String) -> UInt64 {
        expect(daw_add_track(app.session, title, revision()) == 0, "Create clipboard fixture track")
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(app.session, &snapshot) == 0, "Read fixture snapshot")
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(app.session, snapshot.track_count - 1, &track) == 0, "Read fixture track")
        return track.id
    }
    let source = addTrack("Clipboard source"), target = addTrack("Clipboard target"), cutTarget = addTrack("Cut target")
    for start in [UInt64(0), 96000] {
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.start = start; clip.length = 48000
        clip.lane = 4; clip.color = 0xAA77CC; clip.note_count = 1
        var note = daw_midi_note(); note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
        note.version = UInt32(DAW_MIDI_NOTE_VERSION); note.start = 6000; note.length = 12000
        note.pitch = 65; note.channel = 3; note.velocity = 81
        expect(daw_add_midi_clip(app.session, source, &clip, &note, 1, revision()) == 0, "Seed clipboard MIDI")
    }
    app.refresh(); app.setTimelineZoom(1); settle()
    func lane(_ track: UInt64) -> ArrangementEditingController.Lane {
        editor.lanes.first { $0.track == track }!
    }
    func focus(_ track: UInt64) {
        let view = lane(track).view
        view.scrollToVisible(view.bounds); root.layoutSubtreeIfNeeded(); editor.focus(lane(track))
    }
    func key(_ code: UInt16, _ modifiers: NSEvent.ModifierFlags = [.command]) {
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: modifiers, timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, characters: "", charactersIgnoringModifiers: "",
            isARepeat: false, keyCode: code)!
        app.window.sendEvent(event)
    }
    func menu(_ action: Selector) {
        func locate(_ menu: NSMenu) -> NSMenuItem? {
            for item in menu.items {
                if item.action == action { return item }
                if let submenu = item.submenu, let found = locate(submenu) { return found }
            }
            return nil
        }
        guard let item = NSApp.mainMenu.flatMap(locate), let parent = item.menu else { fatalError("Missing clipboard menu action") }
        parent.update()
        expect(item.isEnabled, "Clipboard menu action enabled for current focus")
        parent.performActionForItem(at: parent.index(of: item))
    }
    func count(_ track: UInt64) -> UInt32 {
        var value: UInt32 = 0
        expect(daw_get_midi_clip_count(app.session, track, &value) == 0, "Read actual clip count")
        return value
    }
    func board() -> daw_clipboard_info {
        var info = daw_clipboard_info(); info.struct_size = UInt32(MemoryLayout<daw_clipboard_info>.size)
        expect(daw_get_clipboard(app.session, &info) == 0, "Read actual session clipboard")
        return info
    }
    editor.selectTool(.pointer); focus(source); key(0)
    expect(editor.selection.count == 2, "Cmd-A selects MIDI group")
    let beforeCopy = revision(); menu(#selector(DraftApp.menuCopyClip))
    expect(revision() == beforeCopy && board().clip_count == 2, "Menu copy is read-only and stores the full MIDI group")
    expect(daw_set_midi_clip_color(app.session, source, 0, 0x123456, revision()) == 0, "Change original after copying")
    expect(daw_transpose_midi_clip(app.session, source, 0, 12, revision()) == 0, "Change original notes after copying")
    expect(daw_remove_midi_clip(app.session, source, 1, revision()) == 0, "Remove second original")
    expect(daw_remove_midi_clip(app.session, source, 0, revision()) == 0, "Remove final original")
    app.refresh(); settle(); focus(target); app.seekAudio(0)
    let firstPaste = revision(); key(9); settle()
    expect(count(target) == 2 && revision() == firstPaste + 1, "Keyboard paste survives source deletion and is one transaction")
    var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
    var note = daw_midi_note(), written: UInt32 = 0
    expect(daw_get_midi_clip(app.session, target, 0, &clip, 0, &note, 1, &written) == 0, "Read pasted notes")
    expect(written == 1 && note.pitch == 65 && note.channel == 3 && note.velocity == 81 &&
        clip.color == 0xAA77CC && clip.lane == 4, "Pasted notes and clip color are from capture, not edited originals")
    expect(board().clip_count == 2 && editor.selection.count == 2, "Clipboard and pasted selection survive refresh")
    app.seekAudio(192000); focus(target)
    let secondPaste = revision(); menu(#selector(DraftApp.menuPasteClip)); settle()
    expect(count(target) == 4 && revision() == secondPaste + 1, "Menu paste uses the same reusable clipboard")
    app.undo(); settle(); expect(count(target) == 2 && board().clip_count == 2, "Undo paste keeps clipboard")
    app.redo(); settle(); expect(count(target) == 4, "Redo restores complete pasted group")
    focus(target); app.seekAudio(192000)
    let failedPaste = revision(); key(9); settle()
    expect(count(target) == 4 && revision() == failedPaste && board().clip_count == 2, "Overlap rejection is atomic and preserves buffer")

    focus(target); key(0)
    let cutRevision = revision(); key(7); settle()
    expect(count(target) == 0 && revision() == cutRevision + 1 && board().clip_count == 4, "Cmd-X removes group immediately in one Undo step")
    app.undo(); settle(); expect(count(target) == 4 && board().clip_count == 4, "Undo cut restores all originals but keeps value buffer")
    focus(cutTarget); app.seekAudio(0)
    let cutPaste = revision(); key(9); settle()
    expect(count(cutTarget) == 4 && revision() == cutPaste + 1, "Paste after Undo cut does not move or delete originals")
    expect(count(target) == 4, "Clipboard has no source index references")

    // Same menu route with an audio projection: cut may now empty the lane,
    // without losing its sources or erasing its track/inserts/routing.
    let audioLane = editor.lanes.first { $0.kind == .audio }!
    let audioID = audioLane.track
    let beforeAudio = editor.items.filter { $0.key.track == audioID }
    focus(audioID); key(0); key(7); settle()
    expect(editor.items.filter { $0.key.track == audioID }.isEmpty, "Cut can empty an audio lane")
    expect(lane(audioID).kind == .audio && board().kind == 1, "Empty audio lane retains its media type and captured sources")
    app.undo(); settle()
    expect(editor.items.filter { $0.key.track == audioID }.count == beforeAudio.count, "Undo restores the full audio selection")
    expect(editor.clipboard?.kind == .audio && board().clip_count == UInt32(beforeAudio.count), "Audio clipboard survives Undo")
    let oldDocument = app.midiDocumentID; app.midiDocumentID = UUID(); editor.bindProjection()
    expect(board().clip_count == 0 && editor.clipboard == nil, "New document clears both Swift metadata and core clipboard")
    app.midiDocumentID = oldDocument; editor.bindProjection()
    app.setTimelineZoom(1); app.restoreArrangementViewport(.zero)
    print("PASS: \(checks) native value clipboard assertions")
    return checks
}
