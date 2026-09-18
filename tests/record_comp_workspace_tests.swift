import AppKit

/// Real AppKit events -> application command -> C ABI -> persisted audio regions.
@MainActor
func runRecordCompWorkspaceTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ message: String) {
        checks += 1; precondition(value(), "Record workspace: " + message)
    }
    guard let session = app.session, let workspace = app.workspace,
          let root = app.window.contentView else { fatalError("Missing actual application") }
    let view = app.recordingWorkspace
    let temporary = FileManager.default.temporaryDirectory.appendingPathComponent("comp-ui-\(UUID().uuidString)")
    try! FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: temporary) }
    func revision() -> UInt64 {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &snapshot) == 0, "read revision"); return snapshot.revision
    }
    func settle(_ width: CGFloat = 1536, _ height: CGFloat = 1000) {
        app.window.setContentSize(NSSize(width: width, height: height))
        for _ in 0..<4 { root.layoutSubtreeIfNeeded(); workspace.applyGeometry() }
    }
    func saveScreenshot(_ name: String) {
        guard let bitmap = root.bitmapImageRepForCachingDisplay(in: root.bounds) else { fatalError("No bitmap") }
        root.cacheDisplay(in: root.bounds, to: bitmap)
        try! bitmap.representation(using: .png, properties: [:])!.write(to:
            URL(fileURLWithPath: "build/workspace-ui/\(name).png"))
    }
    // Four seconds of generated PCM, imported only by this test executable.
    let wav = temporary.appendingPathComponent("vocal.wav")
    var bytes = Data()
    func tag(_ text: String) { bytes.append(contentsOf: text.utf8) }
    func put(_ value: UInt32, _ count: Int) {
        for byte in 0..<count { bytes.append(UInt8(truncatingIfNeeded: value >> (byte * 8))) }
    }
    let frames = 48000 * 4
    tag("RIFF"); put(UInt32(36 + frames * 2), 4); tag("WAVEfmt ")
    put(16, 4); put(1, 2); put(1, 2); put(48000, 4); put(96000, 4); put(2, 2); put(16, 2)
    tag("data"); put(UInt32(frames * 2), 4)
    for frame in 0..<frames {
        let envelope = 0.15 + 0.7 * pow(sin(Double(frame) / 9000), 2)
        put(UInt32(truncatingIfNeeded: Int32(sin(Double(frame) / 38) * envelope * 24000)), 2)
    }
    try! bytes.write(to: wav)
    expect(daw_import_wav(session, wav.path, "Vocal Comp Test", revision()) == 0, "import base audio")
    app.refresh()
    let index = app.trackIDs.keys.max()!
    let id = app.trackIDs[index]!
    expect(daw_import_take_wav(session, id, wav.path, "Take 2 · Late start", 48000, revision()) == 0, "import offset take")
    expect(daw_import_take_wav(session, id, wav.path, "Take 3 · Alternative", 0, revision()) == 0, "import third take")
    app.selectedMixerID = id; app.refresh(); workspace.present(.recording); settle()
    expect(view.trackID == id && view.snapshots.count == 3 && view.lanes.count == 4, "real sources and comp")
    expect(view.editable && view.apply.isEnabled, "actual comp actions available")
    func clips() -> [daw_clip] {
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(session, UInt32(index), &track) == 0, "read track")
        return (0..<track.clip_count).map { i in
            var clip = daw_clip(); clip.struct_size = UInt32(MemoryLayout<daw_clip>.size)
            expect(daw_get_clip(session, id, i, &clip) == 0, "read actual comp region"); return clip
        }
    }
    func lane(_ take: UInt32 = 1) -> RecordTakeLaneView { view.lanes.first(where: { $0.take?.index == take })! }
    func mouse(_ kind: NSEvent.EventType, _ lane: RecordTakeLaneView, _ frame: UInt64) -> NSEvent {
        let point = lane.convert(NSPoint(x: lane.x(for: frame), y: lane.timelineRect.midY), to: nil)
        return NSEvent.mouseEvent(with: kind, location: point, modifierFlags: [], timestamp: 0,
            windowNumber: app.window.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!
    }
    let beforeSwipe = revision()
    var source = lane()
    source.mouseDown(with: mouse(.leftMouseDown, source, 72000))
    source.mouseDragged(with: mouse(.leftMouseDragged, source, 144000))
    expect(view.isGesturing && revision() == beforeSwipe, "drag previews without editing project")
    expect(app.mixExportPolicy().block == .gesture, "export cannot open during a comp swipe")
    expect(!workspace.present(.mixer) && workspace.screen == .recording, "unfinished swipe prevents navigation")
    source.mouseUp(with: mouse(.leftMouseUp, source, 144000)); settle()
    expect(!view.isGesturing && revision() == beforeSwipe + 1, "mouse-up commits one revision")
    let result = clips()
    expect(result.count == 3, "comp splits only the selected phrase")
    expect(result[1].take_index == 1 && result[1].start == 72000 && result[1].length == 72000,
           "source take and exact timeline range retained")
    expect(result[1].source_offset == 24000, "late take uses source-relative offset")
    source.mouseUp(with: mouse(.leftMouseUp, source, 144000))
    expect(revision() == beforeSwipe + 1, "duplicate mouse-up cannot commit twice")
    app.undo(); settle(); expect(clips().count == 1 && clips()[0].take_index == 0, "one Undo restores base")
    app.redo(); settle(); expect(clips().count == 3 && clips()[1].take_index == 1, "Redo restores comp")
    source = lane()
    let beforeCancel = revision()
    source.mouseDown(with: mouse(.leftMouseDown, source, 72000))
    source.mouseDragged(with: mouse(.leftMouseDragged, source, 180000))
    let escape = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
        windowNumber: app.window.windowNumber, context: nil, characters: "\u{1b}",
        charactersIgnoringModifiers: "\u{1b}", isARepeat: false, keyCode: 53)!
    source.keyDown(with: escape)
    source.mouseUp(with: mouse(.leftMouseUp, source, 180000))
    expect(!view.isGesturing && revision() == beforeCancel, "Escape cancels without a project edit")
    source.mouseDown(with: mouse(.leftMouseDown, source, 72000))
    source.mouseUp(with: mouse(.leftMouseUp, source, 72000))
    expect(revision() == beforeCancel, "click only selects a take")
    view.selectTake(2); view.start.stringValue = "0,25"; view.end.stringValue = "0.75"
    view.apply.performClick(nil); settle()
    expect(revision() == beforeCancel + 1, "native numeric comp button dispatches")
    expect(clips().contains { $0.take_index == 2 && $0.start == 12000 && $0.length == 24000 }, "comma seconds reach sample-accurate region")
    let invalidRevision = revision()
    view.start.stringValue = "-1"; view.apply.performClick(nil)
    view.start.stringValue = "nan"; view.apply.performClick(nil)
    view.start.stringValue = "0"; view.end.stringValue = "500"; view.apply.performClick(nil)
    expect(revision() == invalidRevision, "invalid/outside source ranges do not commit")
    view.start.stringValue = "1.25"; view.end.stringValue = "2.0"; view.selectTake(1)
    let stale = view.requestFromFields()!
    expect(daw_rename_track(session, id, "Vocal Comp Updated", revision()) == 0, "external revision change")
    let afterExternal = revision()
    app.commitRecordingComp(stale)
    expect(revision() == afterExternal && !view.isGesturing, "stale request rejected without mutation")
    expect(view.revision == afterExternal, "stale rejection reloads authoritative revision")
    let current = view.requestFromFields()!
    app.midiTakeArmed = true; app.updateRecordingWorkspaceRuntime()
    expect(!view.editable && !view.apply.isEnabled, "MIDI capture blocks comp controls")
    app.commitRecordingComp(current); expect(revision() == afterExternal, "capture blocks stale callbacks too")
    app.midiTakeArmed = false; app.updateRecordingWorkspaceRuntime()
    view.zoom.performClick(nil)
    expect(view.lanes.allSatisfy { $0.viewStart == 60000 && $0.viewEnd == 96000 }, "zoom uses common phrase interval")
    view.fit.performClick(nil)
    expect(view.lanes.allSatisfy { $0.viewStart == 0 && $0.viewEnd == 240000 }, "fit includes offset takes")
    let path = temporary.appendingPathComponent("comp.mydawdraft")
    let expectedClips = clips().map { [$0.start, $0.source_offset, $0.length, UInt64($0.take_index)] }
    expect(daw_save_draft(session, path.path) == 0, "save real comp and source takes")
    let pending = view.requestFromFields()!
    let document = app.midiDocumentID, beforeOpen = revision()
    source = lane()
    source.mouseDown(with: mouse(.leftMouseDown, source, 72000))
    source.mouseDragged(with: mouse(.leftMouseDragged, source, 96000))
    app.openDraftFile(path); settle()
    expect(app.midiDocumentID != document && revision() == beforeOpen, "same-revision reopen changes document identity")
    expect(!view.isGesturing, "opening another document cancels the old swipe")
    source.mouseUp(with: mouse(.leftMouseUp, source, 96000))
    app.commitRecordingComp(pending)
    expect(revision() == beforeOpen, "delayed comp callbacks cannot edit a reopened document")
    expect(clips().map { [$0.start, $0.source_offset, $0.length, UInt64($0.take_index)] } == expectedClips,
           "save/open preserves comp source mapping")
    expect(view.snapshots.count == 3, "save/open preserves original takes")
    for (width, height) in [(CGFloat(1060), CGFloat(700)), (1536, 1000), (1920, 1080)] {
        settle(width, height)
        expect(view.scroll.frame.width > 500, "source lanes have usable width")
        let compFrame = view.lanes[0].convert(view.lanes[0].bounds, to: view)
        expect(abs(compFrame.minY - view.scroll.frame.minY) < 2, "comp starts at top, not bottom of viewport")
        for button in [view.record, view.monitor, view.settings, view.apply] {
            let frame = button.convert(button.bounds, to: view)
            expect(frame.minY >= 0 && frame.maxY <= view.bounds.height + 1, "primary controls inside smallest window")
        }
        saveScreenshot("record-comping-\(Int(width))")
    }
    let priorArm = app.armedTrackID
    app.armedTrackID = nil; app.updateRecordingWorkspaceRuntime()
    expect(view.recordTarget.stringValue.contains("новая дорожка"), "unarmed recording destination explicit")
    expect(!view.loop.isEnabled, "loop recording requires an armed target")
    app.armedTrackID = id; app.updateRecordingWorkspaceRuntime()
    expect(view.recordTarget.stringValue.contains("Vocal Comp Updated"), "actual armed destination shown")
    expect(view.loop.isEnabled, "armed audio target exposes loop recording")
    let beforeLoop = revision()
    if app.loopEnabled { view.loop.performClick(nil) }
    view.loop.performClick(nil)
    expect(app.loopEnabled && view.loop.state == .on, "native cycle control sets actual transport")
    view.loop.performClick(nil)
    expect(!app.loopEnabled && revision() == beforeLoop, "cycle toggle is not a project edit")
    app.armedTrackID = priorArm; app.updateRecordingWorkspaceRuntime()
    let finalRevision = revision()
    let layout = workspace.preference
    workspace.present(.mixer); workspace.present(.recording); settle()
    expect(revision() == finalRevision && workspace.preference == layout, "screen switching preserves project and arrangement layout")
    expect(view.snapshots.count == 3 && !view.isGesturing, "return restores actual comp sources")
    workspace.present(.arrange)
    print("PASS: \(checks) recording/comp workspace assertions")
    return checks
}
