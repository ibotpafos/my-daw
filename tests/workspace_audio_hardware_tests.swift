import AppKit

private final class HardwareOperationFixture: AudioHardwareOperation {
    var pending = true
    var failed = false
    var canceled = false
    func cancel() { canceled = true }
    func poll() throws -> AudioHardwareProgress {
        AudioHardwareProgress(pending: pending, success: !failed,
            message: failed ? "Изменение не выполнено; прежний формат восстановлен." : "Подтверждено устройством: 48 кГц · 128 кадров.")
    }
}
@MainActor
func runAudioHardwareSettingsTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ label: String) {
        checks += 1; if !condition() { fatalError("Hardware settings: \(label)") }
    }
    var raw = daw_audio_hardware_settings()
    raw.struct_size = UInt32(MemoryLayout<daw_audio_hardware_settings>.size)
    raw.version = UInt32(DAW_AUDIO_HARDWARE_VERSION)
    raw.device_id = UInt32.max; raw.sample_rate = 44100; raw.buffer_frames = 256
    raw.minimum_buffer = 32; raw.maximum_buffer = 4096
    raw.supports_48k = 1; raw.rate_writable = 1; raw.buffer_writable = 1
    let uid = "mydaw-ci-nonexistent-hardware-f6f54052"
    withUnsafeMutableBytes(of: &raw.uid) { $0.copyBytes(from: uid.utf8CString.map { UInt8(bitPattern: $0) }) }
    var begins = 0, loads = 0, completed = 0, loadFails = false
    var chosen: UInt32 = 0
    let fixture = HardwareOperationFixture()
    let service = AudioHardwareService(load: { requested in
        expect(requested == uid, "captured explicit UID")
        loads += 1
        if loadFails { throw AudioDevicePreferences.Failure.invalid }
        return raw
    }, begin: { snapshot, buffer in
        expect(snapshot.buffer_frames == 256 && snapshot.sample_rate == 44100, "expected snapshot reaches operation")
        begins += 1; chosen = buffer
        return fixture
    })
    let panel = AudioHardwareSettingsController(uid: uid, name: "Studio interface — input / output", service: service)
    defer { panel.close() }
    panel.onCompletion = { completed += 1 }
    expect(begins == 0 && loads == 1, "opening never changes hardware")
    expect(panel.buffer.numberOfItems == 8, "driver bounded buffer choices")
    expect(panel.buffer.selectedItem?.tag == 256 && panel.applyButton.isEnabled, "current buffer selected")
    expect(panel.applyButton.keyEquivalent.isEmpty, "hardware changes have no Return default")
    panel.buffer.selectItem(withTag: 128)
    panel.applyButton.performClick(nil)
    expect(begins == 1 && chosen == 128 && panel.isApplying, "native button begins requested operation")
    expect(!panel.applyButton.isEnabled && !panel.refreshButton.isEnabled && !panel.buffer.isEnabled, "pending controls locked")
    panel.apply(); panel.refresh(); panel.poll()
    expect(begins == 1 && loads == 1 && completed == 0 && panel.isApplying, "no duplicate start or false completion")
    fixture.pending = false; raw.sample_rate = 48000; raw.buffer_frames = 128
    panel.poll()
    expect(!panel.isApplying && completed == 1 && fixture.canceled, "completion releases handle")
    expect(panel.current.stringValue.contains("48.0") && panel.buffer.selectedItem?.tag == 128, "actual format refreshed")
    expect(panel.status.stringValue.contains("Подтверждено"), "readback success message")
    // Capture actual AppKit controls before negative-path tests.
    panel.window?.contentView?.layoutSubtreeIfNeeded()
    if let view = panel.window?.contentView, let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
        view.cacheDisplay(in: view.bounds, to: bitmap)
        if let data = bitmap.representation(using: .png, properties: [:]) {
            try! data.write(to: URL(fileURLWithPath: "build/workspace-ui/audio-hardware-settings.png"))
        }
        expect(panel.applyButton.frame.width > 80 && panel.buffer.frame.width >= 140, "usable native controls")
        let rect = panel.status.convert(panel.status.bounds, to: view)
        expect(rect.minX >= 0 && rect.maxX <= view.bounds.maxX + 1 && rect.maxY <= view.bounds.maxY + 1, "status inside panel")
    } else { fatalError("hardware screenshot") }
    loadFails = true; panel.refreshButton.performClick(nil)
    expect(panel.snapshot == nil && !panel.applyButton.isEnabled, "failed refresh invalidates previous snapshot")
    panel.apply(); expect(begins == 1, "cannot reuse stale snapshot after error")
    loadFails = false; raw.rate_writable = 0; raw.sample_rate = 44100
    panel.refresh(); expect(!panel.applyButton.isEnabled, "non-48 readonly device blocked")
    raw.sample_rate = 48000; raw.buffer_writable = 0
    panel.refresh(); expect(panel.buffer.numberOfItems == 1 && !panel.buffer.isEnabled, "readonly current buffer only")
    raw.buffer_writable = 1; raw.minimum_buffer = 129; raw.maximum_buffer = 193; raw.buffer_frames = 192
    expect(AudioHardwareSettingsController.buffers(raw) == [129, 192, 193], "non-power-of-two bounds/current preserved")
    raw.maximum_buffer = 128
    expect(AudioHardwareSettingsController.buffers(raw).isEmpty, "invalid range fails closed")
    raw.minimum_buffer = 8192; raw.maximum_buffer = 16384; raw.buffer_frames = 8192
    expect(AudioHardwareSettingsController.buffers(raw).isEmpty, "device above engine limit not silently clamped")
    // Closing requests cancellation, not a mutation of the DAW project.
    raw.minimum_buffer = 32; raw.maximum_buffer = 4096; raw.buffer_frames = 256; raw.sample_rate = 44100; raw.rate_writable = 1
    fixture.pending = true; fixture.canceled = false
    panel.refresh(); panel.apply(); panel.close()
    expect(fixture.canceled && !panel.isApplying, "window close cancels and stops timer")
    // Production service / C ABI uses only an impossible UID and cannot open
    // hardware on CI. No success is claimed from this deliberately failed job.
    var before = daw_snapshot(); before.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &before) == 0, "read real project")
    do {
        _ = try app.audioHardwareService().load(uid)
        fatalError("missing hardware must fail")
    } catch { expect(true, "production missing device error") }
    do {
        let operation = try app.audioHardwareService().begin(raw, 128)
        var result = try operation.poll()
        for _ in 0..<200 where result.pending {
            Thread.sleep(forTimeInterval: 0.005)
            result = try operation.poll()
        }
        expect(!result.pending && !result.success, "real bridge worker fails unavailable device safely")
        operation.cancel()
    } catch { fatalError("start valid negative hardware test: \(error)") }
    var after = daw_snapshot(); after.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &after) == 0 && before.revision == after.revision && before.can_undo == after.can_undo,
           "hardware UI path does not alter project or Undo")
    print("PASS: audio hardware AppKit: \(checks) checks")
    return checks
}
