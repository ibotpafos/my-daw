import AppKit

/// All actions still use the existing import, audition and plug-in host paths.
/// Favorites are local preferences; filtering never adds/removes project media.
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
    private(set) var collection = LibraryCollection.all
    private(set) var formatFilter: LibraryFormat?
    private(set) var availableOnly = false
    private(set) var visibleItems: [InspectorBrowserItem] = []
    let favorites: LibraryFavorites
    let folder: LibraryFolderController
    let folderBar: LibraryFolderBar
    let search = NSSearchField()
    let table = LibraryTableView()
    let categories = NSSegmentedControl(labels: LibraryCategory.allCases.map(\.title), trackingMode: .selectOne, target: nil, action: nil)
    let collections = NSSegmentedControl(labels: ["Все", "Избранное"], trackingMode: .selectOne, target: nil, action: nil)
    let formats = NSPopUpButton()
    let availableButton = NSButton()
    let addButton = NSButton(title: "Добавить", target: nil, action: nil)
    let previewButton = NSButton(title: "Слушать", target: nil, action: nil)
    let stopButton = NSButton(title: "Стоп", target: nil, action: nil)
    private let addFolderButton = NSButton(title: "", target: nil, action: nil)
    private let importButton = NSButton(title: "Импорт…", target: nil, action: nil)
    private let scanAU = NSButton(title: "Сканировать AU", target: nil, action: nil)
    private let scanVST = NSButton(title: "Сканировать VST3", target: nil, action: nil)
    private let count = NSTextField(labelWithString: "БИБЛИОТЕКА")
    private let empty = NSTextField(wrappingLabelWithString: "")
    private let previewStatus = NSTextField(wrappingLabelWithString: "")
    private let scroll = NSScrollView()
    private var playing = false
    private var selectedPreviewID: UUID?
    private var previewError: String?
    private var favoriteError: String?
    private var reloading = false
    var selectedItem: InspectorBrowserItem? {
        let row = table.selectedRow
        return visibleItems.indices.contains(row) ? visibleItems[row] : nil
    }
    var browserKind: InspectorBrowserKind { category == .audio ? .audio : .plugins }
    var emptyMessage: String? { empty.isHidden ? nil : empty.stringValue }

    override convenience init(frame: NSRect) { self.init(frame: frame, defaults: .standard) }
    init(frame: NSRect, defaults: UserDefaults) {
        favorites = LibraryFavorites(defaults: defaults)
        folder = LibraryFolderController(defaults: defaults)
        folderBar = LibraryFolderBar(controller: folder)
        super.init(frame: frame)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        let heading = NSTextField(labelWithString: "Библиотека")
        heading.font = .systemFont(ofSize: 17, weight: .semibold)
        heading.textColor = DAWDesignTokens.Color.text
        categories.selectedSegment = 0; categories.controlSize = .small
        categories.target = self; categories.action = #selector(changeCategory)
        categories.setAccessibilityLabel("Тип содержимого библиотеки")
        collections.selectedSegment = 0; collections.controlSize = .small
        collections.target = self; collections.action = #selector(changeCollection)
        collections.setAccessibilityLabel("Все ресурсы или избранное текущего каталога")
        search.placeholderString = "Имя, формат, производитель…"
        search.target = self; search.action = #selector(filter)
        search.sendsSearchStringImmediately = true; search.sendsWholeSearchString = false
        search.setAccessibilityLabel("Поиск в библиотеке")
        formats.controlSize = .small; formats.font = .systemFont(ofSize: 11)
        formats.target = self; formats.action = #selector(changeFormat)
        formats.setAccessibilityLabel("Формат ресурса")
        availableButton.setButtonType(.pushOnPushOff)
        availableButton.bezelStyle = .regularSquare; availableButton.isBordered = false
        availableButton.image = NSImage(systemSymbolName: "checkmark.circle", accessibilityDescription: nil)
        availableButton.target = self; availableButton.action = #selector(changeAvailability)
        availableButton.toolTip = "Показывать только доступные ресурсы"
        availableButton.setAccessibilityLabel("Только доступные ресурсы")
        let filters = NSStackView(views: [formats, availableButton]); filters.spacing = 6
        availableButton.widthAnchor.constraint(equalToConstant: 26).isActive = true
        formats.setContentHuggingPriority(.defaultLow, for: .horizontal)
        count.font = .systemFont(ofSize: 10, weight: .semibold); count.textColor = DAWDesignTokens.Color.secondaryText
        let commands = NSStackView(views: [addFolderButton, importButton, addButton]); commands.spacing = 5
        for button in [addFolderButton, importButton, addButton, previewButton, stopButton, scanAU, scanVST] {
            button.bezelStyle = .texturedRounded; button.font = .systemFont(ofSize: 11, weight: .medium)
            button.controlSize = .small
        }
        addFolderButton.image = NSImage(systemSymbolName: "folder.badge.plus", accessibilityDescription: "Выбрать папку WAV/AIFF")
        addFolderButton.widthAnchor.constraint(equalToConstant: 28).isActive = true
        addFolderButton.target = self; addFolderButton.action = #selector(addFolder)
        importButton.target = self; importButton.action = #selector(importAudio)
        addButton.target = self; addButton.action = #selector(addSelected)
        previewButton.target = self; previewButton.action = #selector(previewSelected)
        stopButton.target = self; stopButton.action = #selector(stopPreview)
        scanAU.target = self; scanAU.action = #selector(scanAudioUnits)
        scanVST.target = self; scanVST.action = #selector(scanVST3)
        let scanMenu = WorkspaceCommandMenu("Сканировать плагины…", sources: [scanAU, scanVST])
        addButton.contentTintColor = DAWDesignTokens.Color.accent
        addFolderButton.toolTip = "Открыть явно выбранную папку WAV/AIFF в библиотеке"
        let column = NSTableColumn(identifier: .init("item")); column.width = 240
        column.minWidth = 100; column.resizingMask = .autoresizingMask
        table.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
        table.addTableColumn(column); table.headerView = nil; table.rowHeight = 44
        table.intercellSpacing = NSSize(width: 0, height: 2)
        table.backgroundColor = DAWDesignTokens.Color.surface; table.style = .plain
        table.dataSource = self; table.delegate = self; table.target = self
        table.doubleAction = #selector(activateSelected)
        table.contextMenuForRow = { [weak self] row in self?.contextMenu(forRow: row) }
        table.setDraggingSourceOperationMask(.copy, forLocal: false)
        table.setDraggingSourceOperationMask(.copy, forLocal: true)
        table.setAccessibilityLabel("Файлы и установленные плагины")
        scroll.documentView = table; scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true; scroll.drawsBackground = false; scroll.borderType = .noBorder
        empty.font = .systemFont(ofSize: 12); empty.textColor = DAWDesignTokens.Color.secondaryText
        empty.alignment = .center; empty.maximumNumberOfLines = 4
        previewStatus.font = .systemFont(ofSize: 11); previewStatus.textColor = DAWDesignTokens.Color.secondaryText
        previewStatus.maximumNumberOfLines = 3
        let previewCommands = NSStackView(views: [previewButton, stopButton]); previewCommands.spacing = 5
        let stack = NSStackView(views: [heading, categories, search, collections, filters, commands, folderBar, scanMenu, count, scroll, empty, previewCommands, previewStatus])
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 9
        stack.translatesAutoresizingMaskIntoConstraints = false; addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12)
        ])
        for view in [categories, collections, search, filters, folderBar, scanMenu, scroll, empty, previewStatus] {
            view.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        }
        scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 40).isActive = true
        rebuildFormats(); reload()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func layout() {
        super.layout()
        let width = scroll.contentSize.width
        guard width > 0 else { return }
        // NSTableColumn starts wider than a compact sidebar. Fit the actual
        // viewport, not its initial width, so row actions remain fully visible.
        if abs(table.frame.width - width) > 0.5 {
            table.setFrameSize(NSSize(width: width, height: table.frame.height))
        }
        table.sizeLastColumnToFit()
    }

    func selectCategory(_ value: LibraryCategory) {
        guard category != value else { return }
        reloading = true; table.deselectAll(nil); reloading = false
        category = value; categories.selectedSegment = value.rawValue
        formatFilter = nil; rebuildFormats(); reload()
        onBrowserSelect?(browserKind, nil)
    }
    func selectCollection(_ value: LibraryCollection) {
        collection = value; collections.selectedSegment = value.rawValue; reload()
    }
    func selectFormat(_ value: LibraryFormat?) {
        formatFilter = value.flatMap { category.formats.contains($0) ? $0 : nil }
        formats.selectItem(at: formatFilter.flatMap { category.formats.firstIndex(of: $0).map { $0 + 1 } } ?? 0)
        reload()
    }
    func setAvailableOnly(_ value: Bool) { availableOnly = value; availableButton.state = value ? .on : .off; reload() }
    func setQuery(_ query: String) { search.stringValue = query; reload() }
    func selectItem(id: UUID) {
        guard let index = visibleItems.firstIndex(where: { $0.id == id }) else { return }
        table.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
    }
    func toggleFavorite(id: UUID) {
        guard let item = visibleItems.first(where: { $0.id == id }), let key = item.resourceKey else { return }
        favoriteError = favorites.set(key, favorite: !favorites.contains(key)) ? nil : "Избранное заполнено: не более \(LibraryFavorites.limit) ресурсов."
        reload()
    }
    private func rebuildFormats() {
        formats.removeAllItems(); formats.addItem(withTitle: "Все форматы")
        formats.addItems(withTitles: category.formats.map(\.title))
    }
    private func reload() {
        let previous = selectedItem
        let origin = scroll.contentView.bounds.origin
        let items = category == .audio ? audioItems : pluginItems.filter { $0.category == category }
        let query = search.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        visibleItems = items.filter { item in
            (!availableOnly || item.available) && (formatFilter == nil || item.format == formatFilter) &&
            (collection == .all || favorites.contains(item.resourceKey)) &&
            LibrarySearch.matches(query, in: [item.title, item.detail, item.format.title, item.sourceURL?.path ?? ""])
        }
        reloading = true
        table.reloadData()
        let index = previous.flatMap { old in
            visibleItems.firstIndex { item in item.id == old.id || (old.resourceKey != nil && item.resourceKey == old.resourceKey) }
        }
        if let index { table.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false) }
        else { table.deselectAll(nil) }
        reloading = false
        var viewport = scroll.contentView.bounds; viewport.origin = origin
        scroll.contentView.scroll(to: scroll.contentView.constrainBoundsRect(viewport).origin)
        scroll.reflectScrolledClipView(scroll.contentView)
        let favoriteCount = items.filter { favorites.contains($0.resourceKey) }.count
        collections.setLabel("Избранное \(favoriteCount)", forSegment: 1)
        count.stringValue = "\(category.title.uppercased()) · \(visibleItems.count) / \(items.count)"
        empty.isHidden = !visibleItems.isEmpty
        if !query.isEmpty || formatFilter != nil || availableOnly {
            empty.stringValue = "Ничего не найдено. Измените запрос или фильтры."
        } else if collection == .favorites {
            empty.stringValue = "Нажмите ☆ рядом с ресурсом. Избранное показывается из текущего каталога."
        } else {
            empty.stringValue = category == .audio ? "Выберите папку с WAV/AIFF. Файлы можно прослушать и перетащить на дорожку." : "Запустите сканирование установленных плагинов."
        }
        // UUID changes after a scan, and availability may change with the same
        // UUID. Both must rebind the controller's current dispatch target.
        if previous != selectedItem { onBrowserSelect?(browserKind, selectedItem) }
        updateControls()
    }
    private func updateControls() {
        let audio = category == .audio
        addButton.isEnabled = mutationEnabled && selectedItem?.available == true && (!audio || !isImportBusy)
        importButton.isEnabled = mutationEnabled && !isImportBusy
        addFolderButton.isEnabled = mutationEnabled && !isImportBusy
        folderBar.enabled = mutationEnabled && !isImportBusy
        folder.isPublishingAllowed = mutationEnabled && !isImportBusy
        scanAU.isEnabled = mutationEnabled; scanVST.isEnabled = mutationEnabled
        previewButton.isHidden = !audio; stopButton.isHidden = !audio && !playing
        previewButton.isEnabled = mutationEnabled && audio && selectedItem?.available == true && !playing
        stopButton.isEnabled = playing
        availableButton.contentTintColor = availableOnly ? DAWDesignTokens.Color.accent : DAWDesignTokens.Color.secondaryText
        let help = audio ? "Пробел — слушать / стоп\nEnter — добавить дорожку" : "Enter — добавить плагин\n☆ — сохранить в избранном"
        previewStatus.stringValue = favoriteError ?? previewError ?? (playing ? "Играет: \(audioItems.first { $0.id == selectedPreviewID }?.title ?? "аудиофайл")" : help)
        previewStatus.textColor = favoriteError == nil && previewError == nil ? DAWDesignTokens.Color.secondaryText : DAWDesignTokens.Color.coral
        addButton.setAccessibilityLabel(selectedItem.map { "Добавить \($0.title)" } ?? "Добавить выбранный элемент")
    }
    func updateAudioPreview(isPlaying: Bool, selectedID: UUID?, error: String?) {
        playing = isPlaying; selectedPreviewID = selectedID; previewError = error; updateControls()
    }
    /// The window calls this before global transport/menu keys, only outside text
    /// editing. Native table arrow navigation and search-field typing are retained.
    func handleFocusedKey(_ event: NSEvent) -> Bool {
        guard let focused = window?.firstResponder as? NSView else { return false }
        let modifiers = event.modifierFlags.intersection(.deviceIndependentFlagsMask).subtracting([.capsLock, .numericPad, .function])
        if focused.isDescendant(of: folderBar) {
            if (event.keyCode == 51 || event.keyCode == 117), modifiers.isEmpty || modifiers == [.command] { return true }
            if event.keyCode == 49, modifiers.isEmpty, let button = focused as? NSButton {
                if !event.isARepeat { button.performClick(nil) }
                return true
            }
            return false
        }
        guard focused === table || focused.isDescendant(of: table) else { return false }
        if (event.keyCode == 51 || event.keyCode == 117), modifiers.isEmpty || modifiers == [.command] { return true }
        guard modifiers.isEmpty else { return false }
        switch event.keyCode {
        case 49:
            if let button = focused as? NSButton {
                if !event.isARepeat { button.performClick(nil) }
                return true
            }
            if !event.isARepeat, category == .audio { playing ? stopPreview() : previewSelected() }
            return true
        case 36, 76: if !event.isARepeat { addSelected() }; return true
        case 115, 119:
            if let item = event.keyCode == 115 ? visibleItems.first : visibleItems.last {
                selectItem(id: item.id); table.scrollRowToVisible(table.selectedRow)
            }
            return true
        default: return false
        }
    }
    func contextMenu(forRow row: Int) -> NSMenu? {
        guard visibleItems.indices.contains(row) else { return nil }
        let item = visibleItems[row]
        let menu = NSMenu(); menu.autoenablesItems = false
        func entry(_ title: String, _ action: LibraryMenuPayload.Action, enabled: Bool) {
            let command = NSMenuItem(title: title, action: #selector(dispatchMenu(_:)), keyEquivalent: "")
            command.target = self; command.representedObject = LibraryMenuPayload(item, action)
            command.isEnabled = enabled; menu.addItem(command)
        }
        entry(favorites.contains(item.resourceKey) ? "Убрать из избранного" : "В избранное", .favorite, enabled: item.resourceKey != nil)
        menu.addItem(.separator())
        entry(category == .audio ? "Добавить аудиодорожку" : "Добавить плагин", .add,
              enabled: mutationEnabled && item.available && (category != .audio || !isImportBusy))
        if category == .audio { entry("Прослушать", .preview, enabled: mutationEnabled && item.available && !playing) }
        return menu
    }
    @objc func dispatchMenu(_ command: NSMenuItem) {
        guard let payload = command.representedObject as? LibraryMenuPayload,
              let item = visibleItems.first(where: { $0.id == payload.itemID && $0.resourceKey == payload.resourceKey }) else { return }
        switch payload.action {
        case .favorite: toggleFavorite(id: item.id)
        case .add:
            guard mutationEnabled, item.available, category != .audio || !isImportBusy else { return }
            selectItem(id: item.id); addSelected()
        case .preview:
            guard mutationEnabled, category == .audio, item.available, !playing else { return }
            selectItem(id: item.id); previewSelected()
        }
    }
    @objc private func changeCategory() { if let category = LibraryCategory(rawValue: categories.selectedSegment) { selectCategory(category) } }
    @objc private func changeCollection() { if let collection = LibraryCollection(rawValue: collections.selectedSegment) { selectCollection(collection) } }
    @objc private func changeFormat() { selectFormat(category.formats.indices.contains(formats.indexOfSelectedItem - 1) ? category.formats[formats.indexOfSelectedItem - 1] : nil) }
    @objc private func changeAvailability() { setAvailableOnly(availableButton.state == .on) }
    @objc private func filter() { reload() }
    @objc private func addFolder() {
        guard addFolderButton.isEnabled else { return }
        // The active workspace owns a background folder coordinator. Retain
        // the legacy callback only for unconfigured/embedded browser hosts.
        if folder.isConfigured { folder.choose(in: window) } else { onAddFolder?() }
    }
    @objc private func importAudio() { guard importButton.isEnabled else { return }; onImport?() }
    @objc func addSelected() { guard addButton.isEnabled, let item = selectedItem else { return }; onAdd?(browserKind, item) }
    @objc func previewSelected() { guard previewButton.isEnabled else { return }; onPreview?(selectedItem) }
    @objc private func stopPreview() { onStopPreview?() }
    @objc private func scanAudioUnits() { guard scanAU.isEnabled else { return }; onScanAU?() }
    @objc private func scanVST3() { guard scanVST.isEnabled else { return }; onScanVST3?() }
    @objc private func activateSelected() { category == .audio ? previewSelected() : addSelected() }
    func numberOfRows(in tableView: NSTableView) -> Int { visibleItems.count }
    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !reloading else { return }
        onBrowserSelect?(browserKind, selectedItem); updateControls()
    }
    func tableView(_ tableView: NSTableView, pasteboardWriterForRow row: Int) -> NSPasteboardWriting? {
        guard visibleItems.indices.contains(row), category == .audio, visibleItems[row].available,
              let url = visibleItems[row].sourceURL, url.isFileURL else { return nil }
        return url as NSURL
    }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard visibleItems.indices.contains(row) else { return nil }
        let item = visibleItems[row]
        let identifier = NSUserInterfaceItemIdentifier("library.resource")
        let cell = tableView.makeView(withIdentifier: identifier, owner: self) as? LibraryItemCell ?? LibraryItemCell(frame: .zero)
        cell.configure(item, favorite: favorites.contains(item.resourceKey))
        cell.onFavorite = { [weak self] in self?.toggleFavorite(id: item.id) }
        return cell
    }
}
