import AppKit

struct PinnedTrackHeaderModel: Equatable, Identifiable {
    let id: UInt64
    var index: Int
    var name: String
    var accent: NSColor
    var gainDb: Double
    var pan: Double
    var armed: Bool
    var muted: Bool
    var solo: Bool
    var takeCount: Int
    var hasAudio: Bool
    var hasMidi: Bool
    var selected: Bool

    init(
        id: UInt64,
        index: Int,
        name: String,
        accent: NSColor,
        gainDb: Double = 0,
        pan: Double = 0,
        armed: Bool = false,
        muted: Bool = false,
        solo: Bool = false,
        takeCount: Int = 0,
        hasAudio: Bool = false,
        hasMidi: Bool = false,
        selected: Bool = false
    ) {
        self.id = id
        self.index = index
        self.name = name
        self.accent = accent
        self.gainDb = gainDb
        self.pan = pan
        self.armed = armed
        self.muted = muted
        self.solo = solo
        self.takeCount = takeCount
        self.hasAudio = hasAudio
        self.hasMidi = hasMidi
        self.selected = selected
    }
}

@MainActor
final class PinnedTrackHeaderView: NSView, NSTextFieldDelegate, NSDraggingSource {
    var model: PinnedTrackHeaderModel { didSet { renderModel() } }
    var onSelect: ((UInt64) -> Void)?
    var onRename: ((UInt64, String) -> Void)?
    var onArm: ((UInt64, Bool) -> Void)?
    var onMute: ((UInt64, Bool) -> Void)?
    var onSolo: ((UInt64, Bool) -> Void)?
    var onGain: ((UInt64, Double) -> Void)?
    var onPan: ((UInt64, Double) -> Void)?
    var onImportTake: ((UInt64) -> Void)?
    var onComp: ((UInt64) -> Void)?
    var onSplit: ((UInt64) -> Void)?
    var onDuplicate: ((UInt64) -> Void)?
    var onDelete: ((UInt64) -> Void)?
    var onDeleteTrack: ((UInt64) -> Void)?
    var onCrossfade: ((UInt64) -> Void)?
    /// Палитра цвета дорожки открывается контроллером у курсора; результат —
    /// durable uint32 0xRRGGBB (0 = сброс).
    var onTrackColor: ((UInt64) -> Void)?
    var onDuplicateTrack: ((UInt64) -> Void)?
    var onGroupMenu: ((UInt64) -> Void)?
    var onExportTrackWav: ((UInt64) -> Void)?
    var onMidiTranspose: ((UInt64) -> Void)?
    var onMidiQuantize: ((UInt64) -> Void)?
    var onMidiColor: ((UInt64) -> Void)?
    var onMidiMove: ((UInt64) -> Void)?
    var onMidiCopy: ((UInt64) -> Void)?
    /// Zero-based insertion index in the current ordering, before the source
    /// channel is removed. The controller preserves selection by channel ID.
    var onMoveToIndex: ((UInt64, Int) -> Void)?

    private let accentBar = NSView()
    private let numberLabel = NSTextField(labelWithString: "")
    private let nameField = NSTextField(string: "")
    private let statusLabel = NSTextField(labelWithString: "")
    private let gain = NSSlider(value: 0, minValue: -120, maxValue: 24, target: nil, action: nil)
    private let pan = NSSlider(value: 0, minValue: -1, maxValue: 1, target: nil, action: nil)
    private let arm = NSButton(title: "R", target: nil, action: nil)
    private let mute = NSButton(title: "M", target: nil, action: nil)
    private let solo = NSButton(title: "S", target: nil, action: nil)
    private let actionMenu = NSPopUpButton(frame: .zero, pullsDown: true)
    private var dragStart: NSPoint?
    private static let trackPasteboardType = NSPasteboard.PasteboardType("com.mydaw.track-reorder")

    init(model: PinnedTrackHeaderModel) {
        self.model = model
        super.init(frame: .zero)
        wantsLayer = true
        registerForDraggedTypes([Self.trackPasteboardType])
        setup()
        renderModel()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private func setup() {
        translatesAutoresizingMaskIntoConstraints = false
        accentBar.wantsLayer = true
        numberLabel.font = .monospacedDigitSystemFont(ofSize: 10, weight: .semibold)
        numberLabel.textColor = DAWDesignTokens.Color.secondaryText
        nameField.font = DAWDesignTokens.Typography.label
        nameField.textColor = DAWDesignTokens.Color.text
        nameField.drawsBackground = false
        nameField.isBordered = false
        nameField.focusRingType = .none
        nameField.delegate = self
        statusLabel.font = .monospacedDigitSystemFont(ofSize: 9, weight: .regular)
        statusLabel.lineBreakMode = .byTruncatingTail
        statusLabel.textColor = DAWDesignTokens.Color.secondaryText

        gain.isContinuous = true
        gain.target = self
        gain.action = #selector(changeGain)
        pan.isContinuous = true
        pan.target = self
        pan.action = #selector(changePan)
        [arm, mute, solo].forEach { button in
            button.setButtonType(.toggle)
            button.bezelStyle = .inline
            button.font = .systemFont(ofSize: 10, weight: .bold)
            button.contentTintColor = .secondaryLabelColor
            button.target = self
            button.widthAnchor.constraint(equalToConstant: 23).isActive = true
        }
        arm.action = #selector(changeArm)
        mute.action = #selector(changeMute)
        solo.action = #selector(changeSolo)

        actionMenu.addItems(withTitles: ["•••", "Переместить выше", "Переместить ниже", "Import take…", "Comp takes", "Split at playhead", "Duplicate clip", "Crossfade", "Delete clip", "Удалить дорожку", "Дублировать дорожку", "Цвет дорожки…", "Группировка в шину…", "Транспонировать MIDI-клип…", "Квантовать MIDI-клип…", "Цвет MIDI-клипа…", "Перенести MIDI-клип на дорожку…", "Копировать MIDI-клип", "Экспорт дорожки в WAV…"])
        actionMenu.item(at: 0)?.isEnabled = false
        actionMenu.menu?.addItem(.separator())
        actionMenu.target = self
        actionMenu.action = #selector(performMenuAction)
        actionMenu.bezelStyle = .inline
        actionMenu.widthAnchor.constraint(equalToConstant: 34).isActive = true
        actionMenu.toolTip = "Действия с клипом, дублями и дорожкой"
        actionMenu.setAccessibilityLabel("Действия дорожки \(model.name)")
        actionMenu.setAccessibilityHelp("Перемещает дорожку выше или ниже, либо перетащи её за заголовок. Содержит обратимое удаление дорожки.")

        nameField.font = .systemFont(ofSize: 12, weight: .medium)
        nameField.lineBreakMode = .byTruncatingTail
        nameField.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        gain.controlSize = .mini; pan.controlSize = .mini
        gain.isContinuous = false; pan.isContinuous = false
        let row = NSStackView(views: [numberLabel, nameField, actionMenu]); row.spacing = 5
        let controls = NSStackView(views: [arm, mute, solo, gain, pan]); controls.spacing = 3
        pan.widthAnchor.constraint(equalToConstant: 42).isActive = true
        let content = NSStackView(views: [row, controls, statusLabel])
        content.orientation = .vertical; content.alignment = .leading; content.spacing = 3
        row.widthAnchor.constraint(equalTo: content.widthAnchor).isActive = true
        controls.widthAnchor.constraint(equalTo: content.widthAnchor).isActive = true
        content.translatesAutoresizingMaskIntoConstraints = false

        [accentBar, content].forEach(addSubview)
        accentBar.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            accentBar.leadingAnchor.constraint(equalTo: leadingAnchor),
            accentBar.topAnchor.constraint(equalTo: topAnchor),
            accentBar.bottomAnchor.constraint(equalTo: bottomAnchor),
            accentBar.widthAnchor.constraint(equalToConstant: 3),
            content.leadingAnchor.constraint(equalTo: accentBar.trailingAnchor, constant: 7),
            content.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            content.topAnchor.constraint(equalTo: topAnchor, constant: 5),
            content.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor, constant: -5),
            nameField.widthAnchor.constraint(greaterThanOrEqualToConstant: 40),
            gain.widthAnchor.constraint(greaterThanOrEqualToConstant: 45)
        ])
    }

    private func renderModel() {
        layer?.backgroundColor = (model.selected
            ? model.accent.withAlphaComponent(0.17)
            : DAWDesignTokens.Color.surface).cgColor
        accentBar.layer?.backgroundColor = model.accent.cgColor
        numberLabel.stringValue = String(format: "%02d", model.index + 1)
        nameField.stringValue = model.name
        gain.doubleValue = model.gainDb
        pan.doubleValue = model.pan
        arm.state = model.armed ? .on : .off
        mute.state = model.muted ? .on : .off
        solo.state = model.solo ? .on : .off
        arm.contentTintColor = model.armed ? NSColor.systemRed : .secondaryLabelColor
        mute.contentTintColor = model.muted ? model.accent : .secondaryLabelColor
        solo.contentTintColor = model.solo ? NSColor.systemYellow : .secondaryLabelColor
        let media = model.hasAudio ? "AUDIO" : (model.hasMidi ? "MIDI" : "EMPTY")
        let takes = model.takeCount > 0 ? " · \(model.takeCount) TAKE\(model.takeCount == 1 ? "" : "S")" : ""
        statusLabel.stringValue = "\(media) · \(String(format: "%+.1f", model.gainDb)) dB · P \(String(format: "%+.2f", model.pan))\(takes)"
        setAccessibilityLabel("Дорожка \(model.index + 1): \(model.name)")
        setAccessibilityHelp("Перетащи заголовок, чтобы изменить порядок. Меню действий содержит команды перемещения.")
    }

    func controlTextDidEndEditing(_ obj: Notification) {
        let name = nameField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !name.isEmpty, name != model.name else { nameField.stringValue = model.name; return }
        onRename?(model.id, name)
    }

    @objc private func changeGain() { onGain?(model.id, gain.doubleValue) }
    @objc private func changePan() { onPan?(model.id, pan.doubleValue) }
    @objc private func changeArm() { onArm?(model.id, arm.state == .on) }
    @objc private func changeMute() { onMute?(model.id, mute.state == .on) }
    @objc private func changeSolo() { onSolo?(model.id, solo.state == .on) }
    override func mouseDown(with event: NSEvent) {
        dragStart = convert(event.locationInWindow, from: nil)
        onSelect?(model.id)
        super.mouseDown(with: event)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let dragStart else { return }
        let location = convert(event.locationInWindow, from: nil)
        guard hypot(location.x - dragStart.x, location.y - dragStart.y) >= 4 else { return }
        self.dragStart = nil
        let item = NSPasteboardItem()
        item.setString(String(model.id), forType: Self.trackPasteboardType)
        let dragItem = NSDraggingItem(pasteboardWriter: item)
        if let preview = bitmapImageRepForCachingDisplay(in: bounds) {
            cacheDisplay(in: bounds, to: preview)
            dragItem.setDraggingFrame(bounds, contents: preview)
        } else {
            dragItem.setDraggingFrame(bounds, contents: nil)
        }
        beginDraggingSession(with: [dragItem], event: event, source: self)
    }

    func draggingSession(_ session: NSDraggingSession, sourceOperationMaskFor context: NSDraggingContext) -> NSDragOperation {
        .move
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        sender.draggingPasteboard.string(forType: Self.trackPasteboardType) == nil ? [] : .move
    }

    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool {
        sender.draggingPasteboard.string(forType: Self.trackPasteboardType) != nil
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let source = sender.draggingPasteboard.string(forType: Self.trackPasteboardType).flatMap(UInt64.init), source != model.id else { return false }
        let location = convert(sender.draggingLocation, from: nil)
        // AppKit's default view coordinates start at the bottom. Dropping on
        // the lower visual half inserts after this row; the upper half inserts
        // before it.
        let rawDestination = model.index + (location.y < bounds.midY ? 1 : 0)
        onMoveToIndex?(source, rawDestination)
        return true
    }

    @objc private func performMenuAction() {
        switch actionMenu.indexOfSelectedItem {
        case 1: onMoveToIndex?(model.id, model.index - 1)
        case 2: onMoveToIndex?(model.id, model.index + 2)
        case 3: onImportTake?(model.id)
        case 4: onComp?(model.id)
        case 5: onSplit?(model.id)
        case 6: onDuplicate?(model.id)
        case 7: onCrossfade?(model.id)
        case 8: onDelete?(model.id)
        case 9: onDeleteTrack?(model.id)
        case 10: onDuplicateTrack?(model.id)
        case 11: onTrackColor?(model.id)
        case 12: onGroupMenu?(model.id)
        case 13: onMidiTranspose?(model.id)
        case 14: onMidiQuantize?(model.id)
        case 15: onMidiColor?(model.id)
        case 16: onMidiMove?(model.id)
        case 17: onMidiCopy?(model.id)
        case 18: onExportTrackWav?(model.id)
        default: break
        }
    }
}

@MainActor
final class EmptyTimelineLaneView: NSView {
    var message: String { didSet { label.stringValue = message } }
    var accent: NSColor { didSet { accentBar.layer?.backgroundColor = accent.cgColor } }

    private let accentBar = NSView()
    private let label = NSTextField(labelWithString: "")

    init(message: String = "Drop audio here or create a recording", accent: NSColor = .systemBlue) {
        self.message = message
        self.accent = accent
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = NSColor(calibratedRed: 0.045, green: 0.05, blue: 0.06, alpha: 1).cgColor
        accentBar.wantsLayer = true
        accentBar.layer?.backgroundColor = accent.cgColor
        label.stringValue = message
        label.font = .systemFont(ofSize: 11, weight: .medium)
        label.textColor = .tertiaryLabelColor
        [accentBar, label].forEach { $0.translatesAutoresizingMaskIntoConstraints = false; addSubview($0) }
        NSLayoutConstraint.activate([
            accentBar.leadingAnchor.constraint(equalTo: leadingAnchor),
            accentBar.topAnchor.constraint(equalTo: topAnchor),
            accentBar.bottomAnchor.constraint(equalTo: bottomAnchor),
            accentBar.widthAnchor.constraint(equalToConstant: 2),
            label.leadingAnchor.constraint(equalTo: accentBar.trailingAnchor, constant: 10),
            label.centerYAnchor.constraint(equalTo: centerYAnchor),
            label.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -8)
        ])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
}
