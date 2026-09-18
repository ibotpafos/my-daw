import AppKit

@MainActor
func runAudioDeviceSettingsTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ label: String) {
        checks += 1; if !condition() { fatalError("Audio settings: \(label)") }
    }
    let suite = "mydaw-audio-ui-\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: suite)!
    let previous = app.audioPreferences
    app.audioPreferences = defaults
    defer {
        app.audioPreferences = previous
        defaults.removePersistentDomain(forName: suite)
        let defaultsConfig = AudioDevicePreferences()
        var input = try! defaultsConfig.recordingInputBridgeValue()
        _ = daw_set_record_input_config(app.session, &input)
        var raw = try! defaultsConfig.bridgeValue()
        _ = daw_set_audio_device_config(app.session, &raw)
    }
    var revision = daw_snapshot(); revision.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &revision) == 0, "read revision")
    var choices = [
        AudioDeviceChoice(uid: "fixture-input", name: "Studio interface", inputs: 8, outputs: 8,
                          sampleRate: 48000, buffer: 128, defaultInput: true, defaultOutput: true),
        AudioDeviceChoice(uid: "fixture-second", name: "Studio interface", inputs: 2, outputs: 2,
                          sampleRate: 44100, buffer: 256, defaultInput: false, defaultOutput: false)]
    let controller = AudioDeviceSettingsController(configuration: AudioDevicePreferences(), load: { choices },
        apply: { try app.applyAudioDeviceConfiguration($0) })
    defer { controller.close() }
    expect(controller.inputDevice.numberOfItems == 3, "catalog plus explicit default choice")
    controller.inputDevice.selectItem(at: 1)
    controller.selectionChanged(controller.inputDevice)
    controller.outputDevice.selectItem(at: 1)
    controller.selectionChanged(controller.outputDevice)
    expect(controller.inputChannel.numberOfItems == 8 && controller.inputRight.numberOfItems == 8,
           "physical channel choices")
    expect(controller.recordingMode.item(withTag: 2)?.isEnabled == true, "stereo mode available for multi-input device")
    controller.recordingMode.selectItem(withTag: 2)
    controller.selectionChanged(controller.recordingMode)
    controller.inputChannel.selectItem(withTag: 3)
    controller.selectionChanged(controller.inputChannel)
    controller.inputRight.selectItem(withTag: 4)
    controller.selectionChanged(controller.inputRight)
    controller.outputLeft.selectItem(withTag: 4)
    controller.selectionChanged(controller.outputLeft)
    controller.outputRight.selectItem(withTag: 5)
    controller.selectionChanged(controller.outputRight)
    controller.applyButton.performClick(nil)
    let saved = try! AudioDevicePreferences.read(from: defaults)
    expect(saved.inputUID == "fixture-input" && saved.outputUID == "fixture-input", "stable UID, not display name")
    expect(saved.recordingChannels == 2 && saved.inputChannel == 3 && saved.inputRight == 4 &&
           saved.outputLeft == 4 && saved.outputRight == 5, "stereo UI maps to zero-based routing")
    var raw = daw_audio_device_config()
    raw.struct_size = UInt32(MemoryLayout<daw_audio_device_config>.size); raw.version = UInt32(DAW_AUDIO_DEVICE_CONFIG_VERSION)
    expect(daw_get_audio_device_config(app.session, &raw) == 0 && raw.input_channel == 3,
           "real device C ABI applied")
    var recordingInput = daw_record_input_config()
    recordingInput.struct_size = UInt32(MemoryLayout<daw_record_input_config>.size)
    recordingInput.version = UInt32(DAW_RECORD_INPUT_CONFIG_VERSION)
    expect(daw_get_record_input_config(app.session, &recordingInput) == 0 &&
           recordingInput.channels == 2 && recordingInput.left == 3 && recordingInput.right == 4,
           "real stereo recording C ABI applied")
    var after = daw_snapshot(); after.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
    expect(daw_get_snapshot(app.session, &after) == 0 && revision.revision == after.revision && revision.can_undo == after.can_undo, "no project/Undo mutation")
    choices.reverse(); controller.refreshButton.performClick(nil)
    expect(controller.inputDevice.selectedItem?.representedObject as? String == "fixture-input", "refresh/reorder preserves UID")
    choices.removeAll(); controller.refreshDevices()
    expect(controller.inputDevice.selectedItem?.representedObject as? String == "fixture-input", "disconnect never selects system default")
    expect(controller.inputInfo.stringValue.contains("недоступно"), "missing hardware is visible")
    controller.apply()
    expect(try! AudioDevicePreferences.read(from: defaults) == saved, "offline explicit choice retained")
    // Malformed channel selections cannot replace a previous valid preference.
    let invalid = AudioDeviceSettingsController(configuration: AudioDevicePreferences(outputLeft: 1, outputRight: 1),
        load: { [] }, apply: { try app.applyAudioDeviceConfiguration($0) })
    invalid.apply()
    expect(invalid.status.stringValue.contains("Некорректные"), "inline validation")
    expect(try! AudioDevicePreferences.read(from: defaults) == saved, "invalid apply is atomic")
    invalid.close()
    // The original production action is visible to both native menu and palette.
    expect(NSApp.mainMenu?.items.first?.submenu?.items.contains(where: {
        $0.action == #selector(DraftApp.showAudioDeviceSettings)
    }) == true, "settings menu mounted")
    if let fresh = daw_create() {
        app.restoreAudioDeviceConfiguration(fresh)
        expect(daw_get_audio_device_config(fresh, &raw) == 0 && raw.input_channel == 3 && raw.output_right == 5,
               "New session restores device preferences through production helper")
        expect(daw_get_record_input_config(fresh, &recordingInput) == 0 &&
               recordingInput.channels == 2 && recordingInput.left == 3 && recordingInput.right == 4,
               "New session restores stereo recording source")
        daw_destroy(fresh)
    } else { fatalError("fresh session") }
    // Capturing the actual view uses native controls, not a design mockup.
    controller.window?.contentView?.layoutSubtreeIfNeeded()
    if let view = controller.window?.contentView, let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
        view.cacheDisplay(in: view.bounds, to: bitmap)
        if let data = bitmap.representation(using: .png, properties: [:]) {
            try! data.write(to: URL(fileURLWithPath: "build/workspace-ui/audio-device-settings.png"))
        }
        expect(controller.inputDevice.frame.width > 40 && controller.applyButton.frame.width > 40, "controls have usable frames")
    } else { fatalError("audio settings screenshot") }
    print("PASS: audio device settings AppKit: \(checks) checks")
    return checks
}
