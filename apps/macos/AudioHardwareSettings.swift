import AppKit

@MainActor
final class AudioHardwareSettingsController: NSWindowController, NSWindowDelegate {
    let deviceUID: String
    let info = NSTextField(wrappingLabelWithString: "")
    let buffer = NSComboBox()
    let status = NSTextField(wrappingLabelWithString: "")
    let applyButton = NSButton(title: "Установить 48 кГц и буфер", target: nil, action: nil)
    let refreshButton = NSButton(title: "Обновить", target: nil, action: nil)
    private(set) var format: AudioHardwareFormat?
    private(set) var pending = false
    private var timer: Timer?
    private let load: () throws -> AudioHardwareFormat
    private let begin: (UInt32) throws -> Void
    private let poll: () throws -> AudioHardwareChangeResult
    var didFinish: (() -> Void)?

    init(uid: String, load: @escaping () throws -> AudioHardwareFormat,
         begin: @escaping (UInt32) throws -> Void,
         poll: @escaping () throws -> AudioHardwareChangeResult) {
        deviceUID = uid; self.load = load; self.begin = begin; self.poll = poll
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 600, height: 470),
                            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        panel.title = "Частота и аудиобуфер"
        panel.isReleasedWhenClosed = false
        super.init(window: panel)
        panel.delegate = self
        let title = NSTextField(wrappingLabelWithString: "Устройство: \(uid)")
        title.lineBreakMode = .byTruncatingMiddle
        title.maximumNumberOfLines = 1
        let note = NSTextField(wrappingLabelWithString:
            "Проект остаётся 48 кГц. Изменится физическое устройство, в том числе его вход и выход и настройки других приложений. Это не настройка задержки записи. Закройте другие аудиоприложения. Изменения не применяются автоматически при открытии проекта.")
        note.textColor = .secondaryLabelColor
        note.font = .systemFont(ofSize: 11)
        let row = NSStackView(views: [NSTextField(labelWithString: "Буфер, кадров:"), buffer])
        row.spacing = 12
        buffer.widthAnchor.constraint(equalToConstant: 160).isActive = true
        buffer.setAccessibilityLabel("Размер аппаратного аудиобуфера в кадрах")
        buffer.setAccessibilityHelp("Выберите значение или введите целое число из диапазона устройства. Драйвер должен подтвердить его.")
        status.setAccessibilityLabel("Результат изменения аппаратной частоты и буфера")
        let actions = NSStackView(views: [refreshButton, applyButton])
        actions.spacing = 12
        let root = NSStackView(views: [title, info, row, note, actions, status])
        root.orientation = .vertical; root.alignment = .leading; root.spacing = 14
        root.translatesAutoresizingMaskIntoConstraints = false
        panel.contentView?.addSubview(root)
        if let content = panel.contentView {
            NSLayoutConstraint.activate([
                root.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 20),
                root.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -20),
                root.topAnchor.constraint(equalTo: content.topAnchor, constant: 20),
                root.bottomAnchor.constraint(lessThanOrEqualTo: content.bottomAnchor, constant: -20)])
        }
        for label in [title, info, note, status] {
            label.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true
        }
        applyButton.target = self; applyButton.action = #selector(apply)
        applyButton.keyEquivalent = "\r"
        refreshButton.target = self; refreshButton.action = #selector(refresh)
        refresh()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc func refresh() {
        guard !pending else { return }
        do {
            let current = try load()
            format = current
            buffer.removeAllItems()
            let candidates: [UInt32] = [16, 32, 64, 128, 256, 512, 1024, 2048, 4096]
            buffer.addItems(withObjectValues: candidates.filter {
                Double($0) >= current.minimumFrames && Double($0) <= current.maximumFrames
            }.map(String.init))
            buffer.stringValue = String(current.bufferFrames)
            info.stringValue = String(format: "Фактически: %.1f кГц · %u кадров\nДиапазон буфера: %.0f–%.0f кадров; предел приложения — 4096.",
                                      current.sampleRate / 1000, current.bufferFrames, current.minimumFrames, current.maximumFrames)
            applyButton.isEnabled = !current.running
            buffer.isEnabled = !current.running
            status.stringValue = current.running ? AudioHardwareFormat.Failure.busy.localizedDescription : "До нажатия кнопки аппаратные настройки не меняются."
        } catch {
            format = nil; applyButton.isEnabled = false; buffer.isEnabled = false
            info.stringValue = "Текущие параметры устройства недоступны."
            status.stringValue = error.localizedDescription
        }
    }
    private func setPending(_ value: Bool) {
        pending = value
        applyButton.isEnabled = !value && format != nil
        refreshButton.isEnabled = !value
        buffer.isEnabled = !value && format != nil
        window?.standardWindowButton(.closeButton)?.isEnabled = !value
    }
    @objc func apply() {
        guard !pending, let format else { return }
        do {
            buffer.validateEditing() // Commit the real field editor, including Return.
            let frames = try format.requestedFrames(buffer.stringValue)
            try begin(frames)
            setPending(true)
            status.stringValue = "Ожидается подтверждение Core Audio… Play / Record временно недоступны."
            let timer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.pollChange() }
            }
            self.timer = timer
            RunLoop.main.add(timer, forMode: .common)
            pollChange()
        } catch { status.stringValue = error.localizedDescription }
    }
    func pollChange() {
        guard pending else { return }
        do {
            let result = try poll()
            guard result.state != UInt32(DAW_DEVICE_CHANGE_PENDING) else { return }
            timer?.invalidate(); timer = nil
            setPending(false)
            refresh()
            if result.state == UInt32(DAW_DEVICE_CHANGE_APPLIED) && result.actualKnown {
                status.stringValue = String(format: "Core Audio подтвердил %.1f кГц и %u кадров.", result.sampleRate / 1000, result.bufferFrames)
            } else {
                status.stringValue = result.error.isEmpty ? "Изменение не подтверждено. Обновите параметры перед повтором." : result.error
                if result.mayHaveChanged { status.stringValue += " Устройство могло измениться частично; автоматического отката нет." }
            }
            didFinish?()
        } catch {
            timer?.invalidate(); timer = nil
            setPending(false); refresh()
            status.stringValue = "Подтверждение не получено: \(error.localizedDescription). Обновите параметры перед повтором."
            didFinish?()
        }
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { !pending }
    override func close() {
        guard !pending else { return }
        timer?.invalidate(); timer = nil
        super.close()
    }
}

extension DraftApp {
    func hardwareFormat(uid: String) throws -> AudioHardwareFormat {
        var raw = daw_audio_device_capabilities()
        raw.struct_size = UInt32(MemoryLayout<daw_audio_device_capabilities>.size)
        raw.version = UInt32(DAW_AUDIO_DEVICE_CAPABILITIES_VERSION)
        guard daw_get_audio_device_capabilities(session, uid, &raw) == 0 else { throw audioConfigurationError() }
        let ranges = withUnsafeBytes(of: raw.sample_rates) { bytes in
            bytes.bindMemory(to: daw_audio_value_range.self).prefix(Int(raw.rate_count)).map { $0.minimum...$0.maximum }
        }
        return AudioHardwareFormat(sampleRate: raw.sample_rate, bufferFrames: raw.buffer_frames,
            minimumFrames: raw.buffer_minimum, maximumFrames: raw.buffer_maximum, sampleRates: ranges,
            rateWritable: raw.rate_writable != 0, bufferWritable: raw.buffer_writable != 0, running: raw.running != 0)
    }
    func beginHardwareFormat(uid: String, frames: UInt32) throws {
        var request = daw_audio_device_change()
        request.struct_size = UInt32(MemoryLayout<daw_audio_device_change>.size)
        request.version = UInt32(DAW_AUDIO_DEVICE_CHANGE_VERSION)
        request.sample_rate = 48000; request.buffer_frames = frames
        guard !uid.isEmpty, uid.utf8.count <= 480, !uid.contains("\0") else { throw AudioDevicePreferences.Failure.invalid }
        withUnsafeMutableBytes(of: &request.uid) { $0.copyBytes(from: uid.utf8CString.map { UInt8(bitPattern: $0) }) }
        guard daw_begin_audio_device_change(session, &request) == 0 else { throw audioConfigurationError() }
    }
    func pollHardwareFormat() throws -> AudioHardwareChangeResult {
        var raw = daw_audio_device_change_status()
        raw.struct_size = UInt32(MemoryLayout<daw_audio_device_change_status>.size)
        raw.version = UInt32(DAW_AUDIO_DEVICE_CHANGE_VERSION)
        guard daw_poll_audio_device_change(session, &raw) == 0 else { throw audioConfigurationError() }
        let error = withUnsafeBytes(of: raw.error) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
        return AudioHardwareChangeResult(state: raw.state, sampleRate: raw.sample_rate, bufferFrames: raw.buffer_frames,
            actualKnown: raw.actual_known != 0, mayHaveChanged: raw.may_have_changed != 0, error: error)
    }
}
