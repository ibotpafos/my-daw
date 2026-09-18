import AppKit

/// Real AppKit events -> the production window/controller -> the real C ABI.
/// Does not require an audio device and does not stand in for a listening test.
@MainActor
func runArrangementEditingTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ text: String) {
        checks += 1
        if !condition() { fatalError("Arrangement editing: \(text)") }
    }
    guard let editor = app.window.arrangementEditing as? ArrangementEditingController, let root = app.window.contentView else { fatalError("Missing mounted editor") }
    func revision() -> UInt64 { editor.currentRevision()! }
    func settle() {
        for _ in 0..<3 { root.layoutSubtreeIfNeeded(); app.fitTimelineViewport() }
        root.layoutSubtreeIfNeeded(); editor.bindProjection()
    }
    func addTrack(_ name: String) -> UInt64 {
        expect(daw_add_track(app.session, name, revision()) == 0, "Create test MIDI track")
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(app.session, &snapshot) == 0, "Read snapshot")
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(app.session, snapshot.track_count - 1, &track) == 0, "Read new track")
        return track.id
    }
    let track = addTrack("Arrangement test MIDI"), emptyTrack = addTrack("Arrangement empty target")
    func meta(_ index: UInt32 = 0, trackID: UInt64? = nil) -> daw_midi_clip {
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        expect(daw_get_midi_clip(app.session, trackID ?? track, index, &clip, 0, nil, 0, nil) == 0, "Read actual MIDI clip")
        return clip
    }
    var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
    clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.start = 48_000; clip.length = 96_000; clip.lane = 0; clip.note_count = 1
    var note = daw_midi_note(); note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size); note.version = UInt32(DAW_MIDI_NOTE_VERSION); note.start = 24_000; note.length = 12_000; note.pitch = 60; note.velocity = 100
    expect(daw_add_midi_clip(app.session, track, &clip, &note, 1, revision()) == 0, "Seed MIDI through ABI")
    app.refresh(); app.setTimelineZoom(1); settle()
    func lane(_ id: UInt64 = 0) -> ArrangementEditingController.Lane {
        guard let lane = editor.lanes.first(where: { $0.track == (id == 0 ? track : id) }) else { fatalError("Missing projected lane") }
        return lane
    }
    func visible(_ id: UInt64 = 0) {
        lane(id).view.scrollToVisible(lane(id).view.bounds); root.layoutSubtreeIfNeeded()
        editor.focus(lane(id))
    }
    func mouse(_ type: NSEvent.EventType, frame: UInt64, trackID: UInt64 = 0,
               flags: NSEvent.ModifierFlags = [], clicks: Int = 1) -> NSEvent {
        let lane = lane(trackID)
        let point = lane.view.convert(NSPoint(x: CGFloat(Double(frame) / Double(app.timelineRuler.projectFrames)) * lane.view.bounds.width,
                                             y: lane.view.bounds.midY), to: nil)
        return NSEvent.mouseEvent(with: type, location: point, modifierFlags: flags, timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, eventNumber: 0, clickCount: clicks, pressure: 1)!
    }
    func key(_ code: UInt16, flags: NSEvent.ModifierFlags = [], text: String = "") -> NSEvent {
        NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, characters: text,
            charactersIgnoringModifiers: text, isARepeat: false, keyCode: code)!
    }
    func send(_ event: NSEvent) { app.window.sendEvent(event) }
    func selectFirst() {
        editor.selectTool(.pointer); visible()
        let clip = meta()
        send(mouse(.leftMouseDown, frame: clip.start + clip.length / 2))
        send(mouse(.leftMouseUp, frame: clip.start + clip.length / 2))
    }
    visible()
    let initial = revision()
    for (code, tool) in [(UInt16(18),ArrangementTool.pointer),(19,.range),(20,.split),(21,.erase),(23,.draw),
                          (22,.mute),(26,.hand),(28,.zoom),(25,.fade)] {
        send(key(code, text: "я")); expect(editor.tool == tool, "Tool selected by physical digit on Russian layout")
    }
    expect(revision() == initial, "Tool changes are not project edits")
    let field = NSTextField(string: "")
    field.frame = NSRect(x: 10, y: 10, width: 100, height: 24); root.addSubview(field)
    app.window.makeFirstResponder(field)
    send(key(18, text: "1")); expect(editor.tool == .fade, "Text input does not switch tool")
    app.window.makeFirstResponder(nil); field.removeFromSuperview()
    selectFirst()
    expect(editor.selection.count == 1, "Real clip click selects one MIDI region")
    expect((lane().view as? MidiArrangementView)?.selectedIndices == [0], "Selected projection is visible")
    expect(revision() == initial, "Selection is not a project edit")

    // Escape and click/no movement do not spend a revision.
    send(mouse(.leftMouseDown, frame: 96_000)); send(mouse(.leftMouseDragged, frame: 144_000, flags: [.shift]))
    expect(editor.gesture?.dragged == true && !editor.overlay.rectangles.isEmpty, "Real drag produces ghost preview")
    expect(revision() == initial && meta().start == 48_000, "Preview does not touch the model")
    send(key(53)); send(mouse(.leftMouseUp, frame: 144_000))
    expect(editor.gesture == nil && editor.overlay.rectangles.isEmpty && meta().start == 48_000, "Escape clears preview, late release cannot commit")
    expect(revision() == initial, "Escape has no undo entry")

    send(mouse(.leftMouseDown, frame: 96_000, flags: [.shift]))
    send(mouse(.leftMouseDragged, frame: 144_000, flags: [.shift]))
    send(mouse(.leftMouseUp, frame: 144_000, flags: [.shift]))
    settle()
    expect(meta().start == 96_000 && meta().length == 96_000, "MIDI body drag reaches public move command")
    expect(revision() == initial + 1, "One drag, one revision")
    expect(editor.selection.count == 1, "Selection survives projection rebuilding")
    var readNote = daw_midi_note(), written: UInt32 = 0, readMeta = meta()
    expect(daw_get_midi_clip(app.session, track, 0, &readMeta, 0, &readNote, 1, &written) == 0 &&
           written == 1 && readNote.start == 24_000 && readNote.pitch == 60, "Move preserves clip-relative note")
    app.undo(); settle(); expect(meta().start == 48_000, "Actual Undo restores move")
    app.redo(); settle(); expect(meta().start == 96_000, "Actual Redo restores move")

    selectFirst()
    let beforeCopy = revision()
    send(mouse(.leftMouseDown, frame: 144_000, flags: [.option,.shift]))
    send(mouse(.leftMouseDragged, frame: 288_000, flags: [.option,.shift]))
    send(mouse(.leftMouseUp, frame: 288_000, flags: [.option,.shift]))
    settle()
    var count: UInt32 = 0
    expect(daw_get_midi_clip_count(app.session, track, &count) == 0 && count == 2, "Option drag copies MIDI clip")
    expect(meta(1).start == 240_000 && meta().start == 96_000, "Copy preserves original and uses drop position")
    expect(revision() == beforeCopy + 1, "Option copy is one undo step")

    selectFirst(); let beforeTrim = revision()
    send(mouse(.leftMouseDown, frame: 191_500, flags: [.shift]))
    send(mouse(.leftMouseDragged, frame: 180_000, flags: [.shift]))
    send(mouse(.leftMouseUp, frame: 180_000, flags: [.shift])); settle()
    expect(meta().length == 84_000 && revision() == beforeTrim + 1, "MIDI right edge trim commits")
    app.undo(); settle()

    selectFirst(); let beforeSplit = revision()
    send(key(20)); send(mouse(.leftMouseDown, frame: 144_000, flags: [.shift]))
    settle()
    expect(daw_get_midi_clip_count(app.session, track, &count) == 0 && count == 3, "Scissors uses click position")
    expect(revision() == beforeSplit + 1, "Split is one undo step")
    app.undo(); settle()
    selectFirst(); send(key(8, flags: [.command])); expect(editor.clipboard != nil, "Cmd-C uses focused MIDI, not stale audio selection")
    let beforeStale = revision()
    expect(daw_set_midi_clip_color(app.session, track, 0, 0xAA33FF, beforeStale) == 0, "External edit invalidates clip index reference")
    // A captured value survives the source change, but a stale UI projection
    // still cannot commit until it is refreshed.
    send(key(9, flags: [.command])); expect(revision() == beforeStale + 1 && editor.clipboard != nil, "Value clipboard survives a source edit without a stale commit")
    app.refresh(); settle()

    selectFirst(); let beforeStaleDuplicate = revision()
    expect(daw_set_midi_clip_color(app.session, track, 0, 0x55AADD, beforeStaleDuplicate) == 0, "External edit before duplicate")
    send(key(2, flags: [.command]))
    expect(revision() == beforeStaleDuplicate + 1, "Duplicate rejects stale projection without another revision")
    settle()

    selectFirst(); send(mouse(.leftMouseDown, frame: 144_000)); send(mouse(.leftMouseDragged, frame: 168_000))
    let beforeConflict = revision()
    expect(daw_set_midi_clip_color(app.session, track, 0, 0x33AAFF, beforeConflict) == 0, "Mutate during gesture fixture")
    send(mouse(.leftMouseUp, frame: 168_000)); expect(revision() == beforeConflict + 1 && editor.gesture == nil, "Stale gesture cannot overwrite newer project")
    app.refresh(); settle()

    selectFirst(); let locked = revision(); app.isRecording = true
    send(key(21)); send(mouse(.leftMouseDown, frame: 144_000)); expect(revision() == locked, "Recording blocks eraser")
    app.isRecording = false; app.midiTakeArmed = true
    send(key(51)); expect(revision() == locked, "MIDI capture blocks delete")
    app.midiTakeArmed = false

    selectFirst(); let navRevision = revision()
    send(key(6)); expect(app.timelineZoom > 1, "Z fits selected clip")
    send(key(6, flags: [.shift])); expect(app.timelineZoom == 1, "Shift-Z fits whole project")
    expect(revision() == navRevision, "Navigation does not dirty project")
    app.seekAudio(0); send(key(48)); expect(app.playheadFrame > 0, "Tab navigates clip boundaries")

    visible(emptyTrack); editor.selectTool(.draw)
    let beforeDraw = revision()
    send(mouse(.leftMouseDown, frame: 400_000, trackID: emptyTrack, flags: [.shift]))
    send(mouse(.leftMouseDragged, frame: 460_000, trackID: emptyTrack, flags: [.shift]))
    send(mouse(.leftMouseUp, frame: 460_000, trackID: emptyTrack, flags: [.shift])); settle()
    expect(meta(trackID: emptyTrack).start == 400_000 && meta(trackID: emptyTrack).length == 60_000, "Pencil creates MIDI in empty track")
    expect(revision() == beforeDraw + 1, "MIDI creation is one undo step")
    editor.selectTool(.erase); visible(emptyTrack)
    send(mouse(.leftMouseDown, frame: 430_000, trackID: emptyTrack)); settle()
    expect(daw_get_midi_clip_count(app.session, emptyTrack, &count) == 0 && count == 0, "Eraser removes last MIDI clip")
    app.undo(); settle(); expect(meta(trackID: emptyTrack).start == 400_000, "Undo restores erased MIDI clip")

    // Use the fixture's real audio clips: no simulated gesture callbacks.
    guard let audioTrack = editor.lanes.first(where: { $0.kind == .audio })?.track else { fatalError("Missing audio fixture") }
    func audioMeta(_ index: UInt32 = 0) -> daw_clip {
        var value = daw_clip(); value.struct_size = UInt32(MemoryLayout<daw_clip>.size)
        expect(daw_get_clip(app.session, audioTrack, index, &value) == 0, "Read actual audio clip")
        return value
    }
    func audioClick(_ frame: UInt64) {
        visible(audioTrack)
        send(mouse(.leftMouseDown, frame: frame, trackID: audioTrack, flags: [.shift]))
        send(mouse(.leftMouseUp, frame: frame, trackID: audioTrack, flags: [.shift]))
    }
    let audio = audioMeta(), audioFrame = audio.start + audio.length / 4
    editor.selectTool(.pointer); audioClick(audioFrame)
    expect(editor.selection.count == 1 && editor.selection.first?.kind == .audio, "Audio selection has correct kind")
    let beforeMute = revision()
    editor.selectTool(.mute); audioClick(audioFrame); settle()
    expect(audioMeta().muted != audio.muted && revision() == beforeMute + 1, "Audio mute commits once")
    app.undo(); settle(); expect(audioMeta().muted == audio.muted, "Undo restores mute")

    editor.selectTool(.fade); visible(audioTrack)
    let beforeFade = revision(), fadeEnd = audio.start + audio.length / 3
    send(mouse(.leftMouseDown, frame: audioFrame, trackID: audioTrack, flags: [.shift]))
    send(mouse(.leftMouseDragged, frame: fadeEnd, trackID: audioTrack, flags: [.shift]))
    expect(!editor.overlay.ramps.isEmpty && revision() == beforeFade, "Audio fade has a visual-only ramp preview")
    send(mouse(.leftMouseUp, frame: fadeEnd, trackID: audioTrack, flags: [.shift])); settle()
    expect(audioMeta().fade_in == audio.length / 3 && revision() == beforeFade + 1, "Audio fade commits one edit")
    app.undo(); settle(); expect(audioMeta().fade_in == audio.fade_in, "Undo restores fade")

    editor.selectTool(.pointer); audioClick(audioFrame)
    send(key(0, flags: [.command]))
    let selectedAudio = editor.items.filter { editor.selection.contains($0.key) }
    expect(selectedAudio.count >= 2 && selectedAudio.allSatisfy { $0.key.track == audioTrack }, "Cmd-A selects the focused audio track")
    let beforeNudge = revision()
    send(key(124, flags: [.option])); settle()
    let shifted = audioMeta().start - audio.start
    expect(shifted > 0 && revision() == beforeNudge + 1, "Audio group nudge uses one transaction")
    for item in selectedAudio {
        expect(audioMeta(UInt32(item.key.index)).start == item.bounds.start + shifted, "Group spacing is preserved")
    }
    app.undo(); settle(); expect(audioMeta().start == audio.start, "Undo restores audio group")

    editor.selectTool(.split); visible(audioTrack)
    let beforeAudioSplit = revision()
    let originalCount = editor.items.filter { $0.key.track == audioTrack }.count
    audioClick(audioFrame); settle()
    expect(editor.items.filter { $0.key.track == audioTrack }.count == originalCount + 1 && revision() == beforeAudioSplit + 1, "Audio scissors split at the clicked position")
    app.undo(); settle()
    editor.selectTool(.erase); audioClick(audioFrame); settle()
    expect(editor.items.filter { $0.key.track == audioTrack }.count == originalCount - 1, "Audio eraser deletes the hit clip")
    app.undo(); settle(); expect(audioMeta().length == audio.length, "Undo restores erased audio clip")

    // Document identity invalidates clipboard and gesture even at an equal revision.
    selectFirst(); send(key(8, flags: [.command])); let oldDocument = app.midiDocumentID
    app.midiDocumentID = UUID(); editor.bindProjection()
    expect(editor.clipboard == nil && editor.selection.isEmpty, "New project clears document-local editing state")
    app.midiDocumentID = oldDocument; editor.bindProjection()
    editor.selectTool(.pointer); app.setTimelineZoom(1); app.restoreArrangementViewport(.zero)
    checks += runArrangementClipboardTests(app)
    print("PASS: \(checks) native arrangement editing assertions")
    return checks
}
