import AppKit

enum InspectorBrowserTab: Int { case inspector = 0, browser = 1 }
enum InspectorBrowserKind: Int { case audio = 0, plugins = 1 }

struct InspectorChannelModel: Equatable {
    var title = ""
    var kind = "TRACK"
    var renameable = true
    var volumeDb: Double = 0
    var pan: Double = 0
    var muted = false
    var solo = false
    var inserts: [String] = []
    var sends: [String] = []
    var accent: NSColor = .systemBlue
}

struct InspectorClipModel: Equatable {
    var title = ""
    var startFrames: UInt64 = 0
    var sourceOffsetFrames: UInt64 = 0
    var lengthFrames: UInt64 = 0
    var fadeInFrames: UInt64 = 0
    var fadeOutFrames: UInt64 = 0
}

struct InspectorMidiModel: Equatable {
    var clips: [PianoRollClipModel] = []
    var selectedClip: Int?
    var notes: [PianoRollNote] = []
    var editable = false
}

struct InspectorBrowserItem: Equatable, Identifiable {
    var id = UUID()
    var title = ""
    var detail = ""
    var available = true
}

@MainActor
final class InspectorBrowserView: NSView, NSTableViewDataSource, NSTableViewDelegate {
    var channel: InspectorChannelModel? { didSet { reloadInspector() } }
    var clip: InspectorClipModel? { didSet { reloadInspector() } }
    var midi: InspectorMidiModel? { didSet { applyMidi() } }
    var audioItems: [InspectorBrowserItem] = [] { didSet { reloadBrowser() } }
    var pluginItems: [InspectorBrowserItem] = [] { didSet { reloadBrowser() } }
    var onChannelChange: ((Double, Double) -> Void)?
    var onChannelRename: ((String) -> Void)?
    var onChannelMute: ((Bool) -> Void)?
    var onChannelSolo: ((Bool) -> Void)?
    var onClipChange: ((InspectorClipModel) -> Void)?
    var onMidiClipSelect: ((Int) -> Void)?
    var onMidiNotesChange: (([PianoRollNote]) -> Void)?
    var onMidiAddNote: (() -> Void)?
    var onMidiRemoveNote: ((Int) -> Void)?
    var onBrowserSelect: ((InspectorBrowserKind, InspectorBrowserItem?) -> Void)?
    var onAddFolder: (() -> Void)?
    var onImport: (() -> Void)?
    var onAdd: ((InspectorBrowserKind, InspectorBrowserItem?) -> Void)?
    var onScanAU: (() -> Void)?
    var onScanVST3: (() -> Void)?
    var onPreview: ((InspectorBrowserItem?) -> Void)?
    var onStopPreview: (() -> Void)?
    var isImportBusy = false { didSet { updateImportControls() } }

    private let tabs = NSSegmentedControl(labels: ["INSPECTOR", "BROWSER"], trackingMode: .selectOne, target: nil, action: nil)
    private let browserKind = NSSegmentedControl(labels: ["Audio", "Plug-ins"], trackingMode: .selectOne, target: nil, action: nil)
    private let titleLabel = NSTextField(labelWithString: "NO SELECTION")
    private let body = NSStackView()
    private let inspectorForm = NSStackView()
    private let search = NSSearchField()
    private let table = NSTableView()
    private let tableScroll = NSScrollView()
    private let preview = NSButton(title: "Прослушать", target: nil, action: nil)
    private let stopPreview = NSButton(title: "Стоп", target: nil, action: nil)
    private let addFolderButton = NSButton(title: "Add Folder", target: nil, action: nil)
    private let importButton = NSButton(title: "Import", target: nil, action: nil)
    private let addButton = NSButton(title: "Add", target: nil, action: nil)
    private let previewStatus = NSTextField(labelWithString: "Выберите аудиофайл для предпрослушивания.")
    private let volume = NSSlider(value: 0, minValue: -120, maxValue: 24, target: nil, action: nil)
    private let pan = NSSlider(value: 0, minValue: -1, maxValue: 1, target: nil, action: nil)
    private let channelName = NSTextField(string: "")
    private let mute = NSButton(title: "M", target: nil, action: nil)
    private let solo = NSButton(title: "S", target: nil, action: nil)
    private let volumeCaption = NSTextField(labelWithString: "VOLUME")
    private let panCaption = NSTextField(labelWithString: "PAN")
    private let insertSummary = NSTextField(wrappingLabelWithString: "")
    private let sendSummary = NSTextField(wrappingLabelWithString: "")
    private let clipForm = NSStackView()
    private let midiEditor = PianoRollEditorView()
    private var fields: [NSTextField] = []
    private var isAudioPreviewPlaying = false
    private var previewedAudioID: UUID?
    private var audioPreviewError: String?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        setup()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private func setup() {
        tabs.selectedSegment = InspectorBrowserTab.inspector.rawValue; tabs.target = self; tabs.action = #selector(changeTab)
        browserKind.selectedSegment = InspectorBrowserKind.audio.rawValue; browserKind.target = self; browserKind.action = #selector(changeBrowserKind)
        titleLabel.font = DAWDesignTokens.Typography.label; titleLabel.textColor = DAWDesignTokens.Color.secondaryText
        body.orientation = .vertical; body.alignment = .width; body.spacing = DAWDesignTokens.Space.xs
        inspectorForm.orientation = .vertical; inspectorForm.alignment = .leading; inspectorForm.spacing = DAWDesignTokens.Space.xs
        [tabs, titleLabel, body, inspectorForm].forEach { $0.translatesAutoresizingMaskIntoConstraints = false; addSubview($0) }
        NSLayoutConstraint.activate([
            tabs.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10), tabs.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10), tabs.topAnchor.constraint(equalTo: topAnchor, constant: 10),
            titleLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12), titleLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12), titleLabel.topAnchor.constraint(equalTo: tabs.bottomAnchor, constant: 12),
            body.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10), body.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10), body.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 9), body.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -10),
            inspectorForm.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10), inspectorForm.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -10), inspectorForm.topAnchor.constraint(equalTo: titleLabel.bottomAnchor, constant: 9), inspectorForm.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor, constant: -10)
        ])
        body.setHuggingPriority(.defaultLow, for: .vertical)
        volume.target = self; volume.action = #selector(changeChannel); volume.isContinuous = true
        pan.target = self; pan.action = #selector(changeChannel); pan.isContinuous = true
        channelName.target = self; channelName.action = #selector(changeChannelName)
        for control in [mute, solo] { control.setButtonType(.toggle); control.bezelStyle = .texturedRounded; control.target = self }
        mute.action = #selector(changeMute); solo.action = #selector(changeSolo)
        [volumeCaption, panCaption].forEach { $0.font = .systemFont(ofSize: 10, weight: .medium); $0.textColor = .secondaryLabelColor }
        insertSummary.font = .systemFont(ofSize: 10, weight: .medium);sendSummary.font = .systemFont(ofSize: 10, weight: .medium)
        let nameRow=row("NAME",channelName)
        let volumeRow=NSStackView(views:[volumeCaption,volume]);volumeRow.orientation = .vertical;volumeRow.alignment = .leading;volumeRow.spacing=4;volume.widthAnchor.constraint(equalTo:volumeRow.widthAnchor).isActive=true
        let panRow=NSStackView(views:[panCaption,pan]);panRow.orientation = .vertical;panRow.alignment = .leading;panRow.spacing=4;pan.widthAnchor.constraint(equalTo:panRow.widthAnchor).isActive=true
        let buttons=NSStackView(views:[mute,solo]);buttons.spacing=6;buttons.alignment = .centerY
        clipForm.orientation = .vertical;clipForm.alignment = .width;clipForm.spacing=6
        for(index,title) in ["Start · sec","Source offset · sec","Length · sec","Fade in · sec","Fade out · sec"].enumerated(){let field=NSTextField(string:"0.000");field.tag=index;field.target=self;field.action=#selector(changeClip);fields.append(field);clipForm.addArrangedSubview(row(title,field))}
        for view in [nameRow,volumeRow,panRow,buttons,insertSummary,sendSummary,clipForm,midiEditor]{inspectorForm.addArrangedSubview(view);view.widthAnchor.constraint(equalTo:inspectorForm.widthAnchor).isActive=true}
        midiEditor.isHidden = true
        midiEditor.onClipSelect = { [weak self] index in self?.onMidiClipSelect?(index) }
        midiEditor.onNotesChange = { [weak self] notes in self?.onMidiNotesChange?(notes) }
        midiEditor.onAddNote = { [weak self] in self?.onMidiAddNote?() }
        midiEditor.onRemoveNote = { [weak self] row in self?.onMidiRemoveNote?(row) }
        search.target = self; search.action = #selector(filterBrowser)
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("item")); column.title = ""; column.width = 220
        table.addTableColumn(column); table.headerView = nil; table.dataSource = self; table.delegate = self; table.rowHeight = 32
        table.target = self; table.doubleAction = #selector(doubleClickBrowserItem)
        tableScroll.documentView = table; tableScroll.hasVerticalScroller = true; tableScroll.drawsBackground = false
        for (button, action) in [(addFolderButton, #selector(addFolder)), (importButton, #selector(importItem)), (addButton, #selector(addItem))] {
            button.target = self; button.action = action; button.bezelStyle = .texturedRounded; button.font = DAWDesignTokens.Typography.caption
        }
        preview.target = self; preview.action = #selector(previewSelectedAudio); preview.bezelStyle = .texturedRounded; preview.font = DAWDesignTokens.Typography.caption
        stopPreview.target = self; stopPreview.action = #selector(stopAudioPreview); stopPreview.bezelStyle = .texturedRounded; stopPreview.font = DAWDesignTokens.Typography.caption
        previewStatus.font = DAWDesignTokens.Typography.caption; previewStatus.textColor = DAWDesignTokens.Color.secondaryText; previewStatus.lineBreakMode = .byTruncatingTail
        reloadInspector()
    }

    private func clearBody() { body.arrangedSubviews.forEach { body.removeArrangedSubview($0); $0.removeFromSuperview() } }
    private func label(_ text: String, color: NSColor = DAWDesignTokens.Color.secondaryText) -> NSTextField { let value = NSTextField(labelWithString: text); value.font = DAWDesignTokens.Typography.caption; value.textColor = color; return value }
    private func row(_ name: String, _ control: NSView) -> NSStackView {
        let stack = NSStackView(views: [label(name), control]);stack.orientation = .vertical;stack.alignment = .leading;stack.spacing = 4
        control.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        return stack
    }

    private func applyMidi() {
        guard let midi, !midi.clips.isEmpty else {
            midiEditor.editorEnabled = false
            midiEditor.isHidden = true
            return
        }
        midiEditor.isHidden = false
        midiEditor.editorEnabled = midi.editable
        midiEditor.clips = midi.clips
        midiEditor.selectedClip = midi.selectedClip
        midiEditor.notes = midi.notes
    }
    private func reloadInspector() {
        guard tabs.selectedSegment == InspectorBrowserTab.inspector.rawValue else { return }
        clearBody();body.isHidden=true;inspectorForm.isHidden=false
        guard let channel else { titleLabel.stringValue = "NO SELECTION";inspectorForm.isHidden=true;body.isHidden=false;body.addArrangedSubview(label("Выбери дорожку, bus или master для управления каналом."));return }
        titleLabel.stringValue = channel.kind; titleLabel.textColor = channel.accent
        channelName.stringValue = channel.title; channelName.isEditable = channel.renameable; channelName.isEnabled = channel.renameable
        volume.doubleValue = channel.volumeDb; pan.doubleValue = channel.pan; mute.state = channel.muted ? .on : .off; solo.state = channel.solo ? .on : .off
        volumeCaption.stringValue="VOLUME  \(String(format:"%+.1f dB",channel.volumeDb))";panCaption.stringValue="PAN  \(String(format:"%+.2f",channel.pan))"
        insertSummary.stringValue=channel.inserts.isEmpty ? "":"INSERTS\n"+channel.inserts.joined(separator:"\n");insertSummary.isHidden=channel.inserts.isEmpty
        sendSummary.stringValue=channel.sends.isEmpty ? "":"SENDS\n"+channel.sends.joined(separator:"\n");sendSummary.isHidden=channel.sends.isEmpty
        clipForm.isHidden=clip == nil
        if let clip {let values=[clip.startFrames,clip.sourceOffsetFrames,clip.lengthFrames,clip.fadeInFrames,clip.fadeOutFrames];for(index,value)in values.enumerated(){fields[index].stringValue=String(format:"%.3f",Double(value)/48000)}}
    }

    private var visibleItems: [InspectorBrowserItem] {
        let source = browserKind.selectedSegment == InspectorBrowserKind.audio.rawValue ? audioItems : pluginItems
        let query = search.stringValue.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return query.isEmpty ? source : source.filter { $0.title.lowercased().contains(query) || $0.detail.lowercased().contains(query) }
    }
    private func reloadBrowser() {
        guard tabs.selectedSegment == InspectorBrowserTab.browser.rawValue else { return }
        titleLabel.stringValue = "BROWSER"; titleLabel.textColor = .secondaryLabelColor
        table.reloadData()
        updatePreviewControls()
        updateImportControls()
    }
    private func showBrowser() {
        clearBody();body.isHidden=false;inspectorForm.isHidden=true; titleLabel.stringValue = "BROWSER"; titleLabel.textColor = .secondaryLabelColor
        let commands = NSStackView(views: [addFolderButton, importButton, addButton]); commands.spacing = 6
        let scanners = NSStackView(views: [button("Scan AU", #selector(scanAU)), button("Scan VST3", #selector(scanVST3))]); scanners.spacing = 6
        let previewCommands = NSStackView(views: [preview, stopPreview]); previewCommands.spacing = 6
        body.addArrangedSubview(browserKind); body.addArrangedSubview(search); body.addArrangedSubview(commands); body.addArrangedSubview(previewCommands); body.addArrangedSubview(previewStatus); body.addArrangedSubview(scanners); body.addArrangedSubview(tableScroll)
        browserKind.widthAnchor.constraint(equalTo: body.widthAnchor).isActive = true; search.widthAnchor.constraint(equalTo: body.widthAnchor).isActive = true; tableScroll.widthAnchor.constraint(equalTo: body.widthAnchor).isActive = true; tableScroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 220).isActive = true
        table.reloadData()
        updatePreviewControls()
        updateImportControls()
    }
    private func button(_ title: String, _ action: Selector) -> NSButton { let button = NSButton(title: title, target: self, action: action); button.bezelStyle = .texturedRounded; button.font = DAWDesignTokens.Typography.caption; return button }

    @objc private func changeTab() { if tabs.selectedSegment == InspectorBrowserTab.inspector.rawValue { reloadInspector() } else { showBrowser() } }
    @objc private func changeBrowserKind() { table.deselectAll(nil); reloadBrowser() }
    @objc private func filterBrowser() { table.deselectAll(nil); table.reloadData(); updatePreviewControls(); updateImportControls() }
    @objc private func changeChannel() { onChannelChange?(volume.doubleValue, pan.doubleValue) }
    @objc private func changeChannelName() { onChannelRename?(channelName.stringValue) }
    @objc private func changeMute() { onChannelMute?(mute.state == .on) }
    @objc private func changeSolo() { onChannelSolo?(solo.state == .on) }
    @objc private func changeClip() {
        guard var clip, fields.count == 5 else { return }
        let values = fields.map { UInt64(max(0, (Double($0.stringValue.replacingOccurrences(of: ",", with: ".")) ?? 0) * 48000).rounded()) }
        clip.startFrames = values[0]; clip.sourceOffsetFrames = values[1]; clip.lengthFrames = max(1, values[2]); clip.fadeInFrames = min(values[3], clip.lengthFrames); clip.fadeOutFrames = min(values[4], clip.lengthFrames - clip.fadeInFrames); self.clip = clip; onClipChange?(clip)
    }
    @objc private func addFolder() { onAddFolder?() }
    @objc private func importItem() { onImport?() }
    @objc private func addItem() { let kind = InspectorBrowserKind(rawValue: browserKind.selectedSegment) ?? .audio; let row = table.selectedRow; let item = row >= 0 && row < visibleItems.count ? visibleItems[row] : nil; onAdd?(kind, item) }
    @objc private func scanAU() { onScanAU?() }
    @objc private func scanVST3() { onScanVST3?() }
    @objc private func previewSelectedAudio() { onPreview?(selectedAudioItem) }
    @objc private func stopAudioPreview() { onStopPreview?() }
    @objc private func doubleClickBrowserItem() {
        guard browserKind.selectedSegment == InspectorBrowserKind.audio.rawValue,
              preview.isEnabled else { return }
        previewSelectedAudio()
    }

    private var selectedAudioItem: InspectorBrowserItem? {
        guard browserKind.selectedSegment == InspectorBrowserKind.audio.rawValue else { return nil }
        let row = table.selectedRow
        guard row >= 0, row < visibleItems.count else { return nil }
        let item = visibleItems[row]
        return item.available ? item : nil
    }

    private var previewedAudioTitle: String? {
        guard let previewedAudioID else { return nil }
        return audioItems.first(where: { $0.id == previewedAudioID })?.title
    }

    private func updatePreviewControls() {
        let isAudio = browserKind.selectedSegment == InspectorBrowserKind.audio.rawValue
        let selected = selectedAudioItem
        let selectedName = selected?.title
        preview.isHidden = !isAudio
        stopPreview.isHidden = !isAudio
        previewStatus.isHidden = !isAudio
        preview.isEnabled = isAudio && selected != nil && !isAudioPreviewPlaying
        stopPreview.isEnabled = isAudio && isAudioPreviewPlaying
        preview.setAccessibilityLabel(selectedName.map { "Прослушать \($0)" } ?? "Прослушать выбранный аудиофайл")
        preview.setAccessibilityHelp("Только прослушивание выбранного аудиофайла: файл и проект не изменяются.")
        stopPreview.setAccessibilityLabel(previewedAudioTitle.map { "Остановить предпрослушивание \($0)" } ?? "Остановить предпрослушивание")
        stopPreview.setAccessibilityHelp("Останавливает только предпрослушивание; файл и проект не изменяются.")
        previewStatus.setAccessibilityLabel("Статус предпрослушивания")
        if let audioPreviewError, !audioPreviewError.isEmpty {
            previewStatus.stringValue = audioPreviewError
            previewStatus.textColor = .systemRed
        } else if isAudioPreviewPlaying {
            previewStatus.stringValue = "Идёт предпрослушивание: \(previewedAudioTitle ?? selectedName ?? "аудиофайл")"
            previewStatus.textColor = DAWDesignTokens.Color.secondaryText
        } else if let selected {
            previewStatus.stringValue = "Готово к предпрослушиванию: \(selected.title)"
            previewStatus.textColor = DAWDesignTokens.Color.secondaryText
        } else {
            previewStatus.stringValue = "Выберите доступный аудиофайл для предпрослушивания."
            previewStatus.textColor = DAWDesignTokens.Color.secondaryText
        }
    }

    private func updateImportControls() {
        let isAudio = browserKind.selectedSegment == InspectorBrowserKind.audio.rawValue
        let selected = selectedAudioItem
        let name = selected?.title
        addButton.isEnabled = !isAudio || (!isImportBusy && selected != nil)
        importButton.isEnabled = !isImportBusy
        addFolderButton.isEnabled = !isImportBusy
        addButton.setAccessibilityLabel(name.map { isImportBusy ? "Добавить \($0): импорт уже выполняется" : "Добавить \($0) как дорожку" } ?? "Добавить выбранный элемент")
        addButton.setAccessibilityHelp(isImportBusy ? "Новый импорт нельзя начать, пока не завершится или не будет отменён текущий WAV import." : "Добавляет выбранный WAV как новую дорожку через фоновый импорт.")
        importButton.setAccessibilityLabel(isImportBusy ? "Импорт WAV: уже выполняется" : "Выбрать WAV для импорта")
        importButton.setAccessibilityHelp(isImportBusy ? "Сначала заверши или отмени текущий импорт WAV." : "Открывает выбор одного WAV для фонового импорта.")
        addFolderButton.setAccessibilityLabel(isImportBusy ? "Добавить папку: импорт уже выполняется" : "Добавить папку с WAV")
        addFolderButton.setAccessibilityHelp(isImportBusy ? "Сначала заверши или отмени текущий импорт WAV." : "Добавляет одну выбранную папку в Audio Browser.")
    }

    func updateAudioPreview(isPlaying: Bool, selectedID: UUID?, error: String?) {
        isAudioPreviewPlaying = isPlaying
        previewedAudioID = selectedID
        audioPreviewError = error
        if let selectedID,
           let visibleRow = visibleItems.firstIndex(where: { $0.id == selectedID }),
           table.selectedRow != visibleRow {
            table.selectRowIndexes(IndexSet(integer: visibleRow), byExtendingSelection: false)
        }
        updatePreviewControls()
    }

    func numberOfRows(in tableView: NSTableView) -> Int { visibleItems.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let item = visibleItems[row]; let view = NSTableCellView(); let title = NSTextField(labelWithString: item.title); let detail = NSTextField(labelWithString: item.available ? item.detail : "MISSING · " + item.detail); title.font = .systemFont(ofSize: 12, weight: .medium); detail.font = .systemFont(ofSize: 10); detail.textColor = item.available ? .secondaryLabelColor : .systemRed; let stack = NSStackView(views: [title, detail]); stack.orientation = .vertical; stack.spacing = 1; stack.translatesAutoresizingMaskIntoConstraints = false; view.addSubview(stack); NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: view.leadingAnchor, constant: 5), stack.trailingAnchor.constraint(equalTo: view.trailingAnchor, constant: -5), stack.centerYAnchor.constraint(equalTo: view.centerYAnchor)]); return view
    }
    func tableViewSelectionDidChange(_ notification: Notification) { let kind = InspectorBrowserKind(rawValue: browserKind.selectedSegment) ?? .audio; let row = table.selectedRow; let item = row >= 0 && row < visibleItems.count ? visibleItems[row] : nil; onBrowserSelect?(kind, item); updatePreviewControls(); updateImportControls() }
}
