import AppKit

enum LibraryCategory: Int, CaseIterable {
    case audio, instruments, effects
    var title: String { switch self { case .audio: return "Аудио"; case .instruments: return "Инстр."; case .effects: return "Эффекты" } }
    var symbol: String { switch self { case .audio: return "waveform"; case .instruments: return "pianokeys"; case .effects: return "slider.horizontal.3" } }
}

/// The existing browser callbacks and import/preview services remain the only
/// command path. The browser is no longer mutually exclusive with the inspector.
@MainActor
final class LibraryBrowserView: NSView, NSTableViewDataSource, NSTableViewDelegate {
    var audioItems: [InspectorBrowserItem] = [] { didSet { reload() } }
    var pluginItems: [InspectorBrowserItem] = [] { didSet { reload() } }
    var onBrowserSelect: ((InspectorBrowserKind, InspectorBrowserItem?) -> Void)?
    var onAddFolder: (() -> Void)?
    var onImport: (() -> Void)?
    var onAdd: ((InspectorBrowserKind, InspectorBrowserItem?) -> Void)?
    var onScanAU: (() -> Void)?
    var onScanVST3: (() -> Void)?
    var onPreview: ((InspectorBrowserItem?) -> Void)?
    var onStopPreview: (() -> Void)?
    var isImportBusy = false { didSet { updateControls() } }
    var mutationEnabled = true { didSet { if mutationEnabled != oldValue { updateControls() } } }
    private(set) var category = LibraryCategory.audio
    private(set) var visibleItems: [InspectorBrowserItem] = []
    let search = NSSearchField()
    let table = NSTableView()
    let categories = NSSegmentedControl(labels: LibraryCategory.allCases.map(\.title), trackingMode: .selectOne, target: nil, action: nil)
    let addButton = NSButton(title: "Добавить", target: nil, action: nil)
    let previewButton = NSButton(title: "Прослушать", target: nil, action: nil)
    let stopButton = NSButton(title: "Стоп", target: nil, action: nil)
    private let addFolderButton = NSButton(title: "Папка…", target: nil, action: nil)
    private let importButton = NSButton(title: "Импорт…", target: nil, action: nil)
    private let count = NSTextField(labelWithString: "БИБЛИОТЕКА")
    private let empty = NSTextField(wrappingLabelWithString: "")
    private let previewStatus = NSTextField(wrappingLabelWithString: "")
    private var playing = false
    private var selectedPreviewID: UUID?
    private var previewError: String?
    private var reloading = false
    var selectedItem: InspectorBrowserItem? {
        let row = table.selectedRow
        return visibleItems.indices.contains(row) ? visibleItems[row] : nil
    }
    var browserKind: InspectorBrowserKind { category == .audio ? .audio : .plugins }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        let heading = NSTextField(labelWithString: "Библиотека")
        heading.font = .systemFont(ofSize: 17, weight: .semibold)
        heading.textColor = DAWDesignTokens.Color.text
        categories.selectedSegment = 0; categories.controlSize = .small
        categories.target = self; categories.action = #selector(changeCategory)
        categories.setAccessibilityLabel("Тип содержимого библиотеки")
        search.placeholderString = "Поиск файлов и плагинов"
        search.target = self; search.action = #selector(filter)
        search.sendsSearchStringImmediately = true; search.sendsWholeSearchString = false
        search.setAccessibilityLabel("Поиск в библиотеке")
        count.font = .systemFont(ofSize: 10, weight: .semibold); count.textColor = DAWDesignTokens.Color.secondaryText
        let commands = NSStackView(views: [addFolderButton, importButton, addButton]); commands.spacing = 5
        let scanAU = NSButton(title: "Scan AU", target: self, action: #selector(scanAudioUnits))
        let scanVST = NSButton(title: "Scan VST3", target: self, action: #selector(scanVST3))
        let scanners = NSStackView(views: [scanAU, scanVST]); scanners.spacing = 5
        for button in [addFolderButton, importButton, addButton, previewButton, stopButton, scanAU, scanVST] {
            button.bezelStyle = .texturedRounded; button.font = .systemFont(ofSize: 11, weight: .medium)
            button.controlSize = .small
        }
        addFolderButton.target = self; addFolderButton.action = #selector(addFolder)
        importButton.target = self; importButton.action = #selector(importAudio)
        addButton.target = self; addButton.action = #selector(addSelected)
        previewButton.target = self; previewButton.action = #selector(previewSelected)
        stopButton.target = self; stopButton.action = #selector(stopPreview)
        addButton.contentTintColor = DAWDesignTokens.Color.accent
        addFolderButton.toolTip = "Добавить явно выбранную папку WAV/AIFF в библиотеку"
        scanAU.toolTip = "Просканировать установленные Audio Units"
        scanVST.toolTip = "Просканировать VST3; требуется сборка с SDK"
        let column = NSTableColumn(identifier: .init("item")); column.width = 240
        table.addTableColumn(column); table.headerView = nil; table.rowHeight = 44
        table.intercellSpacing = NSSize(width: 0, height: 2)
        table.backgroundColor = DAWDesignTokens.Color.surface; table.style = .sourceList
        table.dataSource = self; table.delegate = self; table.target = self
        table.doubleAction = #selector(activateSelected)
        table.setDraggingSourceOperationMask(.copy, forLocal: false)
        table.setDraggingSourceOperationMask(.copy, forLocal: true)
        table.setAccessibilityLabel("Файлы и установленные плагины")
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.drawsBackground = false; scroll.borderType = .noBorder
        empty.font = .systemFont(ofSize: 12); empty.textColor = DAWDesignTokens.Color.secondaryText
        empty.alignment = .center; empty.maximumNumberOfLines = 4
        previewStatus.font = .systemFont(ofSize: 11); previewStatus.textColor = DAWDesignTokens.Color.secondaryText
        previewStatus.maximumNumberOfLines = 2; previewStatus.lineBreakMode = .byTruncatingTail
        let previewCommands = NSStackView(views: [previewButton, stopButton]); previewCommands.spacing = 5
        let stack = NSStackView(views: [heading, categories, search, commands, scanners, count, scroll, empty, previewCommands, previewStatus])
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 10
        stack.translatesAutoresizingMaskIntoConstraints = false; addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12)
        ])
        for view in [categories, search, scroll, empty, previewStatus] { view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true }
        scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 40).isActive = true
        reload()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func selectCategory(_ value: LibraryCategory) {
        category = value; categories.selectedSegment = value.rawValue
        table.deselectAll(nil); reload(); onBrowserSelect?(browserKind, nil)
    }
    func setQuery(_ query: String) { search.stringValue = query; reload() }
    func selectItem(id: UUID) {
        guard let index = visibleItems.firstIndex(where: { $0.id == id }) else { return }
        table.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
    }
    private func reload() {
        let selection = selectedItem?.id
        let items = category == .audio ? audioItems : pluginItems.filter { $0.category == category }
        let query = search.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        visibleItems = query.isEmpty ? items : items.filter { $0.title.localizedCaseInsensitiveContains(query) || $0.detail.localizedCaseInsensitiveContains(query) }
        reloading = true
        table.reloadData()
        if let selection, let index = visibleItems.firstIndex(where: { $0.id == selection }) {
            table.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
        } else { table.deselectAll(nil) }
        reloading = false
        count.stringValue = "\(category.title.uppercased()) · \(visibleItems.count)"
        empty.isHidden = !visibleItems.isEmpty
        empty.stringValue = !query.isEmpty ? "Ничего не найдено. Попробуйте другой запрос." : (category == .audio ? "Добавьте папку или импортируйте WAV/AIFF. Здесь появятся ваши файлы." : "Установленные плагины появятся после Scan AU / Scan VST3.")
        if selection != selectedItem?.id { onBrowserSelect?(browserKind, selectedItem) }
        updateControls()
    }
    private func updateControls() {
        let audio = category == .audio
        addButton.isEnabled = mutationEnabled && selectedItem?.available == true && (!audio || !isImportBusy)
        importButton.isEnabled = mutationEnabled && !isImportBusy
        addFolderButton.isEnabled = !isImportBusy
        previewButton.isHidden = !audio; stopButton.isHidden = !audio && !playing
        previewButton.isEnabled = audio && selectedItem?.available == true && !playing
        stopButton.isEnabled = playing
        previewStatus.stringValue = previewError ?? (playing ? "Предпрослушивание: \(audioItems.first { $0.id == selectedPreviewID }?.title ?? "аудиофайл")" : "Двойной клик: прослушать аудио / добавить плагин. WAV/AIFF можно перетащить на дорожку.")
        previewStatus.textColor = previewError == nil ? DAWDesignTokens.Color.secondaryText : DAWDesignTokens.Color.coral
        addButton.setAccessibilityLabel(selectedItem.map { "Добавить \($0.title)" } ?? "Добавить выбранный элемент")
    }
    func updateAudioPreview(isPlaying: Bool, selectedID: UUID?, error: String?) {
        playing = isPlaying; selectedPreviewID = selectedID; previewError = error; updateControls()
    }
    @objc private func changeCategory() { if let category = LibraryCategory(rawValue: categories.selectedSegment) { selectCategory(category) } }
    @objc private func filter() { reload() }
    @objc private func addFolder() { onAddFolder?() }
    @objc private func importAudio() { guard importButton.isEnabled else { return }; onImport?() }
    @objc func addSelected() { guard addButton.isEnabled, let item = selectedItem else { return }; onAdd?(browserKind, item) }
    @objc func previewSelected() { guard previewButton.isEnabled else { return }; onPreview?(selectedItem) }
    @objc private func stopPreview() { onStopPreview?() }
    @objc private func scanAudioUnits() { onScanAU?() }
    @objc private func scanVST3() { onScanVST3?() }
    @objc private func activateSelected() { category == .audio ? previewSelected() : addSelected() }
    func numberOfRows(in tableView: NSTableView) -> Int { visibleItems.count }
    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !reloading else { return }
        onBrowserSelect?(browserKind, selectedItem); updateControls()
    }
    func tableView(_ tableView: NSTableView, pasteboardWriterForRow row: Int) -> NSPasteboardWriting? {
        guard visibleItems.indices.contains(row), category == .audio, visibleItems[row].available,
              let url = visibleItems[row].sourceURL else { return nil }
        return url as NSURL
    }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard visibleItems.indices.contains(row) else { return nil }
        let item = visibleItems[row]
        let cell = NSTableCellView()
        let icon = NSImageView(image: NSImage(systemSymbolName: category.symbol, accessibilityDescription: category.title) ?? NSImage())
        icon.contentTintColor = item.available ? DAWDesignTokens.Color.accent : DAWDesignTokens.Color.coral
        let title = NSTextField(labelWithString: item.title); title.font = .systemFont(ofSize: 12, weight: .medium)
        let detail = NSTextField(labelWithString: item.available ? item.detail : "Недоступен · " + item.detail)
        detail.font = .systemFont(ofSize: 10); detail.textColor = item.available ? DAWDesignTokens.Color.secondaryText : DAWDesignTokens.Color.coral
        for field in [title, detail] { field.lineBreakMode = .byTruncatingTail; field.setContentCompressionResistancePriority(.defaultLow, for: .horizontal) }
        let text = NSStackView(views: [title, detail]); text.orientation = .vertical; text.alignment = .leading; text.spacing = 3
        let stack = NSStackView(views: [icon, text]); stack.spacing = 10; stack.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(stack)
        NSLayoutConstraint.activate([icon.widthAnchor.constraint(equalToConstant: 20),
                                     stack.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 6),
                                     stack.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -6),
                                     stack.centerYAnchor.constraint(equalTo: cell.centerYAnchor)])
        return cell
    }
}
