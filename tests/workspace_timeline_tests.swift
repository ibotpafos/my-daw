import AppKit

@MainActor
func runTimelineEditingTests(_ controller: DraftApp) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        if !condition() { fatalError("Timeline integration: \(message)") }
    }
    func transport() -> daw_transport {
        var value = daw_transport(); value.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        expect(daw_get_transport(controller.session, &value) == 0, "Read actual transport")
        return value
    }
    func revision() -> UInt64 {
        var value = daw_snapshot(); value.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(controller.session, &value) == 0, "Read actual project")
        return value.revision
    }
    let root = controller.window.contentView!
    controller.setTimelineZoom(1); controller.restoreArrangementViewport(.zero)
    controller.clearRange(); controller.seekAudio(0)
    controller.gridPopup.selectItem(withTitle: "1/8"); controller.updateTimelineTools()
    root.layoutSubtreeIfNeeded()
    let range = controller.timelineRuler.cycleRange
    expect(range.bounds.height >= 24 && range.bounds.width == controller.timelineRuler.bounds.width, "Range strip is mounted in the actual ruler")
    expect(range.playableFrames == transport().duration && range.playableFrames > 0, "Actual playable duration, not padded viewport")
    let before = revision()
    func event(_ type: NSEvent.EventType, _ frame: UInt64, modifiers: NSEvent.ModifierFlags = []) -> NSEvent {
        let x = CGFloat(frame) / CGFloat(range.projectFrames) * range.bounds.width
        let location = range.convert(NSPoint(x: x, y: range.bounds.midY), to: nil)
        return NSEvent.mouseEvent(with: type, location: location, modifierFlags: modifiers,
            timestamp: 0, windowNumber: controller.window.windowNumber, context: nil,
            eventNumber: 0, clickCount: 1, pressure: 1)!
    }
    func drag(_ start: UInt64, _ end: UInt64, modifiers: NSEvent.ModifierFlags = []) {
        range.mouseDown(with: event(.leftMouseDown, start, modifiers: modifiers))
        range.mouseDragged(with: event(.leftMouseDragged, end, modifiers: modifiers))
        range.mouseUp(with: event(.leftMouseUp, end, modifiers: modifiers))
    }
    range.mouseDown(with: event(.leftMouseDown, 99000))
    range.mouseDragged(with: event(.leftMouseDragged, 286000))
    expect(range.preview?.start == 96000 && range.preview?.end == 288000, "Drag preview snaps through existing beat grid")
    expect(transport().loop_enabled == 0 && controller.rangeStart == nil, "Preview does not change live transport")
    range.mouseUp(with: event(.leftMouseUp, 286000))
    expect(controller.rangeStart == 96000 && controller.rangeEnd == 288000 && controller.loopEnabled, "Mouse-up commits existing range state")
    expect(transport().loop_start == 96000 && transport().loop_end == 288000, "Actual engine receives the cycle")
    expect(controller.waveforms.allSatisfy { $0.rangeStart == 96000 && $0.rangeEnd == 288000 }, "Audio lanes share the committed range")
    expect(controller.midiArrangementViews.allSatisfy { $0.cycleSelection == range.selection && $0.cycleEnabled }, "MIDI lanes share the committed range")
    expect(controller.exportButton.title.contains("диапазона"), "Export uses the same selected range")

    drag(96000, 48000)
    expect(controller.rangeStart == 48000 && controller.rangeEnd == 288000, "Left edge resizes without shifting end")
    drag(288000, 336000)
    expect(controller.rangeStart == 48000 && controller.rangeEnd == 336000, "Right edge resizes without shifting start")
    drag(180000, 228000)
    expect(controller.rangeStart == 96000 && controller.rangeEnd == 384000, "Body drag translates both edges")
    drag(192000, 0, modifiers: [.shift])
    expect(controller.rangeStart == 0 && controller.rangeEnd == 288000, "Move clamps at zero without changing duration")
    // Option forces a new range inside an old one; Shift bypasses quantization.
    drag(53123, 200987, modifiers: [.option, .shift])
    expect(controller.rangeStart == 53123 && controller.rangeEnd == 200987, "Shift preserves exact frame endpoints")
    drag(390000, 310000, modifiers: [.option])
    expect(controller.rangeStart == 312000 && controller.rangeEnd == 396000, "Reverse creation is normalized")
    let committed = range.selection
    range.mouseDown(with: event(.leftMouseDown, 312000))
    range.mouseDragged(with: event(.leftMouseDragged, 100000))
    expect(range.preview != nil, "Escape test has a live preview")
    let escape = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
        windowNumber: controller.window.windowNumber, context: nil, characters: "\u{1b}",
        charactersIgnoringModifiers: "\u{1b}", isARepeat: false, keyCode: 53)!
    range.keyDown(with: escape); range.mouseUp(with: event(.leftMouseUp, 100000))
    expect(range.selection == committed && range.preview == nil && controller.rangeStart == 312000, "Escape discards preview, never commits")
    range.mouseDown(with: event(.leftMouseDown, 440000)); range.mouseUp(with: event(.leftMouseUp, 440000))
    expect(range.selection == committed, "Click does not replace range")
    drag(410000, range.projectFrames, modifiers: [.option])
    expect(controller.rangeEnd == range.playableFrames, "Creation clamps to actual duration excluding padding")

    let locked = range.selection
    controller.isRecording = true; controller.updateWorkspaceChrome()
    drag(20000, 50000, modifiers: [.option]); range.clearRange(); range.toggleRange()
    controller.setRangeStart(); controller.setRangeEnd(); controller.clearRange(); controller.toggleLoop()
    expect(range.selection == locked && controller.loopEnabled, "Audio recording locks drag, menu and existing range commands")
    controller.isRecording = false; controller.midiTakeArmed = true; controller.updateWorkspaceChrome()
    expect(!controller.commitTimelineRange(TimelineFrameRange(start: 0, end: 48000, limit: 48000)!), "MIDI capture guards stale callbacks")
    controller.clearRange(); controller.toggleLoop()
    expect(range.selection == locked && controller.loopEnabled, "MIDI capture locks existing commands too")
    controller.midiTakeArmed = false; controller.updateWorkspaceChrome()
    range.mouseDown(with: event(.leftMouseDown, 20000, modifiers: [.option]))
    range.mouseDragged(with: event(.leftMouseDragged, 50000, modifiers: [.option]))
    controller.isRecording = true; controller.updateWorkspaceChrome()
    range.mouseUp(with: event(.leftMouseUp, 50000))
    expect(range.selection == locked && range.preview == nil, "Capture beginning mid-gesture cancels it")
    controller.isRecording = false; controller.updateWorkspaceChrome()
    expect(!controller.commitTimelineRange(TimelineFrameRange(start: 0, end: range.playableFrames + 1, limit: UInt64.max)!), "Stale overlong range is rejected by actual ABI")
    expect(range.selection == locked, "Rejected commit preserves previous UI range")
    let menu = range.menu(for: event(.rightMouseDown, 100000))!
    expect(menu.numberOfItems == 2 && menu.items.allSatisfy(\.isEnabled), "Native cycle context actions enabled")
    menu.performActionForItem(at: 0)
    expect(!controller.loopEnabled && transport().loop_enabled == 0 && range.selection == locked, "Toggle keeps selected export range")
    menu.performActionForItem(at: 0)
    expect(controller.loopEnabled && transport().loop_enabled == 1, "Same native toggle restores cycle")
    controller.window.makeFirstResponder(range)
    expect(!controller.shouldHandleWorkspaceClipDelete, "Range focus never routes Delete into selected clips")
    let delete = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
        windowNumber: controller.window.windowNumber, context: nil, characters: "\u{7f}",
        charactersIgnoringModifiers: "\u{7f}", isARepeat: false, keyCode: 51)!
    range.keyDown(with: delete)
    expect(controller.rangeStart == nil && controller.rangeEnd == nil && transport().loop_enabled == 0, "Delete in range clears range only")
    expect(revision() == before, "All range gestures/menu/keyboard operations preserve project revision")

    // Anchor the visible cursor in document coordinates using native clip view.
    controller.seekAudio(96000); controller.setTimelineZoom(1)
    controller.restoreArrangementViewport(.zero); root.layoutSubtreeIfNeeded()
    let scroll = controller.timelineScroll!
    func screenCursorX() -> CGFloat {
        CGFloat(controller.playheadFrame) / CGFloat(controller.timelineRuler.projectFrames) * controller.timelineRuler.bounds.width - scroll.contentView.bounds.minX
    }
    let oldX = screenCursorX(); controller.setTimelineZoom(2)
    expect(abs(screenCursorX() - oldX) < 1, "Zoom in retains on-screen playhead location")
    controller.setTimelineZoom(4)
    expect(abs(screenCursorX() - oldX) < 1, "Repeated zoom retains on-screen playhead location")
    controller.restoreArrangementViewport(NSPoint(x: 0, y: 0)); controller.seekAudio(528000)
    let oldMidpoint = scroll.contentView.bounds.midX / controller.timelineRuler.bounds.width
    controller.setTimelineZoom(8)
    let newMidpoint = scroll.contentView.bounds.midX / controller.timelineRuler.bounds.width
    expect(abs(newMidpoint - oldMidpoint) < 0.001, "Offscreen playhead does not jump viewport during zoom")
    controller.setTimelineZoom(1)
    expect(scroll.contentView.bounds.minX == 0, "Fit resets horizontal offset")
    controller.setTimelineZoom(.nan)
    expect(controller.timelineZoom == 1 && scroll.contentView.bounds.minX == 0, "Invalid zoom returns to fit safely")
    expect(revision() == before, "Zoom does not dirty the project")
    _ = controller.commitTimelineRange(TimelineFrameRange(start: 96000, end: 384000, limit: transport().duration)!)
    controller.seekAudio(192000)
    checks += runArrangementEditingTests(controller)
    print("PASS: \(checks) native timeline editing assertions")
    return checks
}
