import AppKit

@MainActor
func runRecordingControlsTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ label: String) {
        checks += 1; if !value() { fatalError("Recording controls: \(label)") }
    }
    let previous = app.recordMonitorOn
    defer {
        _ = daw_set_record_monitor(app.session, previous ? 1 : 0)
        app.recordMonitorOn = previous; app.syncRecordMonitorButton()
        app.setProjectControlsEnabled(true)
    }
    var before = daw_snapshot(); before.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &before) == 0, "read real project revision")
    expect(app.recordMonitorButton.isDescendant(of: app.window.contentView!), "real MON mounted")
    app.setProjectControlsEnabled(false)
    expect(app.recordMonitorButton.isEnabled, "MON stays enabled with recording project lock")
    expect(app.recordButton.isEnabled && !app.playButton.isEnabled, "Record can stop; Play cannot conflict")
    app.recordMonitorButton.performClick(nil)
    var monitor: Int32 = 0
    expect(daw_get_record_monitor(app.session, &monitor) == 0 && (monitor != 0) == !previous,
           "native MON action reaches C ABI")
    expect(app.recordMonitorOn == !previous, "display reflects actual monitor intent")
    expect(app.recordMonitorButton.accessibilityHelp()?.contains("любой аудиозаписи") == true,
           "accessibility describes normal and loop capture")
    app.recordMonitorButton.performClick(nil)
    expect(daw_get_record_monitor(app.session, &monitor) == 0 && (monitor != 0) == previous,
           "second native click restores setting")
    var after = daw_snapshot(); after.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &after) == 0 && before.revision == after.revision && before.can_undo == after.can_undo,
           "monitor never creates project command")
    var recording = daw_recording(); recording.struct_size = UInt32(MemoryLayout<daw_recording>.size)
    recording.recording = 1; recording.frames = 48000
    var text = recordingStatusText(recording, monitor: false)
    expect(text.contains("Запись") && text.contains("playback + mono") && text.contains("MON выкл."), "linear capture label")
    expect(!text.contains("дублей"), "linear capture is not labelled loop")
    recording.target_track_id = 12
    text = recordingStatusText(recording, monitor: true)
    expect(text.contains("Новый дубль") && text.contains("MON вкл."), "normal take label")
    recording.loop_recording = 1; recording.pass_count = 3
    text = recordingStatusText(recording, monitor: true)
    expect(text.contains("Луп-запись") && text.contains("дублей 3"), "loop mode remains separate")
    print("PASS: normal recording native controls: \(checks) checks")
    return checks
}
