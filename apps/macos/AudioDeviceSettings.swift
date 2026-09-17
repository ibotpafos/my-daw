import AppKit

struct AudioDeviceChoice {
    let uid: String
    let name: String
    let inputs: UInt32
    let outputs: UInt32
    let sampleRate: Double
    let buffer: UInt32
    let defaultInput: Bool
    let defaultOutput: Bool
}

@MainActor
final class AudioDeviceSettingsController: NSWindowController {
    let inputDevice = NSPopUpButton(frame: .zero, pullsDown: false)
    let outputDevice = NSPopUpButton(frame: .zero, pullsDown: false)
    let inputChannel = NSPopUpButton(frame: .zero, pullsDown: false)
    let outputLeft = NSPopUpButton(frame: .zero, pullsDown: false)
    let outputRight = NSPopUpButton(frame: .zero, pullsDown: false)
    let inputInfo = NSTextField(wrappingLabelWithString: "")
    let outputInfo = NSTextField(wrappingLabelWithString: "")
    let status = NSTextField(wrappingLabelWithString: "")
    let applyButton = NSButton(title: "Применить", target: nil, action: nil)
    let refreshButton = NSButton(title: "Обновить устройства", target: nil, action: nil)
    private(set) var devices: [AudioDeviceChoice] = []
    private(set) var draft: AudioDevicePreferences
    var loadDevices: () throws -> [AudioDeviceChoice]
    var applyConfiguration: (AudioDevicePreferences) throws -> Void

    init(configuration: AudioDevicePreferences,
         load: @escaping () throws -> [AudioDeviceChoice],
         apply: @escaping (AudioDevicePreferences) throws -> Void) {
        draft = configuration
        loadDevices = load
        applyConfiguration = apply
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 600, height: 480),
                            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        panel.title = "Настройки аудио"
        panel.isReleasedWhenClosed = false
        super.init(window: panel)
        let root = NSStackView()
        root.orientation = .vertical
        root.alignment = .leading
        root.spacing = 12
        root.edgeInsets = NSEdgeInsets(top: 20, left: 20, bottom: 20, right: 20)
        root.translatesAutoresizingMaskIntoConstraints = false
        panel.contentView?.addSubview(root)
        if let content = panel.contentView {
            NSLayoutConstraint.activate([root.leadingAnchor.constraint(equalTo: content.leadingAnchor),
                root.trailingAnchor.constraint(equalTo: content.trailingAnchor),
                root.topAnchor.constraint(equalTo: content.topAnchor),
                root.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor)])
        }
        func row(_ title: String, _ controls: [NSView]) {
            let label = NSTextField(labelWithString: title)
            label.widthAnchor.constraint(equalToConstant: 120).isActive = true
            let line = NSStackView(views: [label] + controls)
            line.spacing = 8
            root.addArrangedSubview(line)
            line.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -40).isActive = true
        }
        row("Устройство входа", [inputDevice])
        row("Mono-вход", [inputChannel])
        root.addArrangedSubview(inputInfo)
        row("Устройство выхода", [outputDevice])
        row("Стереовыход L / R", [outputLeft, outputRight])
        root.addArrangedSubview(outputInfo)
        let note = NSTextField(wrappingLabelWithString:
            "Проект: 48 кГц. Частота и буфер ниже — реальные настройки устройства, а не измеренная задержка. В этой версии они изменяются в системной утилите или панели интерфейса. Для loop-записи вход и выход должны принадлежать одному устройству.")
        note.textColor = .secondaryLabelColor
        note.font = .systemFont(ofSize: 11)
        root.addArrangedSubview(note)
        let systemButton = NSButton(title: "Audio MIDI Setup…", target: self, action: #selector(openSystemSettings))
        let actions = NSStackView(views: [refreshButton, systemButton, applyButton])
        actions.spacing = 10
        root.addArrangedSubview(actions)
        root.addArrangedSubview(status)
        for label in [inputInfo, outputInfo, note, status] {
            label.widthAnchor.constraint(equalTo: root.widthAnchor, constant: -40).isActive = true
        }
        status.setAccessibilityLabel("Результат настройки аудио")
        for (control, title) in [(inputDevice, "Устройство аудиовхода"), (outputDevice, "Устройство аудиовыхода"),
                                 (inputChannel, "Канал монофонической записи"), (outputLeft, "Левый канал мастера"),
                                 (outputRight, "Правый канал мастера")] {
            control.target = self
            control.action = #selector(selectionChanged(_:))
            control.setAccessibilityLabel(title)
            control.setContentHuggingPriority(.defaultLow, for: .horizontal)
        }
        refreshButton.target = self; refreshButton.action = #selector(refreshDevices)
        applyButton.target = self; applyButton.action = #selector(apply)
        applyButton.keyEquivalent = "\r"
        refreshDevices()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    @objc func refreshDevices() {
        do {
            let catalog = try loadDevices()
            devices = catalog
            rebuildControls()
            status.stringValue = "Выбор не включает запись или мониторинг. Применение возможно только при остановленном транспорте."
        } catch { status.stringValue = error.localizedDescription }
    }
    private func populate(_ menu: NSPopUpButton, uid: String, input: Bool) {
        menu.removeAllItems()
        menu.addItem(withTitle: "Системное устройство по умолчанию")
        menu.lastItem?.representedObject = ""
        for device in devices where input ? device.inputs > 0 : device.outputs >= 2 {
            menu.addItem(withTitle: device.name)
            menu.lastItem?.representedObject = device.uid
        }
        if let index = menu.itemArray.firstIndex(where: { $0.representedObject as? String == uid }) {
            menu.selectItem(at: index)
        } else {
            menu.addItem(withTitle: "Недоступно: \(uid)")
            menu.lastItem?.representedObject = uid
            menu.selectItem(at: menu.numberOfItems - 1)
        }
    }
    private func choice(uid: String, input: Bool) -> AudioDeviceChoice? {
        devices.first { uid.isEmpty ? (input ? $0.defaultInput : $0.defaultOutput) : $0.uid == uid }
    }
    private func populateChannels(_ menu: NSPopUpButton, count: UInt32, selection: UInt32) {
        menu.removeAllItems()
        for channel in 0..<min(count, 128) {
            menu.addItem(withTitle: "\(channel + 1)")
            menu.lastItem?.tag = Int(channel)
        }
        if !menu.selectItem(withTag: Int(selection)) {
            menu.addItem(withTitle: "\(UInt64(selection) + 1) — недоступен")
            menu.lastItem?.tag = Int(selection)
            menu.selectItem(at: menu.numberOfItems - 1)
        }
    }
    private func rebuildControls() {
        populate(inputDevice, uid: draft.inputUID, input: true)
        populate(outputDevice, uid: draft.outputUID, input: false)
        let input = choice(uid: draft.inputUID, input: true)
        let output = choice(uid: draft.outputUID, input: false)
        populateChannels(inputChannel, count: input?.inputs ?? 0, selection: draft.inputChannel)
        populateChannels(outputLeft, count: output?.outputs ?? 0, selection: draft.outputLeft)
        populateChannels(outputRight, count: output?.outputs ?? 0, selection: draft.outputRight)
        inputInfo.stringValue = details(input, title: "Вход")
        outputInfo.stringValue = details(output, title: "Выход")
    }
    private func details(_ device: AudioDeviceChoice?, title: String) -> String {
        guard let device else { return "\(title): устройство недоступно; автоматической замены не будет." }
        let rate = device.sampleRate.isFinite ? String(format: "%.1f", device.sampleRate / 1000) : "?"
        let compatible = device.sampleRate.isFinite && abs(device.sampleRate - 48000) <= 0.5
        return "\(title): \(rate) кГц · буфер \(device.buffer) кадров" + (compatible ? "" : " · требуется 48 кГц")
    }
    @objc func selectionChanged(_ sender: NSPopUpButton) {
        if sender === inputDevice {
            draft.inputUID = sender.selectedItem?.representedObject as? String ?? draft.inputUID
            draft.inputChannel = 0
        } else if sender === outputDevice {
            draft.outputUID = sender.selectedItem?.representedObject as? String ?? draft.outputUID
            draft.outputLeft = 0; draft.outputRight = 1
        } else if let selected = sender.selectedItem, let channel = UInt32(exactly: selected.tag) {
            if sender === inputChannel { draft.inputChannel = channel }
            if sender === outputLeft { draft.outputLeft = channel }
            if sender === outputRight { draft.outputRight = channel }
        }
        rebuildControls()
        status.stringValue = "Настройки изменены, но ещё не применены."
    }
    @objc func apply() {
        do {
            try draft.validate()
            try applyConfiguration(draft)
            status.stringValue = "Настройки сохранены. Доступность устройства и каналов проверяется перед Play / Record."
        } catch { status.stringValue = error.localizedDescription }
    }
    @objc private func openSystemSettings() {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: "com.apple.audio.AudioMIDISetup") else {
            status.stringValue = "Откройте «Настройка Audio-MIDI» из папки «Утилиты»."; return
        }
        NSWorkspace.shared.open(url)
    }
}

extension AudioDevicePreferences {
    func bridgeValue() throws -> daw_audio_device_config {
        try validate()
        var value = daw_audio_device_config()
        value.struct_size = UInt32(MemoryLayout<daw_audio_device_config>.size)
        value.version = UInt32(DAW_AUDIO_DEVICE_CONFIG_VERSION)
        value.input_channel = inputChannel; value.output_left = outputLeft; value.output_right = outputRight
        withUnsafeMutableBytes(of: &value.input_uid) { $0.copyBytes(from: inputUID.utf8CString.map { UInt8(bitPattern: $0) }) }
        withUnsafeMutableBytes(of: &value.output_uid) { $0.copyBytes(from: outputUID.utf8CString.map { UInt8(bitPattern: $0) }) }
        return value
    }
}

extension DraftApp {
    func audioConfigurationError() -> NSError {
        var bytes = [CChar](repeating: 0, count: 512)
        daw_error(session, &bytes, bytes.count)
        return NSError(domain: "MyDAW.AudioSettings", code: 1,
            userInfo: [NSLocalizedDescriptionKey: String(cString: bytes)])
    }
    func restoreAudioDeviceConfiguration(_ target: OpaquePointer) {
        do {
            var raw = try AudioDevicePreferences.read(from: audioPreferences).bridgeValue()
            guard daw_set_audio_device_config(target, &raw) == 0 else { throw audioConfigurationError() }
        } catch {
            setProjectMessage("Настройки аудио не восстановлены: \(error.localizedDescription)")
        }
    }
    func audioDeviceChoices() throws -> [AudioDeviceChoice] {
        var count: UInt32 = 0
        guard daw_refresh_audio_devices(session, &count) == 0 else { throw audioConfigurationError() }
        var choices: [AudioDeviceChoice] = []
        for index in 0..<count {
            var raw = daw_audio_device()
            raw.struct_size = UInt32(MemoryLayout<daw_audio_device>.size)
            raw.version = UInt32(DAW_AUDIO_DEVICE_VERSION)
            guard daw_get_audio_device(session, index, &raw) == 0 else { throw audioConfigurationError() }
            let uid = withUnsafeBytes(of: raw.uid) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            let name = withUnsafeBytes(of: raw.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            choices.append(AudioDeviceChoice(uid: uid, name: name, inputs: raw.input_channels,
                outputs: raw.output_channels, sampleRate: raw.sample_rate, buffer: raw.buffer_frames,
                defaultInput: raw.is_default_input != 0, defaultOutput: raw.is_default_output != 0))
        }
        return choices
    }
    func applyAudioDeviceConfiguration(_ config: AudioDevicePreferences) throws {
        let data = try config.encoded()
        var raw = try config.bridgeValue()
        guard daw_set_audio_device_config(session, &raw) == 0 else { throw audioConfigurationError() }
        audioPreferences.set(data, forKey: AudioDevicePreferences.key)
    }
    @objc func showAudioDeviceSettings() {
        if let settings = audioDeviceSettings {
            settings.showWindow(nil); settings.window?.makeKeyAndOrderFront(nil); return
        }
        let config: AudioDevicePreferences
        do { config = try AudioDevicePreferences.read(from: audioPreferences) }
        catch { config = AudioDevicePreferences() }
        let settings = AudioDeviceSettingsController(configuration: config, load: { [weak self] in
            guard let self else { return [] }
            return try self.audioDeviceChoices()
        }, apply: { [weak self] config in
            guard let self, self.session != nil else { throw AudioDevicePreferences.Failure.invalid }
            try self.applyAudioDeviceConfiguration(config)
        })
        audioDeviceSettings = settings
        settings.window?.center()
        settings.showWindow(nil); settings.window?.makeKeyAndOrderFront(nil)
    }
}
