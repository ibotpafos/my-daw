import AppKit

@MainActor
func runAudioHardwareSettingsTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ label: String) {
        checks += 1; if !condition() { fatalError("Hardware settings: \(label)") }
    }
    let initial = AudioHardwareFormat(sampleRate: 44100, bufferFrames: 512,
        minimumFrames: 32, maximumFrames: 2048, sampleRates: [44100...48000],
        rateWritable: true, bufferWritable: true, running: false)
    var current = initial
    var requests: [UInt32] = []
    var result = AudioHardwareChangeResult(state: UInt32(DAW_DEVICE_CHANGE_PENDING))
    var loadFails = false
    var beginFails = false
    var finishes = 0
    let panel = AudioHardwareSettingsController(uid: "fixture-hardware-uid", load: {
        if loadFails { throw AudioDevicePreferences.Failure.invalid }
        return current
    }, begin: {
        if beginFails { throw AudioDevicePreferences.Failure.invalid }
        requests.append($0)
    }, poll: { result })
    panel.didFinish = { finishes += 1 }
    defer { panel.close() }
    expect(requests.isEmpty, "opening only reads hardware")
    expect(panel.buffer.stringValue == "512" && panel.buffer.numberOfItems == 7, "supported range choices and actual value")
    expect(panel.deviceUID == "fixture-hardware-uid", "explicit immutable identity")
    expect(panel.info.stringValue.contains("44.1"), "shows actual not requested rate")
    expect(panel.applyButton.isEnabled && !panel.pending, "idle controls")
    for text in ["", "0", "4097", "64.5", "nan", "1024junk", "16"] {
        panel.buffer.stringValue = text
        panel.applyButton.performClick(nil)
        expect(requests.isEmpty && !panel.pending, "invalid edit never begins: \(text)")
    }
    beginFails = true; panel.buffer.stringValue = "256"; panel.applyButton.performClick(nil)
    expect(!panel.pending && requests.isEmpty, "bridge rejection preserves retry path")
    beginFails = false
    // Real field editor: the cell has 512, the user types 256 and immediately clicks Apply.
    panel.window?.makeKeyAndOrderFront(nil)
    panel.buffer.stringValue = "512"
    expect(panel.window?.makeFirstResponder(panel.buffer) == true, "combo can take focus")
    guard let editor = panel.window?.fieldEditor(true, for: panel.buffer) as? NSTextView else {
        fatalError("hardware combo field editor missing")
    }
    editor.selectAll(nil); editor.insertText("256", replacementRange: NSRange(location: NSNotFound, length: 0))
    panel.applyButton.performClick(nil)
    expect(requests == [256], "Apply commits live text, not old cell value")
    expect(panel.pending && !panel.applyButton.isEnabled && !panel.buffer.isEnabled && !panel.refreshButton.isEnabled,
           "all mutating controls disabled while awaiting acknowledgement")
    expect(panel.window?.standardWindowButton(.closeButton)?.isEnabled == false, "pending native close disabled")
    expect(panel.windowShouldClose(panel.window!) == false, "pending delegate close refused")
    panel.apply(); panel.refresh(); panel.close()
    expect(requests == [256] && panel.pending, "repeat apply, refresh and close do not resubmit")
    expect(!panel.status.stringValue.contains("подтвердил"), "submission is not success")
    current.sampleRate = 48000; current.bufferFrames = 256
    result = AudioHardwareChangeResult(state: UInt32(DAW_DEVICE_CHANGE_APPLIED), sampleRate: 48000,
        bufferFrames: 256, actualKnown: true)
    panel.pollChange()
    expect(!panel.pending && panel.applyButton.isEnabled && finishes == 1, "acknowledged terminal state re-enables UI")
    expect(panel.status.stringValue.contains("подтвердил") && panel.status.stringValue.contains("256"), "success uses confirmed format")
    panel.pollChange()
    expect(finishes == 1, "terminal poll idempotent")
    result = AudioHardwareChangeResult(state: UInt32(DAW_DEVICE_CHANGE_FAILED), mayHaveChanged: true,
        error: "Timed out waiting for Core Audio")
    panel.buffer.stringValue = "128"; panel.applyButton.performClick(nil)
    expect(!panel.pending && requests == [256, 128], "failed operation terminates UI")
    expect(panel.status.stringValue.contains("Timed out") && panel.status.stringValue.contains("отката нет"), "partial timeout is visible without fake rollback")
    result = AudioHardwareChangeResult(state: UInt32(DAW_DEVICE_CHANGE_APPLIED), actualKnown: false)
    panel.apply()
    expect(!panel.status.stringValue.contains("подтвердил"), "unknown actual state cannot be success")
    current.running = true; panel.refresh()
    expect(!panel.applyButton.isEnabled && !panel.buffer.isEnabled, "externally running hardware blocked")
    current.running = false; loadFails = true; panel.refresh()
    expect(panel.format == nil && !panel.applyButton.isEnabled, "disconnect discards cached capabilities")
    loadFails = false; panel.refresh()
    expect(panel.format != nil && panel.applyButton.isEnabled, "refresh after reconnect")

    // The real parent action resolves the system-default entry once. Later catalog
    // changes must not retarget a visible settings panel to a different device.
    var choices = [AudioDeviceChoice(uid: "first", name: "Interface", inputs: 2, outputs: 2,
        sampleRate: 48000, buffer: 256, defaultInput: true, defaultOutput: true)]
    let parent = AudioDeviceSettingsController(configuration: AudioDevicePreferences(), load: { choices }, apply: { _ in })
    var openedUID = ""
    parent.makeHardwareSettings = { uid in
        openedUID = uid
        return AudioHardwareSettingsController(uid: uid, load: { initial }, begin: { _ in }, poll: { result })
    }
    parent.outputFormatButton.performClick(nil)
    expect(openedUID == "first" && parent.hardwareSettings?.deviceUID == "first", "button freezes resolved default UID")
    choices = [AudioDeviceChoice(uid: "second", name: "Interface", inputs: 2, outputs: 2,
        sampleRate: 48000, buffer: 256, defaultInput: true, defaultOutput: true)]
    parent.refreshDevices()
    expect(parent.hardwareSettings?.deviceUID == "first", "refresh/default change does not retarget existing operation")
    parent.hardwareSettings?.close(); parent.close()

    // Real production bridge helpers: malformed/absent UID never select a default
    // or write hardware, and inspection does not mutate the musical project.
    var before = daw_snapshot(); before.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &before) == 0, "real revision before rejected request")
    do { try app.beginHardwareFormat(uid: "", frames: 256); fatalError("empty UID accepted") }
    catch { checks += 1 }
    do { _ = try app.hardwareFormat(uid: "mydaw-ci-definitely-missing-uid"); fatalError("missing hardware accepted") }
    catch { checks += 1 }
    do { try app.beginHardwareFormat(uid: "mydaw-ci-definitely-missing-uid", frames: 256); fatalError("missing hardware changed") }
    catch { checks += 1 }
    let idle = try! app.pollHardwareFormat()
    expect(idle.state == UInt32(DAW_DEVICE_CHANGE_IDLE) && !idle.actualKnown, "failed begin leaves no phantom operation")
    var after = daw_snapshot(); after.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &after) == 0 && before.revision == after.revision && before.can_undo == after.can_undo,
           "real bridge keeps revision and Undo")
    panel.window?.contentView?.layoutSubtreeIfNeeded()
    if let view = panel.window?.contentView, let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
        expect(panel.buffer.frame.width >= 150 && panel.applyButton.frame.height >= 18, "usable control geometry")
        let statusFrame = panel.status.convert(panel.status.bounds, to: view)
        expect(view.bounds.insetBy(dx: 1, dy: 1).contains(statusFrame), "status fits panel")
        view.cacheDisplay(in: view.bounds, to: bitmap)
        try! bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "build/workspace-ui/audio-hardware-settings.png"))
    } else { fatalError("hardware screenshot missing") }
    print("PASS: audio hardware settings AppKit: \(checks) checks")
    return checks
}
