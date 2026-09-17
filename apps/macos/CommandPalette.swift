import AppKit

@MainActor
private final class DAWCommandPaletteEntry {
    let searchItem: DAWCommandSearchItem
    let action: Selector
    let target: AnyObject
    let menuItem: NSMenuItem

    init(searchItem: DAWCommandSearchItem, action: Selector, target: AnyObject, menuItem: NSMenuItem) {
        self.searchItem = searchItem
        self.action = action
        self.target = target
        self.menuItem = menuItem
    }
}

@MainActor
private final class DAWCommandPaletteSearchField: NSSearchField {
    var onMove: ((Int) -> Void)?
    var onCommit: (() -> Void)?
    var onCancel: (() -> Void)?

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 125: // Down arrow
            onMove?(1)
        case 126: // Up arrow
            onMove?(-1)
        case 36, 76: // Return / keypad Enter
            onCommit?()
        case 53: // Escape
            onCancel?()
        default:
            super.keyDown(with: event)
        }
    }
}

@MainActor
private final class DAWCommandPaletteRowView: NSTableCellView {
    static let identifier = NSUserInterfaceItemIdentifier("DAWCommandPaletteRow")

    private let titleLabel = NSTextField(labelWithString: "")
    private let pathLabel = NSTextField(labelWithString: "")
    private let shortcutLabel = NSTextField(labelWithString: "")

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        identifier = Self.identifier

        titleLabel.font = NSFont.systemFont(ofSize: 13, weight: .medium)
        titleLabel.textColor = .labelColor
        titleLabel.lineBreakMode = .byTruncatingTail
        pathLabel.font = NSFont.systemFont(ofSize: 10, weight: .regular)
        pathLabel.textColor = .secondaryLabelColor
        pathLabel.lineBreakMode = .byTruncatingMiddle
        shortcutLabel.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .medium)
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

    func configure(title: String, path: String, shortcut: String) {
        titleLabel.stringValue = title
        pathLabel.stringValue = path
        shortcutLabel.stringValue = shortcut
        shortcutLabel.isHidden = shortcut.isEmpty
    }
}

/// Keyboard-first command surface. The palette intentionally indexes the
/// application's existing NSMenu tree instead of maintaining a second command
/// registry. Menu validation and target/action remain the authority, so a new
/// menu command becomes searchable without extra palette plumbing.
@MainActor
final class DAWCommandPaletteController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate {
    private let panel: NSPanel
    private let searchField = DAWCommandPaletteSearchField()
    private let tableView = NSTableView()
    private let resultLabel = NSTextField(labelWithString: "")
    private weak var hostWindow: NSWindow?
    private weak var previousFirstResponder: NSResponder?
    private var entries: [DAWCommandPaletteEntry] = []
    private var visibleEntries: [DAWCommandPaletteEntry] = []

    override init() {
        panel = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 620, height: 430),
            styleMask: [.titled, .fullSizeContentView],
            backing: .buffered,
            defer: false
        )
        super.init()
        configurePanel()
    }

    var isVisible: Bool { panel.isVisible }

    func toggle(from window: NSWindow) {
        if isVisible {
            dismiss()
        } else {
            present(from: window)
        }
    }

    func present(from window: NSWindow) {
        // Resolve responder-chain targets before the search field becomes first
        // responder. This preserves commands such as Undo that use nil targets.
        entries = collectMenuCommands()
        hostWindow = window
        previousFirstResponder = window.firstResponder
        searchField.stringValue = ""
        applyFilter()

        let size = panel.frame.size
        let screenFrame = window.screen?.visibleFrame ?? window.frame
        var origin = NSPoint(
            x: window.frame.midX - size.width / 2,
            y: window.frame.maxY - size.height - 88
        )
        origin.x = min(max(origin.x, screenFrame.minX + 20), screenFrame.maxX - size.width - 20)
        origin.y = min(max(origin.y, screenFrame.minY + 20), screenFrame.maxY - size.height - 20)
        panel.setFrameOrigin(origin)

        window.addChildWindow(panel, ordered: .above)
        panel.makeKeyAndOrderFront(nil)
        panel.makeFirstResponder(searchField)
        searchField.selectText(nil)
    }

    func dismiss() {
        guard panel.isVisible else { return }
        let host = hostWindow
        host?.removeChildWindow(panel)
        panel.orderOut(nil)
        host?.makeKeyAndOrderFront(nil)
        if let responder = previousFirstResponder {
            host?.makeFirstResponder(responder)
        }
        hostWindow = nil
        previousFirstResponder = nil
    }

    private func configurePanel() {
        panel.titleVisibility = .hidden
        panel.titlebarAppearsTransparent = true
        panel.isMovableByWindowBackground = false
        panel.isFloatingPanel = true
        panel.hidesOnDeactivate = true
        panel.level = .floating
        panel.hasShadow = true
        panel.standardWindowButton(.closeButton)?.isHidden = true
        panel.standardWindowButton(.miniaturizeButton)?.isHidden = true
        panel.standardWindowButton(.zoomButton)?.isHidden = true
        panel.setAccessibilityLabel("Палитра команд")

        let root = NSVisualEffectView()
        root.material = .hudWindow
        root.blendingMode = .behindWindow
        root.state = .active
        root.translatesAutoresizingMaskIntoConstraints = false
        panel.contentView = root

        searchField.placeholderString = "Найти команду…"
        searchField.font = NSFont.systemFont(ofSize: 17, weight: .medium)
        searchField.focusRingType = .none
        searchField.delegate = self
        searchField.translatesAutoresizingMaskIntoConstraints = false
        searchField.setAccessibilityLabel("Поиск команды")
        searchField.setAccessibilityHelp("Введите название команды, используйте стрелки для выбора и Enter для выполнения")
        searchField.onMove = { [weak self] delta in self?.moveSelection(delta) }
        searchField.onCommit = { [weak self] in self?.invokeSelection() }
        searchField.onCancel = { [weak self] in self?.dismiss() }

        let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("command"))
        column.resizingMask = .autoresizingMask
        tableView.addTableColumn(column)
        tableView.headerView = nil
        tableView.rowHeight = 48
        tableView.intercellSpacing = NSSize(width: 0, height: 2)
        tableView.backgroundColor = .clear
        tableView.selectionHighlightStyle = .regular
        tableView.dataSource = self
        tableView.delegate = self
        tableView.target = self
        tableView.doubleAction = #selector(invokeSelection)
        tableView.setAccessibilityLabel("Результаты палитры команд")

        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = tableView
        scroll.translatesAutoresizingMaskIntoConstraints = false

        resultLabel.font = NSFont.systemFont(ofSize: 10, weight: .regular)
        resultLabel.textColor = .tertiaryLabelColor
        resultLabel.alignment = .left
        resultLabel.translatesAutoresizingMaskIntoConstraints = false

        let hint = NSTextField(labelWithString: "↑↓ выбрать    ↩ выполнить    esc закрыть    ⌘K открыть/закрыть")
        hint.font = NSFont.systemFont(ofSize: 10, weight: .regular)
        hint.textColor = .tertiaryLabelColor
        hint.alignment = .right
        hint.translatesAutoresizingMaskIntoConstraints = false

        root.addSubview(searchField)
        root.addSubview(scroll)
        root.addSubview(resultLabel)
        root.addSubview(hint)
        NSLayoutConstraint.activate([
            searchField.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 18),
            searchField.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -18),
            searchField.topAnchor.constraint(equalTo: root.topAnchor, constant: 18),
            searchField.heightAnchor.constraint(equalToConstant: 34),

            scroll.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 12),
            scroll.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -12),
            scroll.topAnchor.constraint(equalTo: searchField.bottomAnchor, constant: 12),
            scroll.bottomAnchor.constraint(equalTo: resultLabel.topAnchor, constant: -8),

            resultLabel.leadingAnchor.constraint(equalTo: root.leadingAnchor, constant: 18),
            resultLabel.bottomAnchor.constraint(equalTo: root.bottomAnchor, constant: -12),
            hint.trailingAnchor.constraint(equalTo: root.trailingAnchor, constant: -18),
            hint.centerYAnchor.constraint(equalTo: resultLabel.centerYAnchor),
            hint.leadingAnchor.constraint(greaterThanOrEqualTo: resultLabel.trailingAnchor, constant: 12)
        ])
    }

    func controlTextDidChange(_ obj: Notification) {
        applyFilter()
    }

    func numberOfRows(in tableView: NSTableView) -> Int { visibleEntries.count }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard visibleEntries.indices.contains(row) else { return nil }
        let view = (tableView.makeView(withIdentifier: DAWCommandPaletteRowView.identifier, owner: self) as? DAWCommandPaletteRowView)
            ?? DAWCommandPaletteRowView(frame: .zero)
        let item = visibleEntries[row].searchItem
        view.configure(title: item.title, path: item.path, shortcut: item.shortcut)
        return view
    }

    private func applyFilter() {
        let indexed = Dictionary(uniqueKeysWithValues: entries.map { ($0.searchItem.id, $0) })
        let matches = DAWCommandPaletteSearch.results(
            in: entries.map(\.searchItem),
            query: searchField.stringValue
        )
        visibleEntries = matches.compactMap { indexed[$0.id] }
        tableView.reloadData()
        resultLabel.stringValue = visibleEntries.isEmpty
            ? "Команды не найдены"
            : "\(visibleEntries.count) \(visibleEntries.count == 1 ? "команда" : "команд")"
        if !visibleEntries.isEmpty {
            tableView.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
            tableView.scrollRowToVisible(0)
        }
    }

    private func moveSelection(_ delta: Int) {
        guard !visibleEntries.isEmpty else { return }
        let current = tableView.selectedRow >= 0 ? tableView.selectedRow : 0
        let next = min(visibleEntries.count - 1, max(0, current + delta))
        tableView.selectRowIndexes(IndexSet(integer: next), byExtendingSelection: false)
        tableView.scrollRowToVisible(next)
    }

    @objc private func invokeSelection() {
        let row = tableView.selectedRow
        guard visibleEntries.indices.contains(row) else { return }
        let entry = visibleEntries[row]
        let host = hostWindow
        dismiss()

        // Wait until the host window is key again, then invoke the exact target
        // resolved when the palette opened. This avoids routing nil-target menu
        // actions into the palette's own search field.
        DispatchQueue.main.async {
            host?.makeKeyAndOrderFront(nil)
            _ = NSApp.sendAction(entry.action, to: entry.target, from: entry.menuItem)
        }
    }

    private func collectMenuCommands() -> [DAWCommandPaletteEntry] {
        guard let mainMenu = NSApp.mainMenu else { return [] }
        var result: [DAWCommandPaletteEntry] = []
        var ordinal = 0

        func walk(_ menu: NSMenu, parents: [String]) {
            menu.update()
            for item in menu.items {
                guard !item.isSeparatorItem, !item.isHidden else { continue }
                let title = item.title.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !title.isEmpty else { continue }

                if let submenu = item.submenu {
                    walk(submenu, parents: parents + [title])
                    continue
                }
                guard item.isEnabled, let action = item.action else { continue }
                guard let target = NSApp.target(forAction: action, to: item.target, from: item) as AnyObject? else { continue }

                ordinal += 1
                let parentPath = parents.joined(separator: " › ")
                let searchablePath = (parents + [title]).joined(separator: " › ")
                let searchItem = DAWCommandSearchItem(
                    id: "\(ordinal):\(searchablePath):\(NSStringFromSelector(action))",
                    title: title,
                    path: parentPath,
                    shortcut: shortcutText(for: item)
                )
                result.append(DAWCommandPaletteEntry(searchItem: searchItem, action: action, target: target, menuItem: item))
            }
        }

        walk(mainMenu, parents: [])
        return result
    }

    private func shortcutText(for item: NSMenuItem) -> String {
        guard !item.keyEquivalent.isEmpty else { return "" }
        let modifiers = item.keyEquivalentModifierMask.intersection(.deviceIndependentFlagsMask)
        var value = ""
        if modifiers.contains(.control) { value += "⌃" }
        if modifiers.contains(.option) { value += "⌥" }
        if modifiers.contains(.shift) { value += "⇧" }
        if modifiers.contains(.command) { value += "⌘" }

        let key: String
        switch item.keyEquivalent {
        case "\r": key = "↩"
        case "\t": key = "⇥"
        case " ": key = "Space"
        default: key = item.keyEquivalent.uppercased()
        }
        return value + key
    }
}
