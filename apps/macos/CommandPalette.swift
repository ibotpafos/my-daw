import AppKit

@MainActor
private final class DAWCommandPaletteEntry {
    let searchItem: DAWCommandSearchItem
    let action: Selector
    weak var target: AnyObject?
    let menuItem: NSMenuItem
    let tag: Int

    init(searchItem: DAWCommandSearchItem, action: Selector, target: AnyObject, menuItem: NSMenuItem) {
        self.searchItem = searchItem
        self.action = action
        self.target = target
        self.menuItem = menuItem
        self.tag = menuItem.tag
    }
}

@MainActor
enum DAWCommandPaletteKey {
    static func isToggle(_ event: NSEvent) -> Bool {
        let modifiers = event.modifierFlags.intersection([.command, .control, .option, .shift])
        // Physical K also works on the RU layout; Caps Lock is not a modifier
        // that should disable the shortcut. Other command chords are untouched.
        return event.type == .keyDown && modifiers == [.command]
            && (event.keyCode == 40 || event.charactersIgnoringModifiers?.lowercased() == "k")
    }
}

@MainActor
private final class DAWCommandPalettePanel: NSPanel {
    var onCancel: (() -> Void)?

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if DAWCommandPaletteKey.isToggle(event) {
            if !event.isARepeat { onCancel?() }
            return true
        }
        return super.performKeyEquivalent(with: event)
    }

    override func cancelOperation(_ sender: Any?) { onCancel?() }
}

@MainActor
private final class DAWCommandPaletteTableView: NSTableView {
    var onCommit: (() -> Void)?
    var onCancel: (() -> Void)?

    override func keyDown(with event: NSEvent) {
        let modifiers = event.modifierFlags.intersection([.command, .control, .option, .shift])
        if modifiers.isEmpty {
            switch event.keyCode {
            case 36, 76: onCommit?(); return
            case 53: onCancel?(); return
            default: break
            }
        }
        super.keyDown(with: event)
    }
}

@MainActor
private final class DAWCommandPaletteRowView: NSTableCellView {
    static let reuseIdentifier = NSUserInterfaceItemIdentifier("DAWCommandPaletteRow")
    private let titleLabel = NSTextField(labelWithString: "")
    private let pathLabel = NSTextField(labelWithString: "")
    private let shortcutLabel = NSTextField(labelWithString: "")

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        identifier = Self.reuseIdentifier
        titleLabel.font = .systemFont(ofSize: 13, weight: .medium)
        titleLabel.lineBreakMode = .byTruncatingTail
        pathLabel.font = .systemFont(ofSize: 10, weight: .regular)
        pathLabel.textColor = .secondaryLabelColor
        pathLabel.lineBreakMode = .byTruncatingMiddle
        shortcutLabel.font = .monospacedSystemFont(ofSize: 11, weight: .medium)
        shortcutLabel.textColor = .secondaryLabelColor
        shortcutLabel.alignment = .right
        shortcutLabel.setContentCompressionResistancePriority(.required, for: .horizontal)
        let labels = NSStackView(views: [titleLabel, pathLabel])
        labels.orientation = .vertical
        labels.alignment = .leading
        labels.spacing = 2
        labels.translatesAutoresizingMaskIntoConstraints = false
        shortcutLabel.translatesAutoresizingMaskIntoConstraints = false
        addSubview(labels)
        addSubview(shortcutLabel)
        NSLayoutConstraint.activate([
            labels.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10),
            labels.centerYAnchor.constraint(equalTo: centerYAnchor),
            labels.trailingAnchor.constraint(lessThanOrEqualTo: shortcutLabel.leadingAnchor, constant: -12),
            shortcutLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            shortcutLabel.centerYAnchor.constraint(equalTo: centerYAnchor)
        ])
    }

    required init?(coder: NSCoder) { nil }

    func configure(_ item: DAWCommandSearchItem) {
        titleLabel.stringValue = item.title
        pathLabel.stringValue = item.path
        shortcutLabel.stringValue = item.shortcut
        setAccessibilityLabel([item.title, item.path, item.shortcut].filter { !$0.isEmpty }.joined(separator: ", "))
    }
}

/// NSMenu owns availability and target/action. History only orders this view;
/// it never stores executable closures or writes through the C project bridge.
@MainActor
final class DAWCommandPaletteController: NSObject, NSTableViewDataSource, NSTableViewDelegate,
                                         NSSearchFieldDelegate, NSWindowDelegate {
    private let panel = DAWCommandPalettePanel(
        contentRect: NSRect(x: 0, y: 0, width: 620, height: 470),
        styleMask: [.titled, .fullSizeContentView], backing: .buffered, defer: false)
    private let searchField = NSSearchField()
    private let tableView = DAWCommandPaletteTableView()
    private let scopes = NSSegmentedControl(labels: ["Все", "Недавние", "Частые"],
                                            trackingMode: .selectOne, target: nil, action: nil)
    private let clearButton = NSButton(title: "Очистить историю", target: nil, action: nil)
    private let resultLabel = NSTextField(labelWithString: "")
    private let usageStore: DAWCommandUsageStore
    private weak var hostWindow: NSWindow?
    private weak var previousFirstResponder: NSResponder?
    private var entries: [DAWCommandPaletteEntry] = []
    private var visibleEntries: [DAWCommandPaletteEntry] = []
    private var dismissing = false

    init(usageStore: DAWCommandUsageStore = DAWCommandUsageStore()) {
        self.usageStore = usageStore
        super.init()
        configurePanel()
    }

    var isVisible: Bool { hostWindow != nil && panel.isVisible }

    func toggle(from window: NSWindow) {
        if hostWindow != nil { dismiss() } else { present(from: window) }
    }

    func present(from window: NSWindow) {
        guard window.isVisible, window.attachedSheet == nil, NSApp.modalWindow == nil else { return }
        if hostWindow != nil { dismiss(restoreFocus: false) }
        // Resolve the original responder chain before giving search its own
        // field editor. No menu target is ever resolved against the palette.
        hostWindow = window
        previousFirstResponder = window.firstResponder
        entries = collectMenuCommands()
        scopes.selectedSegment = DAWCommandPaletteScope.all.rawValue
        searchField.stringValue = ""
        applyFilter()
        let size = panel.frame.size
        let screen = window.screen?.visibleFrame ?? window.frame
        let maxX = max(screen.minX + 12, screen.maxX - size.width - 12)
        let maxY = max(screen.minY + 12, screen.maxY - size.height - 12)
        panel.setFrameOrigin(NSPoint(
            x: min(max(window.frame.midX - size.width / 2, screen.minX + 12), maxX),
            y: min(max(window.frame.maxY - size.height - 72, screen.minY + 12), maxY)))
        window.addChildWindow(panel, ordered: .above)
        panel.makeKeyAndOrderFront(nil)
        focusSearch()
    }

    func dismiss(restoreFocus: Bool = true) {
        // hidesOnDeactivate may already have hidden the panel. Ownership, not
        // isVisible, is the cleanup guard, so no stale targets survive a close.
        guard hostWindow != nil, !dismissing else { return }
        dismissing = true
        let host = hostWindow
        let responder = previousFirstResponder
        hostWindow = nil
        previousFirstResponder = nil
        entries.removeAll()
        visibleEntries.removeAll()
        host?.removeChildWindow(panel)
        panel.orderOut(nil)
        if restoreFocus, let host, host.isVisible {
            host.makeKeyAndOrderFront(nil)
            if let responder { host.makeFirstResponder(responder) }
        }
        dismissing = false
    }

    func windowDidResignKey(_ notification: Notification) { dismiss(restoreFocus: false) }

    private func configurePanel() {
        panel.delegate = self
        panel.onCancel = { [weak self] in self?.dismiss() }
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.isMovableByWindowBackground = false
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = true
        panel.level = .floating
        panel.hasShadow = true
        panel.isReleasedWhenClosed = false
        panel.standardWindowButton(.closeButton)?.isHidden = true
        panel.standardWindowButton(.miniaturizeButton)?.isHidden = true
        panel.standardWindowButton(.zoomButton)?.isHidden = true
        panel.setAccessibilityLabel("Палитра команд")
        let root = NSVisualEffectView(frame: panel.contentLayoutRect)
        root.material = .hudWindow
        root.blendingMode = .behindWindow
        root.state = .active
        root.autoresizingMask = [.width, .height]
        panel.contentView = root
        searchField.placeholderString = "Найти команду…"
        searchField.font = .systemFont(ofSize: 17, weight: .medium)
        searchField.delegate = self
        searchField.sendsSearchStringImmediately = true
        searchField.setAccessibilityLabel("Поиск команды")
        searchField.setAccessibilityHelp("Введите название. Стрелки выбирают команду, Enter выполняет, Escape закрывает")
        scopes.selectedSegment = 0
        scopes.target = self
        scopes.action = #selector(scopeChanged)
        scopes.setAccessibilityLabel("Все команды, недавние или частые")
        clearButton.target = self
        clearButton.action = #selector(clearHistory)
        clearButton.bezelStyle = .rounded
        clearButton.controlSize = .small
        clearButton.setAccessibilityHelp("Удаляет только локальную историю палитры, не меняя проект")
        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("command"))
        column.width = 590
        tableView.addTableColumn(column)
        tableView.columnAutoresizingStyle = .lastColumnOnlyAutoresizingStyle
        tableView.headerView = nil
        tableView.rowHeight = 48
        tableView.intercellSpacing = NSSize(width: 0, height: 2)
        tableView.backgroundColor = .clear
        tableView.dataSource = self
        tableView.delegate = self
        tableView.target = self
        tableView.doubleAction = #selector(invokeClickedRow)
        tableView.onCommit = { [weak self] in self?.invokeSelection() }
        tableView.onCancel = { [weak self] in self?.dismiss() }
        tableView.setAccessibilityLabel("Результаты палитры команд")
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = tableView
        resultLabel.font = .systemFont(ofSize: 10, weight: .regular)
        resultLabel.textColor = .secondaryLabelColor
        let hint = NSTextField(labelWithString: "↑↓ выбрать    ↩ выполнить    esc / ⌘K закрыть")
        hint.font = .systemFont(ofSize: 10, weight: .regular)
        hint.textColor = .secondaryLabelColor
        for view in [searchField, scopes, clearButton, scroll, resultLabel, hint] as [NSView] {
            view.translatesAutoresizingMaskIntoConstraints = false
            root.addSubview(view)
        }
        NSLayoutConstraint.activate([
            searchField.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 18),
            searchField.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -18),
            searchField.topAnchor.constraint(equalTo: root.topAnchor, constant: 18),
            searchField.heightAnchor.constraint(equalToConstant: 34),
            scopes.leadingAnchor.constraint(equalTo: searchField.leadingAnchor),
            scopes.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 12),
            clearButton.trailingAnchor.constraint(equalTo: searchField.trailingAnchor),
            clearButton.centerYAnchor.constraint(equalTo: scopes.centerYAnchor),
            clearButton.leadingAnchor.constraint(greaterThanOrEqualTo: scopes.trailingAnchor, constant: 12),
            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 12),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -12),
            scroll.topAnchor.constraint(equalTo: scopes.bottomAnchor, constant: 10),
            scroll.bottomAnchor.constraint(equalTo: resultLabel.topAnchor, constant: -8),
            resultLabel.leadingAnchor.constraint(equalTo: searchField.leadingAnchor),
            resultLabel.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -12),
            hint.trailingAnchor.constraint(equalTo: searchField.trailingAnchor),
            hint.centerYAnchor.constraint(equalTo: resultLabel.centerYAnchor),
            hint.leadingAnchor.constraint(greaterThanOrEqualTo: resultLabel.trailingAnchor, constant: 12)
        ])
    }

    // NSSearchField edits through NSTextView; overriding searchField.keyDown
    // alone cannot handle arrows/Return while the user is actually typing.
    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        guard control === searchField, !textView.hasMarkedText() else { return false }
        switch selector {
        case #selector(NSResponder.moveDown(_:)): moveSelection(1)
        case #selector(NSResponder.moveUp(_:)): moveSelection(-1)
        case #selector(NSResponder.insertNewline(_:)): invokeSelection()
        case #selector(NSResponder.cancelOperation(_:)): dismiss()
        default: return false
        }
        return true
    }

    func controlTextDidChange(_ obj: Notification) { applyFilter() }
    func numberOfRows(in tableView: NSTableView) -> Int { visibleEntries.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard visibleEntries.indices.contains(row) else { return nil }
        let view = tableView.makeView(withIdentifier: DAWCommandPaletteRowView.reuseIdentifier, owner: self)
            as? DAWCommandPaletteRowView ?? DAWCommandPaletteRowView(frame: .zero)
        view.configure(visibleEntries[row].searchItem)
        return view
    }

    private func focusSearch() {
        panel.makeFirstResponder(searchField)
        searchField.selectText(nil)
    }

    @objc private func scopeChanged() { applyFilter(); focusSearch() }
    @objc private func clearHistory() { usageStore.clear(); applyFilter(); focusSearch() }

    private func applyFilter() {
        let scope = DAWCommandPaletteScope(rawValue: scopes.selectedSegment) ?? .all
        let commands = usageStore.history.orderedCommands(entries.map(\.searchItem), scope: scope)
        let index = Dictionary(uniqueKeysWithValues: entries.map { ($0.searchItem.id, $0) })
        visibleEntries = DAWCommandPaletteSearch.results(in: commands, query: searchField.stringValue)
            .compactMap { index[$0.id] }
        clearButton.isEnabled = !usageStore.history.entries.isEmpty
        tableView.reloadData()
        resultLabel.stringValue = visibleEntries.isEmpty ? "Нет доступных команд" : "Показано: \(visibleEntries.count)"
        if visibleEntries.isEmpty { tableView.deselectAll(nil) }
        else {
            tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
            tableView.scrollRowToVisible(0)
        }
    }

    private func moveSelection(_ delta: Int) {
        guard !visibleEntries.isEmpty else { return }
        let current = max(0, tableView.selectedRow)
        let next = min(visibleEntries.count - 1, max(0, current + delta))
        tableView.selectRowIndexes(IndexSet(integer: next), byExtendingSelection: false)
        tableView.scrollRowToVisible(next)
    }

    @objc private func invokeClickedRow() {
        guard visibleEntries.indices.contains(tableView.clickedRow) else { return }
        tableView.selectRowIndexes(IndexSet(integer: tableView.clickedRow), byExtendingSelection: false)
        invokeSelection()
    }

    private func invokeSelection() {
        guard visibleEntries.indices.contains(tableView.selectedRow), let host = hostWindow,
              host.isVisible, host.attachedSheet == nil, NSApp.modalWindow == nil else { return }
        let entry = visibleEntries[tableView.selectedRow]
        let target = entry.target
        dismiss()
        // Context can change while the user searches. Restore focus, update the
        // real menu, and require the SAME item, selector and target to remain
        // available. Never substitute a replacement menu item or action target.
        guard let target, collectMenuCommands().contains(where: {
            $0.searchItem.id == entry.searchItem.id && $0.menuItem === entry.menuItem
                && $0.action == entry.action && $0.tag == entry.tag && $0.target === target
        }) else { NSSound.beep(); return }
        if NSApp.sendAction(entry.action, to: target, from: entry.menuItem) {
            usageStore.recordInvocation(id: entry.searchItem.id)
        }
    }

    private func collectMenuCommands() -> [DAWCommandPaletteEntry] {
        guard let mainMenu = NSApp.mainMenu else { return [] }
        var result: [DAWCommandPaletteEntry] = []
        var occurrences: [String: Int] = [:]
        func walk(_ menu: NSMenu, parents: [String]) {
            guard parents.count < 16, result.count < 2048 else { return }
            menu.update()
            for item in menu.items {
                guard result.count < 2048, !item.isSeparatorItem, !item.isHidden, item.isEnabled else { continue }
                let title = item.title.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !title.isEmpty else { continue }
                if let submenu = item.submenu { walk(submenu, parents: parents + [title]); continue }
                guard let action = item.action,
                      action != #selector(DAWWindow.showCommandPalette(_:)),
                      let target = NSApp.target(forAction: action, to: item.target, from: item) else { continue }
                let baseID = DAWCommandIdentity.make(identifier: item.identifier?.rawValue,
                    parents: parents, title: title, selector: NSStringFromSelector(action), tag: item.tag)
                let occurrence = (occurrences[baseID] ?? 0) + 1
                occurrences[baseID] = occurrence
                let id = occurrence == 1 ? baseID : "\(baseID)#\(occurrence)"
                let value = DAWCommandSearchItem(id: id, title: title, path: parents.joined(separator: " › "),
                                                shortcut: shortcutText(for: item))
                result.append(DAWCommandPaletteEntry(searchItem: value, action: action,
                                                     target: target as AnyObject, menuItem: item))
            }
        }
        walk(mainMenu, parents: [])
        return result
    }

    private func shortcutText(for item: NSMenuItem) -> String {
        guard !item.keyEquivalent.isEmpty else { return "" }
        var text = ""
        let flags = item.keyEquivalentModifierMask
        if flags.contains(.control) { text += "⌃" }
        if flags.contains(.option) { text += "⌥" }
        if flags.contains(.shift) { text += "⇧" }
        if flags.contains(.command) { text += "⌘" }
        switch item.keyEquivalent {
        case "\r": text += "↩"
        case "\t": text += "⇥"
        case " ": text += "Space"
        default: text += item.keyEquivalent.uppercased()
        }
        return text
    }
}
