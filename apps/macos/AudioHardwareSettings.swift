import AppKit

struct AudioHardwareProgress {
    let pending: Bool
    let success: Bool
    let message: String
}
protocol AudioHardwareOperation: AnyObject {
    func poll() throws -> AudioHardwareProgress
    func cancel()
}
// The C handle is independent of the session. Its release never blocks and the
// worker retains process-wide exclusion until readback/rollback has finished.
final class AudioHardwareBridgeOperation: AudioHardwareOperation {
    private var handle: OpaquePointer?
    init(_ handle: OpaquePointer) { self.handle = handle }
    deinit { if let handle { daw_release_audio_hardware_change(handle) } }
    func cancel() {
        if let handle { daw_release_audio_hardware_change(handle) }
        handle = nil
    }
    func poll() throws -> AudioHardwareProgress {
        guard let handle else { throw AudioDevicePreferences.Failure.invalid }
        var raw = daw_audio_hardware_status()
        raw.struct_size = UInt32(MemoryLayout<daw_audio_hardware_status>.size)
        raw.version = UInt32(DAW_AUDIO_HARDWARE_VERSION)
        guard daw_poll_audio_hardware_change(handle, &raw) == 0 else { throw AudioDevicePreferences.Failure.invalid }
        if raw.status == 0 { return AudioHardwareProgress(pending: true, success: false, message: "Ожидаем подтверждение драйвера…") }
        let message: String
        if raw.status == 1 {
            message = "Подтверждено устройством: \(String(format: "%.1f", raw.actual_rate / 1000)) кГц · \(raw.actual_buffer) кадров."
        } else {
            let detail = withUnsafeBytes(of: raw.error) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            let recovery = raw.restored != 0 ? "Прежний формат восстановлен." : "Восстановление не подтверждено. Обновите данные и проверьте Audio MIDI Setup."
            message = "Изменение не выполнено. \(recovery)\n\(detail)"
        }
        cancel()
        return AudioHardwareProgress(pending: false, success: raw.status == 1, message: message)
    }
}

@MainActor
struct AudioHardwareService {
    let load: (String) throws -> daw_audio_hardware_settings
    let begin: (daw_audio_hardware_settings, UInt32) throws -> AudioHardwareOperation
}

@MainActor
final class AudioHardwareSettingsController: NSWindowController, NSWindowDelegate {
    let current = NSTextField(wrappingLabelWithString: "")
    let buffer = NSPopUpButton(frame: .zero, pullsDown: false)
    let applyButton = NSButton(title: "Применить к устройству", target: nil, action: nil)
    let refreshButton = NSButton(title: "Обновить", target: nil, action: nil)
    let status = NSTextField(wrappingLabelWithString: "")
    private let uid: String
    private let service: AudioHardwareService
    private(set) var snapshot: daw_audio_hardware_settings?
    private var operation: AudioHardwareOperation?
    private var timer: Timer?
    var onCompletion: (() -> Void)?
    var isApplying: Bool { operation != nil }

    init(uid: String, name: String, service: AudioHardwareService) {
        self.uid = uid; self.service = service
        let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 610, height: 380),
                            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        panel.title = "Формат аудиоустройства"
        panel.isReleasedWhenClosed = false
        super.init(window: panel)
        panel.delegate = self
        let root = NSStackView()
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
        let title = NSTextField(wrappingLabelWithString: name)
        title.font = .boldSystemFont(ofSize: 14)
        let note = NSTextField(wrappingLabelWithString:
            "Целевая частота проекта: 48 кГц. Буфер выбирается в пределах, сообщённых драйвером; окончательное значение проверяется после применения. Это меняет оборудование и может затронуть другие приложения. Запись и мониторинг не включаются.")
        note.font = .systemFont(ofSize: 11); note.textColor = .secondaryLabelColor
        for view in [title, current, note] { root.addArrangedSubview(view) }
        let row = NSStackView(views: [NSTextField(labelWithString: "Буфер, кадров"), buffer])
        row.spacing = 12; root.addArrangedSubview(row)
        buffer.widthAnchor.constraint(greaterThanOrEqualToConstant: 150).isActive = true
        buffer.setAccessibilityLabel("Запрашиваемый размер аппаратного буфера, кадров")
        buffer.setAccessibilityHelp("Это не полная задержка входа и выхода. Драйвер должен подтвердить выбранное значение.")
        root.addArrangedSubview(NSStackView(views: [refreshButton, applyButton]))
        root.addArrangedSubview(status)
        for view in [title, current, note, status] { view.widthAnchor.constraint(equalTo: root.widthAnchor).isActive = true }
        status.font = .systemFont(ofSize: 11)
        status.setAccessibilityLabel("Результат изменения аппаратного формата")
        refreshButton.target = self; refreshButton.action = #selector(refresh)
        applyButton.target = self; applyButton.action = #selector(apply)
        // No Return default: changing machine hardware needs an explicit action.
        refresh()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    static func buffers(_ value: daw_audio_hardware_settings) -> [UInt32] {
        guard value.minimum_buffer > 0, value.minimum_buffer <= value.maximum_buffer else { return [] }
        let candidates: [UInt32] = [32, 64, 128, 256, 512, 1024, 2048, 4096,
                                    value.minimum_buffer, min(value.maximum_buffer, 4096), value.buffer_frames]
        return Set(candidates.filter { $0 >= value.minimum_buffer && $0 <= value.maximum_buffer && $0 <= 4096 }).sorted()
    }
    private func setBusy(_ busy: Bool) {
        refreshButton.isEnabled = !busy
        buffer.isEnabled = !busy && snapshot.map { $0.buffer_writable != 0 } == true
        applyButton.isEnabled = !busy && snapshot != nil && buffer.numberOfItems > 0
    }
    @objc func refresh() {
        guard !isApplying else { return }
        snapshot = nil; buffer.removeAllItems()
        do {
            let value = try service.load(uid)
            let identity = withUnsafeBytes(of: value.uid) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
            guard value.struct_size == UInt32(MemoryLayout<daw_audio_hardware_settings>.size),
                  value.version == UInt32(DAW_AUDIO_HARDWARE_VERSION), identity == uid else {
                throw AudioDevicePreferences.Failure.invalid
            }
            current.stringValue = "Сейчас: \(String(format: "%.1f", value.sample_rate / 1000)) кГц · \(value.buffer_frames) кадров\nДиапазон буфера драйвера: \(value.minimum_buffer)…\(value.maximum_buffer). Предел движка: 4096."
            for frames in Self.buffers(value) where value.buffer_writable != 0 || frames == value.buffer_frames {
                buffer.addItem(withTitle: String(frames)); buffer.lastItem?.tag = Int(frames)
            }
            buffer.selectItem(withTag: Int(value.buffer_frames))
            let at48 = value.sample_rate.isFinite && abs(value.sample_rate - 48000) <= 0.5
            if !at48 && (value.supports_48k == 0 || value.rate_writable == 0) {
                status.stringValue = "Драйвер не позволяет установить 48 кГц. Выберите совместимое устройство или проверьте его панель управления."
            } else {
                snapshot = value
                status.stringValue = "Изменение доступно только при остановленном аудиодвижке."
            }
        } catch {
            current.stringValue = "Не удалось прочитать актуальный аппаратный формат."
            status.stringValue = error.localizedDescription
        }
        setBusy(false)
    }
    @objc func apply() {
        guard !isApplying, let snapshot, let item = buffer.selectedItem, let frames = UInt32(exactly: item.tag) else { return }
        do {
            operation = try service.begin(snapshot, frames)
            setBusy(true)
            status.stringValue = "Применяем параметры; ожидаем подтверждение драйвера…"
            timer = Timer(timeInterval: 0.05, target: self, selector: #selector(poll), userInfo: nil, repeats: true)
            if let timer { RunLoop.main.add(timer, forMode: .common) }
        } catch { status.stringValue = error.localizedDescription }
    }
    @objc func poll() {
        guard let operation else { return }
        do {
            let progress = try operation.poll()
            guard !progress.pending else { return }
            operation.cancel(); self.operation = nil
            timer?.invalidate(); timer = nil
            refresh()
            status.stringValue = progress.message
            onCompletion?()
        } catch {
            operation.cancel(); self.operation = nil
            timer?.invalidate(); timer = nil
            self.snapshot = nil; setBusy(false)
            status.stringValue = "Ошибка проверки: \(error.localizedDescription). Обновите настройки."
        }
    }
    func windowWillClose(_ notification: Notification) {
        operation?.cancel(); operation = nil
        timer?.invalidate(); timer = nil
        snapshot = nil
    }
}

extension DraftApp {
    func audioHardwareService() -> AudioHardwareService {
        AudioHardwareService(load: { [weak self] uid in
            guard let self, self.session != nil else { throw AudioDevicePreferences.Failure.invalid }
            var value = daw_audio_hardware_settings()
            value.struct_size = UInt32(MemoryLayout<daw_audio_hardware_settings>.size)
            value.version = UInt32(DAW_AUDIO_HARDWARE_VERSION)
            guard daw_get_audio_hardware_settings(self.session, uid, &value) == 0 else { throw self.audioConfigurationError() }
            return value
        }, begin: { [weak self] expected, buffer in
            guard let self, self.session != nil else { throw AudioDevicePreferences.Failure.invalid }
            var value = expected
            guard let job = daw_begin_audio_hardware_change(self.session, &value, buffer) else { throw self.audioConfigurationError() }
            return AudioHardwareBridgeOperation(job)
        })
    }
}
