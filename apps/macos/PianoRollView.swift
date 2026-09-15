import AppKit

// Data-only MIDI note row for the v0 table editor. Frame values follow the
// bridge: note starts are CLIP-RELATIVE 48 kHz frames; the session owns the
// clip-relative frames. This view never talks to the bridge itself.
struct PianoRollNote: Equatable {
    var startFrames: UInt64 = 0
    var lengthFrames: UInt64 = 480
    var pitch: UInt8 = 60
    var channel: UInt8 = 0
    var velocity: UInt8 = 100
}

struct PianoRollClipModel: Equatable, Hashable {
    var index: Int = 0
    var startFrames: UInt64 = 0
    var lengthFrames: UInt64 = 0
    var noteCount: UInt32 = 0
    var title: String { "Клип \(index + 1) · \(Self.seconds(startFrames))–\(Self.seconds(startFrames + lengthFrames)) с · \(noteCount) нот" }
    private static func seconds(_ frames: UInt64) -> String { String(format: "%.1f", Double(frames) / 48000) }
}

@MainActor
final class PianoRollEditorView: NSView, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate {
    var notes: [PianoRollNote] = [] { didSet { if notes != oldValue { table.reloadData() } } }
    var clips: [PianoRollClipModel] = [] { didSet { reloadClips() } }
    var selectedClip: Int? { didSet { syncClipSelection() } }
    var editorEnabled = false { didSet { applyEnabled() } }
    var onClipSelect: ((Int) -> Void)?
    var onNotesChange: (([PianoRollNote]) -> Void)?
    var onAddNote: (() -> Void)?
    var onRemoveNote: ((Int) -> Void)?

    private let clipPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let clipCaption = NSTextField(labelWithString: "MIDI-КЛИПЫ")
    private let table = NSTableView()
    private let scroll = NSScrollView()
    private let addButton = NSButton(title: "+ Нота", target: nil, action: nil)
    private let removeButton = NSButton(title: "− Удалить", target: nil, action: nil)
    private let hint = NSTextField(labelWithString: "Отсчёт от начала клипа · ⌘Z отменяет изменение")

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = DAWDesignTokens.Color.raisedSurface.cgColor
        setup()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private func setup() {
        clipCaption.font = DAWDesignTokens.Typography.label; clipCaption.textColor = DAWDesignTokens.Color.secondaryText
        clipPopup.target = self; clipPopup.action = #selector(selectClip)
        clipPopup.font = DAWDesignTokens.Typography.caption
        for title in ["Начало · мс", "Длит. · мс", "Нота", "Vel"] {
            let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier(title)); column.title = title; column.width = title.count < 5 ? 48 : 96
            table.addTableColumn(column)
        }
        table.headerView = NSTableHeaderView(); table.rowHeight = 22
        table.usesAlternatingRowBackgroundColors = true; table.allowsEmptySelection = true
        table.allowsMultipleSelection = false; table.dataSource = self; table.delegate = self
        table.target = self; table.doubleAction = #selector(noop)
        scroll.documentView = table; scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder
        addButton.bezelStyle = .texturedRounded; addButton.font = DAWDesignTokens.Typography.caption
        removeButton.bezelStyle = .texturedRounded; removeButton.font = DAWDesignTokens.Typography.caption
        addButton.target = self; addButton.action = #selector(addNote)
        removeButton.target = self; removeButton.action = #selector(removeNote)
        hint.font = DAWDesignTokens.Typography.caption; hint.textColor = DAWDesignTokens.Color.secondaryText
        let header = NSStackView(views: [clipCaption, clipPopup]); header.orientation = .vertical; header.alignment = .leading; header.spacing = 4
        let controls = NSStackView(views: [addButton, removeButton, hint]); controls.spacing = 6; controls.alignment = .centerY
        let stack = NSStackView(views: [header, scroll, controls]); stack.orientation = .vertical; stack.alignment = .width; stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false; addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6), stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 6), stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
            scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 140)
        ])
        clipCaption.setAccessibilityLabel("Раздел MIDI-редактора")
        clipPopup.setAccessibilityLabel("Выбор MIDI-клипа")
        clipPopup.setAccessibilityHelp("Показывает ноты выбранного клипа дорожки; границы клипа заданы на линейке.")
        addButton.setAccessibilityLabel("Добавить ноту на позицию воспроизведения")
        addButton.setAccessibilityHelp("Вставляет ноту C4 на текущей позиции воспроизведения, 10 мс, скорость 100.")
        removeButton.setAccessibilityLabel("Удалить выбранную ноту")
        removeButton.setAccessibilityHelp("Удаляет только выбранную строку; ⌘Z отменяет удаление.")
        hint.setAccessibilityLabel("Подсказка MIDI-редактора")
        applyEnabled()
    }
    @objc private func noop() {}

    private func reloadClips() {
        clipPopup.removeAllItems()
        for clip in clips { clipPopup.addItem(withTitle: clip.title) }
        syncClipSelection()
        removeButton.isEnabled = editorEnabled && !clips.isEmpty
    }
    private func syncClipSelection() {
        guard !clips.isEmpty else { clipPopup.isEnabled = false; return }
        clipPopup.isEnabled = editorEnabled
        if let selected = selectedClip, clips.contains(where: { $0.index == selected }) {
            clipPopup.selectItem(at: clips.firstIndex(where: { $0.index == selected })!)
        } else {
            clipPopup.selectItem(at: 0)
        }
    }
    private func applyEnabled() {
        addButton.isEnabled = editorEnabled && !clips.isEmpty
        removeButton.isEnabled = editorEnabled && !clips.isEmpty
        clipPopup.isEnabled = editorEnabled && !clips.isEmpty
    }

    @objc private func selectClip() {
        let position = clipPopup.indexOfSelectedItem
        guard position >= 0, position < clips.count else { return }
        onClipSelect?(clips[position].index)
    }
    @objc private func addNote() { onAddNote?() }
    @objc private func removeNote() { let row = table.selectedRow; guard row >= 0, row < notes.count else { return }; onRemoveNote?(row) }

    func numberOfRows(in tableView: NSTableView) -> Int { notes.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row < notes.count, let identifier = tableColumn?.identifier.rawValue else { return nil }
        let note = notes[row]
        let text: String
        switch identifier {
        case "Начало · мс": text = String(format: "%.1f", Double(note.startFrames) / 48)
        case "Длит. · мс": text = String(format: "%.1f", Double(note.lengthFrames) / 48)
        case "Нота": text = Self.noteName(note.pitch)
        default: text = String(note.velocity)
        }
        let view = NSTableCellView()
        let field = NSTextField(string: text)
        field.font = .systemFont(ofSize: 11); field.isBordered = false; field.drawsBackground = false
        field.isEditable = editorEnabled; field.isSelectable = editorEnabled
        field.delegate = self; field.tag = row * 10 + (tableColumn.flatMap { tableView.tableColumns.firstIndex(of: $0) } ?? 0)
        field.target = self; field.action = #selector(cellCommitted(_:))
        field.setAccessibilityLabel("\(identifier) нота \(row + 1)")
        view.addSubview(field); field.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            field.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 2), field.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -2),
            field.centerYAnchor.constraint(equalTo: view.centerYAnchor)
        ])
        return view
    }

    @objc private func cellCommitted(_ sender: NSTextField) {
        let row = sender.tag / 10, column = sender.tag % 10
        guard row < notes.count, column < 4 else { return }
        var note = notes[row]
        let raw = sender.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")
        switch column {
        case 0: if let ms = Double(raw), ms >= 0 { note.startFrames = UInt64((ms * 48).rounded()) }
        case 1: if let ms = Double(raw), ms > 0 { note.lengthFrames = min(UInt64((ms * 48).rounded()), 480000) }
        case 2: if let pitch = Self.parsePitch(raw) { note.pitch = pitch }
        default: if let velocity = Int(raw), (1...127).contains(velocity) { note.velocity = UInt8(velocity) }
        }
        notes[row] = note
        onNotesChange?(notes)
    }

    static func noteName(_ pitch: UInt8) -> String {
        guard pitch <= 127 else { return "?" }
        let names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
        return "\(names[Int(pitch) % 12])\(Int(pitch) / 12 - 1)"
    }
    static func parsePitch(_ raw: String) -> UInt8? {
        if let number = Int(raw), (0...127).contains(number) { return UInt8(number) }
        let upper = raw.uppercased()
        guard let letter = upper.first, "AABCDEFGH".contains(letter) else { return nil }
        var index = upper.index(after: upper.startIndex); let semitone = ["C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11][String(letter)]
        guard var value = semitone else { return nil }
        if index < upper.endIndex, upper[index] == "#" { value += 1; index = upper.index(after: index) }
        guard index < upper.endIndex, let octave = Int(upper[index...]), (0...9).contains(octave) else { return nil }
        let pitch = (octave + 1) * 12 + value
        return (0...127).contains(pitch) ? UInt8(pitch) : nil
    }
}