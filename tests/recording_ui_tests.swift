import AppKit

// Uses the same headless startup flag as the existing workspace harness but
// is a separate executable. Only hardware factory is replaced at link time;
// DraftApp -> public C ABI -> real renderer/writer/Undo/storage are unchanged.
@MainActor
func runWorkspaceIntegrationTests() {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        if !value() { fatalError("Recording UI: \(message)") }
    }
    let application = NSApplication.shared
    application.setActivationPolicy(.prohibited)
    let app = DraftApp()
    let preferences = UserDefaults(suiteName: "mydaw-recording-tests-\(UUID().uuidString)")!
    app.audioPreferences = preferences
    application.delegate = app
    app.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("mydaw-record-ui-\(UUID().uuidString)")
    try! FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    app.recordingRoot = root
    defer {
        recording_fixture_failure(0)
        app.window.orderOut(nil)
        app.window.delegate = nil
        NotificationCenter.default.removeObserver(app)
        daw_destroy(app.session); app.session = nil
        application.delegate = nil
        try? FileManager.default.removeItem(at: root)
    }
    func snapshot() -> daw_snapshot {
        var value = daw_snapshot(); value.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(app.session, &value) == 0, "snapshot")
        return value
    }
    func pump(_ frames: Int, _ value: Float = 0.25) -> [Float] {
        let input = [Float](repeating: value, count: frames)
        var left = [Float](repeating: 99, count: frames)
        var right = left
        expect(recording_fixture_pump(input, UInt32(frames), &left, &right) == 0, "actual shared callback body")
        expect(left == right, "mono monitor duplicated")
        app.pollTransport()
        return left
    }
    func screenshot(_ name: String) {
        app.window.setContentSize(NSSize(width: 1280, height: 800))
        guard let view = app.window.contentView else { fatalError("No content") }
        view.layoutSubtreeIfNeeded()
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { fatalError("No bitmap") }
        view.cacheDisplay(in: view.bounds, to: rep)
        try! rep.representation(using: .png, properties: [:])!.write(to:
            URL(fileURLWithPath: "build/recording-ui/\(name).png"))
    }

    app.workspace?.present(.recording)
    expect(!app.recordingWorkspace.isHiddenOrHasHiddenAncestor, "record screen is real mounted UI")

    // Start in an empty project beyond its former zero-length playback end.
    let initial = snapshot().revision
    app.rangeStart = 1000
    expect(daw_set_record_preroll(app.session, 1000) == 0, "set pre-roll")
    app.beginRecording()
    expect(app.isRecording && recording_fixture_active() == 1, "normal Record is duplex")
    expect(app.stopButton.isEnabled && app.recordMonitorButton.isEnabled, "Stop and MON remain reachable")
    expect(!app.playButton.isEnabled && !app.exportButton.isEnabled, "incompatible actions disabled")
    expect(app.transportLabel.stringValue.contains("Преролл"), "separate pre-roll state")
    _ = pump(512)
    expect(app.playheadFrame == 512 && app.timelineRuler.playhead == 512, "shared cursor advances during pre-roll")
    expect(snapshot().revision == initial && snapshot().track_count == 0, "no project mutation before stop")
    screenshot("recording-preroll")
    expect(app.recordingWorkspace.record.isEnabled && app.recordingWorkspace.monitor.isEnabled, "record screen Stop and MON available during pre-roll")
    expect(!app.recordingWorkspace.apply.isEnabled, "comp disabled during capture")
    app.recordingWorkspace.record.performClick(nil)
    expect(!app.isRecording && recording_fixture_active() == 0, "native Stop quiesces recording")
    expect(snapshot().revision == initial && snapshot().track_count == 0, "no empty pre-roll clip")
    expect(app.activeRecordingURL == nil, "no dangling active recovery")

    // No pre-roll: genuine dry samples are committed by the production UI.
    expect(daw_set_record_preroll(app.session, 0) == 0, "disable pre-roll")
    app.rangeStart = 0
    app.beginRecording()
    expect(pump(512).allSatisfy { $0 == 0 }, "MON initially off: output silent, recording not silent")
    app.recordingWorkspace.monitor.performClick(nil)
    let monitored = pump(512)
    expect(abs(monitored.last! - 0.25) < 0.000001, "native MON applies to the running capture")
    expect(app.recordMonitorOn && app.playheadFrame == 1024, "MON state and cursor")
    screenshot("recording-active")
    app.togglePlayStop() // same target reached by Space
    expect(!app.isRecording && !app.isPlaying, "Space ends recording, not only backing")
    expect(snapshot().track_count == 1 && snapshot().revision == initial + 1, "one recorded track and one revision")
    expect(app.undoButton.isEnabled && app.exportButton.isEnabled, "editing and export restored")
    let recordedRevision = snapshot().revision
    app.undo()
    expect(snapshot().track_count == 0, "UI Undo removes whole recording")
    app.redo()
    expect(snapshot().track_count == 1 && snapshot().revision > recordedRevision, "UI Redo restores recording")
    screenshot("recording-stopped")

    // An armed non-loop recording creates one take and does not divide by zero.
    var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
    expect(daw_get_track(app.session, 0, &track) == 0, "target track")
    app.armedTrackID = track.id
    app.rangeStart = 0
    app.beginRecording(); _ = pump(512, 0.5)
    expect(app.transportLabel.stringValue.contains("Новый дубль"), "ordinary take presentation")
    app.stopAudio()
    expect(daw_get_track(app.session, 0, &track) == 0 && track.take_count == 2, "exactly one new take")
    app.armedTrackID = nil

    // Capacity exhaustion is a normal Stop, not an overflow/failure dialog.
    app.rangeStart = 48000 * 600 - 9
    app.beginRecording(); _ = pump(17)
    expect(!app.isRecording && recording_fixture_active() == 0, "capture-limit auto Stop")
    expect(snapshot().track_count == 2, "limited capture committed once")

    // Driver loss is non-modal, returns controls, and preserves confirmed PCM.
    app.rangeStart = 0
    app.beginRecording(); _ = pump(512)
    let recovery = app.activeRecordingURL!
    recording_fixture_failure(2)
    app.pollTransport()
    expect(!app.isRecording && recording_fixture_active() == 0, "device loss cancels active I/O")
    expect(FileManager.default.fileExists(atPath: recovery.path), "confirmed capture remains recoverable")
    expect(app.transportLabel.stringValue.contains("восстановления"), "recovery explained without a modal")
    recording_fixture_failure(0)
    recording_fixture_failure(1)
    let beforeFailure = snapshot().revision
    app.beginRecording()
    expect(!app.isRecording && recording_fixture_active() == 0, "failed start never claims success")
    expect(snapshot().revision == beforeFailure, "failed start preserves project")
    recording_fixture_failure(0)
    // Timestamp failure takes the same native failure/recovery path, not a
    // successful Stop. The shared capture processor rejects the bad PCM.
    for fault in [Int32(3), Int32(4)] {
        app.rangeStart = 100
        let before = snapshot().revision
        app.beginRecording(); _ = pump(512)
        let clockRecovery = app.activeRecordingURL!
        recording_fixture_failure(fault)
        expect(pump(64, 0.75).allSatisfy { $0 == 0 }, "timestamp failure immediately silences output")
        expect(!app.isRecording && recording_fixture_active() == 0, "clock fault ends device lifetime")
        expect(snapshot().revision == before, "failed clock never commits partial take implicitly")
        expect(app.transportLabel.stringValue.contains(fault == 3 ? "sample clock" : "sample timestamp"), "specific clock failure reaches native UI")
        expect(app.transportLabel.stringValue.contains("восстановления"), "recovery action explained")
        expect(app.playButton.isEnabled && app.exportButton.isEnabled, "controls restored after clock error")
        expect(FileManager.default.fileExists(atPath: clockRecovery.path), "clock prefix remains recoverable")
        recording_fixture_failure(0)
        expect(daw_recover_take(app.session, clockRecovery.path, "Clock recovery", before) == 0, "real public recovery succeeds")
        expect(snapshot().revision == before + 1, "recovery is one project command")
        app.refresh()
    }
    // Real Stop controls remain responsive while delayed input drains. Only
    // the hardware timestamps/PCM are simulated; UI/bridge/writer are actual.
    expect(recording_fixture_latency(256, 17, 43) == 0, "configure paired-clock device")
    app.rangeStart = 100
    app.armedTrackID = nil
    expect(daw_set_record_preroll(app.session, 0) == 0, "zero preroll for latency scenario")
    let beforeAligned = snapshot()
    app.beginRecording()
    _ = pump(512)
    expect(app.transportLabel.stringValue.contains("компенсация"), "latency report shown in native transport")
    app.stopButton.performClick(nil)
    expect(app.isRecording && app.recordingStopPending && !app.isPlaying, "Stop requests nonblocking drain")
    expect(!app.recordMonitorButton.isEnabled && app.stopButton.isEnabled, "draining MON disabled, Stop responsive")
    expect(snapshot().revision == beforeAligned.revision, "drain request does not commit")
    app.stopButton.performClick(nil)
    expect(snapshot().revision == beforeAligned.revision, "repeated Stop does not create another command")
    expect(pump(128).allSatisfy { $0 == 0 }, "draining input never emits backing or MON")
    expect(app.isRecording && app.transportLabel.stringValue.contains("последние входные кадры"), "pending state visible")
    screenshot("recording-latency-draining")
    _ = pump(256, 0.75)
    expect(!app.isRecording && !app.recordingStopPending, "poll completes drained take")
    expect(snapshot().revision == beforeAligned.revision + 1, "aligned take is one Undo command")
    expect(snapshot().track_count == beforeAligned.track_count + 1, "one recorded track")
    app.undo()
    expect(snapshot().track_count == beforeAligned.track_count, "aligned UI Undo")
    app.redo()
    expect(snapshot().track_count == beforeAligned.track_count + 1, "aligned UI Redo")
    let beforePreroll = snapshot()
    expect(daw_set_record_preroll(app.session, 1000) == 0, "set paired-clock preroll")
    app.rangeStart = 1000
    app.beginRecording(); _ = pump(64)
    app.stopButton.performClick(nil); _ = pump(64)
    expect(!app.isRecording && !app.recordingStopPending, "Stop in preroll ends at next callback")
    expect(snapshot().revision == beforePreroll.revision && snapshot().track_count == beforePreroll.track_count, "no phantom clip from compensated preroll")
    expect(recording_fixture_no_latency() == 0, "clear device fixture")
    print("Recording native UI: \(checks) checks passed (simulated device, real AppKit/bridge/writer)")
}
