import AppKit

@MainActor
func runArrangementOverviewTests(_ controller: DraftApp) -> Int {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ text: String) {
        checks += 1
        if !value() { fatalError("Overview integration: \(text)") }
    }
    guard let overview = controller.workspace?.overview, let scroll = controller.timelineScroll,
          let root = controller.window.contentView else { fatalError("Production overview missing") }
    func settle() {
        for _ in 0..<3 { root.layoutSubtreeIfNeeded(); controller.fitTimelineViewport() }
        root.layoutSubtreeIfNeeded(); controller.updateArrangementOverview()
    }
    func transport() -> daw_transport {
        var value = daw_transport(); value.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        expect(daw_get_transport(controller.session, &value) == 0, "Read real transport")
        return value
    }
    func revision() -> UInt64 {
        var value = daw_snapshot(); value.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(controller.session, &value) == 0, "Read real revision")
        return value.revision
    }
    func mouse(_ type: NSEvent.EventType, fraction: Double, clicks: Int = 1) -> NSEvent {
        let p = overview.convert(NSPoint(x: overview.mapRect.minX + fraction * overview.mapRect.width,
                                        y: overview.bounds.midY), to: nil)
        return NSEvent.mouseEvent(with: type, location: p, modifierFlags: [], timestamp: 0,
            windowNumber: controller.window.windowNumber, context: nil, eventNumber: 0, clickCount: clicks, pressure: 1)!
    }
    func key(_ code: UInt16, flags: NSEvent.ModifierFlags = []) -> NSEvent {
        NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: controller.window.windowNumber, context: nil, characters: "", charactersIgnoringModifiers: "",
            isARepeat: false, keyCode: code)!
    }
    controller.workspace?.resetLayout()
    controller.window.setContentSize(NSSize(width: 1536, height: 1000))
    controller.setTimelineZoom(4); settle()
    expect(overview.isDescendant(of: controller.workspace!), "Mounted in the actual workspace")
    expect(overview.scrollView === scroll && overview.bounds.height >= 30, "Bound to native arrangement scroll view")
    expect(abs(overview.geometry.extent - 0.25) < 0.005, "Quarter project visible at 4x")
    expect(overview.tracks.count == controller.mixerWorkspace.strips.filter { $0.kind == .track }.count, "Tracks reflect actual project order")
    let expectedClips = controller.laneViews.values.reduce(0) { $0 + $1.clips.count } + controller.midiArrangementViews.reduce(0) { $0 + $1.clips.count }
    expect(overview.tracks.flatMap { $0 }.count == expectedClips && expectedClips > 0, "Actual audio and MIDI clips, not demo content")
    let generation = overview.contentGeneration
    for _ in 0..<20 { controller.updateWorkspaceChrome() }
    expect(overview.contentGeneration == generation, "Transport chrome does not rebuild clip projection")
    let before = transport(), beforeRevision = revision()
    let y = scroll.contentView.bounds.minY
    overview.mouseDown(with: mouse(.leftMouseDown, fraction: 0.8))
    overview.mouseUp(with: mouse(.leftMouseUp, fraction: 0.8))
    expect(abs(overview.geometry.start - 0.675) < 0.005, "Click centers native viewport")
    expect(scroll.contentView.bounds.minX > 0 && scroll.contentView.bounds.minY == y, "Only horizontal viewport moved")
    overview.navigate(to: 0.3)
    overview.mouseDown(with: mouse(.leftMouseDown, fraction: 0.35))
    expect(abs(overview.geometry.start - 0.3) < 0.005, "Grabbing inside frame does not jump")
    overview.mouseDragged(with: mouse(.leftMouseDragged, fraction: 0.45))
    expect(abs(overview.geometry.start - 0.4) < 0.005, "Drag preserves grab offset")
    overview.keyDown(with: key(53))
    overview.mouseUp(with: mouse(.leftMouseUp, fraction: 0.45))
    expect(abs(overview.geometry.start - 0.3) < 0.005 && !overview.hasGesture, "Escape restores viewport; late mouse-up ignored")
    overview.mouseDown(with: mouse(.leftMouseDown, fraction: 0.35))
    controller.setTimelineZoom(2); settle()
    expect(!overview.hasGesture, "Zoom invalidates old drag geometry")
    let zoomedOrigin = overview.geometry.start
    overview.mouseDragged(with: mouse(.leftMouseDragged, fraction: 0.9)); overview.mouseUp(with: mouse(.leftMouseUp, fraction: 0.9))
    expect(overview.geometry.start == zoomedOrigin, "Old drag cannot move resized viewport")
    controller.setTimelineZoom(4); settle(); overview.navigate(to: 0.3)
    overview.mouseDown(with: mouse(.leftMouseDown, fraction: 0.35))
    let originalID = controller.midiDocumentID
    controller.midiDocumentID = UUID(); controller.updateArrangementOverview()
    expect(!overview.hasGesture && overview.contentGeneration == generation + 1, "Same revision new-document token invalidates gesture and projection")
    overview.mouseDragged(with: mouse(.leftMouseDragged, fraction: 0.95)); overview.mouseUp(with: mouse(.leftMouseUp, fraction: 0.95))
    expect(abs(overview.geometry.start - 0.3) < 0.005, "Late document drag ignored")
    controller.midiDocumentID = originalID; controller.updateArrangementOverview()

    let oldDelete = controller.window.onDeleteSelectedClip, oldTrackDelete = controller.window.onDeleteSelectedTrack
    let oldRewind = controller.window.onRewind
    var destructiveCalls = 0
    controller.window.onDeleteSelectedClip = { destructiveCalls += 1 }
    controller.window.onDeleteSelectedTrack = { destructiveCalls += 1 }
    controller.window.onRewind = { destructiveCalls += 1 }
    defer {
        controller.window.onDeleteSelectedClip = oldDelete; controller.window.onDeleteSelectedTrack = oldTrackDelete
        controller.window.onRewind = oldRewind
    }
    expect(controller.window.makeFirstResponder(overview), "Navigator can receive native focus")
    controller.window.sendEvent(key(115))
    expect(overview.geometry.start == 0 && destructiveCalls == 0, "Home scrolls, never rewinds actual transport")
    controller.window.sendEvent(key(119))
    expect(abs(overview.geometry.start - 0.75) < 0.005, "End scrolls to last viewport")
    controller.window.sendEvent(key(123))
    expect(overview.geometry.start < 0.75, "Arrow pans locally")
    controller.window.sendEvent(key(51)); _ = controller.window.performKeyEquivalent(with: key(51, flags: [.command]))
    expect(destructiveCalls == 0, "Delete and Command-Delete never delete project content")
    expect(!overview.handleFocusedKey(key(49)), "Space remains the existing global transport action")
    overview.setAccessibilityValue(NSNumber(value: 0.4))
    expect(abs(overview.geometry.start - 0.4) < 0.005, "Accessible value uses native scroll")
    expect(overview.accessibilityPerformIncrement(), "Accessible increment available")
    expect(overview.geometry.start > 0.4, "Accessible increment pans viewport")
    expect(overview.accessibilityPerformDecrement(), "Accessible decrement available")
    expect(!overview.navigate(to: .nan), "Invalid navigation rejected")
    // Pure viewport navigation is safe while capture is active: no audio command.
    controller.isRecording = true; overview.navigate(to: 0.2); controller.isRecording = false
    controller.midiTakeArmed = true; overview.navigate(to: 0.5); controller.midiTakeArmed = false
    let after = transport()
    expect(after.frame == before.frame && after.playing == before.playing, "No transport seek or stop from navigation")
    expect(after.loop_enabled == before.loop_enabled && after.loop_start == before.loop_start && after.loop_end == before.loop_end, "Cycle unchanged")
    expect(revision() == beforeRevision, "Navigation creates no project revision/Undo")

    // Native track edits rebuild the projection, then undo restores the same data.
    let oldGeneration = overview.contentGeneration
    let id = controller.mixerWorkspace.strips.first(where: { $0.kind == .track })!.id
    expect(daw_set_track_color(controller.session, id, 0xF07040, revision()) == 0, "Real track-color command")
    controller.refresh(); settle()
    expect(overview.contentGeneration == oldGeneration + 1, "Committed edit refreshes projection exactly once")
    expect(overview.tracks.first?.first?.color.usingColorSpace(.sRGB) == dawColorFromHex(0xF07040).usingColorSpace(.sRGB), "Actual track color projected")
    controller.undo(); settle()
    expect(overview.contentGeneration == oldGeneration + 2, "Undo refreshes real projection")
    for width in [1060, 1536, 1920] {
        controller.window.setContentSize(NSSize(width: width, height: 1000)); settle()
        expect(overview.viewportRect.minX >= overview.mapRect.minX - 1 && overview.viewportRect.maxX <= overview.mapRect.maxX + 1, "Viewport stays inside map after resize")
        expect(overview.bounds.width == controller.workspace!.geometry.center, "Overview remains pinned at full center width")
    }
    controller.workspace?.setDockFocus(true); settle()
    expect(overview.isHiddenOrHasHiddenAncestor, "Mixer Focus hides overview with arrangement")
    controller.workspace?.setDockFocus(false); settle()
    expect(!overview.isHiddenOrHasHiddenAncestor, "Leaving Focus restores same overview")
    overview.mouseDown(with: mouse(.leftMouseDown, fraction: 0.4, clicks: 2)); settle()
    expect(controller.timelineZoom == 1 && abs(overview.geometry.extent - 1) < 0.005, "Double click uses existing fit-all zoom")
    controller.setTimelineZoom(4); settle(); overview.navigate(to: 0.25)
    controller.window.makeFirstResponder(overview)
    print("PASS: \(checks) native arrangement overview assertions")
    return checks
}
