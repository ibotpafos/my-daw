import AppKit

/// Real window events and the production C ABI. No fake editing callbacks.
@MainActor
func runArrangementMixedGroupTests(_ app: DraftApp) -> Int {
    guard let editor = app.window.arrangementEditing as? ArrangementEditingController,
          let root = app.window.contentView else { fatalError("Group editor not mounted") }
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        if !value() { fatalError("Native mixed group: \(message)") }
    }
    func revision() -> UInt64 { editor.currentRevision()! }
    func settle() {
        root.layoutSubtreeIfNeeded(); app.fitTimelineViewport()
        root.layoutSubtreeIfNeeded(); editor.bindProjection()
    }
    func addTrack(_ title: String) -> UInt64 {
        expect(daw_add_track(app.session, title, revision()) == 0, "Add group fixture track")
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(app.session, &snapshot) == 0, "Read track count")
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(app.session, snapshot.track_count - 1, &track) == 0, "Read new track ID")
        return track.id
    }
    func midi(_ track: UInt64, _ index: UInt32 = 0) -> daw_midi_clip {
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        expect(daw_get_midi_clip(app.session, track, index, &clip, 0, nil, 0, nil) == 0, "Read MIDI geometry")
        return clip
    }
    func audio(_ track: UInt64, _ index: UInt32 = 0) -> daw_clip {
        var clip = daw_clip(); clip.struct_size = UInt32(MemoryLayout<daw_clip>.size)
        expect(daw_get_clip(app.session, track, index, &clip) == 0, "Read audio geometry")
        return clip
    }
    func count(_ track: UInt64, midi: Bool) -> Int {
        if midi {
            var count: UInt32 = 0
            expect(daw_get_midi_clip_count(app.session, track, &count) == 0, "Read MIDI count")
            return Int(count)
        }
        return editor.items.filter { $0.key.track == track && $0.key.kind == .audio }.count
    }
    func addMidi(_ track: UInt64, start: UInt64, length: UInt64) {
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.start = start; clip.length = length; clip.note_count = 1
        var note = daw_midi_note(); note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
        note.version = UInt32(DAW_MIDI_NOTE_VERSION); note.start = 1000; note.length = 6000
        note.pitch = 65; note.velocity = 88; note.channel = 3
        expect(daw_add_midi_clip(app.session, track, &clip, &note, 1, revision()) == 0, "Seed MIDI with sustained note")
    }
    guard let seed = editor.items.first(where: { $0.key.kind == .audio }) else { fatalError("Missing fixture audio") }
    let sourceA = addTrack("Group audio"), sourceM = addTrack("Group MIDI")
    let targetA = addTrack("Group audio destination"), targetM = addTrack("Group MIDI destination")
    let repeated = addTrack("Repeated paste off grid")
    expect(daw_copy_clip_to_track(app.session, seed.key.track, UInt32(seed.key.index), sourceA, 0, revision()) == 0, "Seed independent audio lane")
    expect(daw_edit_clip_full(app.session, sourceA, 0, 0, 0, 48000, 0, 0, revision()) == 0, "Normalize audio test region")
    addMidi(sourceM, start: 60000, length: 24000)
    addMidi(repeated, start: 0, length: 10001)
    app.refresh(); app.setTimelineZoom(1); settle(); editor.selectTool(.pointer)
    func lane(_ track: UInt64) -> ArrangementEditingController.Lane { editor.lanes.first { $0.track == track }! }
    func focus(_ track: UInt64) {
        let view = lane(track).view; view.scrollToVisible(view.bounds)
        root.layoutSubtreeIfNeeded(); editor.focus(lane(track))
    }
    func key(_ code: UInt16, flags: NSEvent.ModifierFlags = []) {
        let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, characters: "", charactersIgnoringModifiers: "",
            isARepeat: false, keyCode: code)!
        app.window.sendEvent(event)
    }
    func menu(_ action: Selector) {
        func find(_ menu: NSMenu) -> NSMenuItem? {
            for item in menu.items {
                if item.action == action { return item }
                if let submenu = item.submenu, let found = find(submenu) { return found }
            }
            return nil
        }
        guard let item = NSApp.mainMenu.flatMap(find), let parent = item.menu else { fatalError("Missing group menu action") }
        parent.update(); expect(item.isEnabled, "Group menu action enabled")
        parent.performActionForItem(at: parent.index(of: item))
    }
    func mouse(_ type: NSEvent.EventType, track: UInt64, frame: UInt64, flags: NSEvent.ModifierFlags = []) {
        let view = lane(track).view
        let point = view.convert(NSPoint(x: CGFloat(Double(frame) / Double(app.timelineRuler.projectFrames)) * view.bounds.width,
                                         y: view.bounds.midY), to: nil)
        let event = NSEvent.mouseEvent(with: type, location: point, modifierFlags: flags, timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!
        app.window.sendEvent(event)
    }
    func selectSources() {
        editor.selectTool(.pointer)
        // A blank click clears the old selection, then modified clicks build it.
        focus(sourceA); mouse(.leftMouseDown, track: sourceA, frame: 300000)
        mouse(.leftMouseUp, track: sourceA, frame: 300000)
        mouse(.leftMouseDown, track: sourceA, frame: 24000)
        mouse(.leftMouseUp, track: sourceA, frame: 24000)
        focus(sourceM); mouse(.leftMouseDown, track: sourceM, frame: 72000, flags: [.control])
        mouse(.leftMouseUp, track: sourceM, frame: 72000, flags: [.control])
        expect(editor.selection.count == 2, "Modified clicks select audio and MIDI across tracks")
    }
    selectSources(); let beforeNudge = revision()
    key(124, flags: [.option]); settle()
    let delta = audio(sourceA).start
    expect(delta > 0 && midi(sourceM).start == 60000 + delta && revision() == beforeNudge + 1, "Mixed nudge is one edit and preserves spacing")
    expect(editor.selection.count == 2, "Selection survives mixed nudge")
    app.undo(); settle(); expect(audio(sourceA).start == 0 && midi(sourceM).start == 60000, "One Undo restores both kinds")
    selectSources(); let beforeCopy = revision()
    key(2, flags: [.command]); settle()
    expect(count(sourceA, midi: false) == 2 && count(sourceM, midi: true) == 2, "Cmd-D duplicates entire mixed group")
    expect(audio(sourceA, 1).start == 84000 && midi(sourceM, 1).start == 144000 && revision() == beforeCopy + 1, "Group duplicates after full span")
    expect(editor.selection.count == 2, "Only newly duplicated clips selected")
    app.undo(); settle()
    selectSources(); let beforeMenuCopy = revision()
    menu(#selector(DraftApp.menuClipDuplicate)); settle()
    expect(revision() == beforeMenuCopy + 1 && count(sourceA, midi: false) == 2 && count(sourceM, midi: true) == 2, "Menu duplicate matches Cmd-D for mixed selection")
    app.undo(); settle()
    selectSources(); let beforeDelete = revision()
    menu(#selector(DraftApp.menuClipDelete)); settle()
    expect(count(sourceA, midi: false) == 0 && count(sourceM, midi: true) == 0 && revision() == beforeDelete + 1, "Delete removes mixed group atomically")
    app.undo(); settle(); expect(audio(sourceA).length == 48000 && midi(sourceM).length == 24000, "Delete Undo restores full group")

    selectSources(); focus(sourceA)
    let beforeDrag = revision()
    mouse(.leftMouseDown, track: sourceA, frame: 24000, flags: [.shift])
    mouse(.leftMouseDragged, track: targetA, frame: 120000, flags: [.shift])
    expect(editor.gesture?.dragged == true && editor.overlay.rectangles.count == 2 && revision() == beforeDrag, "Vertical mixed drag previews all clips without editing")
    mouse(.leftMouseUp, track: targetA, frame: 120000, flags: [.shift]); settle()
    expect(audio(targetA).start == 96000 && midi(targetM).start == 156000 && revision() == beforeDrag + 1, "Vertical group drag preserves relative track and time offsets")
    expect(count(sourceA, midi: false) == 0 && count(sourceM, midi: true) == 0 && editor.selection.count == 2, "Originals removed; moved group selected")
    app.undo(); settle()
    // Core must reject the second destination without leaving the first changed.
    addMidi(targetM, start: 156000, length: 24000); app.refresh(); settle(); selectSources(); focus(sourceA)
    let conflict = revision()
    mouse(.leftMouseDown, track: sourceA, frame: 24000, flags: [.shift])
    mouse(.leftMouseDragged, track: targetA, frame: 120000, flags: [.shift])
    mouse(.leftMouseUp, track: targetA, frame: 120000, flags: [.shift]); settle()
    expect(revision() == conflict && count(targetA, midi: false) == 0 && audio(sourceA).start == 0 && midi(sourceM).start == 60000, "Overlap on second track rolls back the entire gesture")
    expect(editor.selection.count == 2, "Rejected edit preserves source selection")
    selectSources(); let locked = revision(); app.isRecording = true
    key(51); expect(revision() == locked, "UI blocks mixed delete while recording")
    app.isRecording = false; app.midiTakeArmed = true
    key(2, flags: [.command]); expect(revision() == locked, "UI blocks mixed duplicate during MIDI capture")
    app.midiTakeArmed = false

    // Port the earlier COPY-02 continuation rule to the current value clipboard.
    focus(repeated); key(0, flags: [.command]); key(8, flags: [.command])
    app.seekAudio(10001); key(9, flags: [.command]); settle()
    let end = midi(repeated, 1).start + 10001
    expect(app.playheadFrame == end, "Paste moves insertion cursor to pasted end")
    key(9, flags: [.command]); settle()
    expect(count(repeated, midi: true) == 3 && midi(repeated, 2).start == end, "Repeated paste bypasses rounding backwards on non-grid length")
    // Scissors through a sustained note reach the revised model contract.
    editor.selectTool(.split); focus(sourceM)
    let beforeSplit = revision()
    mouse(.leftMouseDown, track: sourceM, frame: 63000, flags: [.shift]); settle()
    expect(count(sourceM, midi: true) == 2 && revision() == beforeSplit + 1, "Scissors split through held MIDI note")
    var left = daw_midi_clip(); left.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
    var note = daw_midi_note(), written: UInt32 = 0
    expect(daw_get_midi_clip(app.session, sourceM, 0, &left, 0, &note, 1, &written) == 0 && written == 1 && note.length == 2000, "Left note portion preserved")
    expect(daw_get_midi_clip(app.session, sourceM, 1, &left, 0, &note, 1, &written) == 0 && written == 1 && note.start == 0 && note.length == 4000 && note.pitch == 65, "Right note retriggers with preserved attributes")
    app.undo(); settle(); expect(count(sourceM, midi: true) == 1, "Split Undo rejoins original note")
    editor.selectTool(.pointer); focus(sourceA); key(0, flags: [.command, .shift])
    expect(editor.selection.count == editor.items.count, "Cmd-Shift-A selects project clips")
    editor.selection.removeAll(); editor.syncSelection()
    app.setTimelineZoom(1); app.restoreArrangementViewport(.zero)
    print("PASS: \(checks) native mixed-selection and continuous-paste assertions")
    return checks
}
