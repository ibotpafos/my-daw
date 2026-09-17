import AppKit

/// A view-only preflight. Session still validates every actual graph mutation.
/// Walking the entire destination chain also refuses malformed/cyclic snapshots.
enum MixerRoutingPolicy {
    static func outputBlockReason(row: MixerStripModel, destinationID: UInt64,
                                  strips: [MixerStripModel]) -> String? {
        guard row.kind != .master else { return "Master has no internal output destination." }
        var next = destinationID
        var visited = Set<UInt64>()
        while next != 0 {
            if row.kind == .bus && next == row.id { return "This route would create feedback." }
            guard visited.insert(next).inserted else { return "The destination contains a routing cycle." }
            guard let bus = strips.first(where: { $0.kind == .bus && $0.id == next }) else {
                return "The destination is no longer available."
            }
            next = bus.outputID
        }
        return nil
    }
}

@MainActor
private final class MixerRoutingTable: NSTableView {
    weak var matrix: MixerRoutingMatrixView?
    override func keyDown(with event: NSEvent) {
        let modifiers = event.modifierFlags.intersection([.command, .option, .control, .shift])
        guard modifiers.isEmpty, let matrix else { super.keyDown(with: event); return }
        switch event.keyCode {
        case 123: matrix.moveFocus(rows: 0, columns: -1)
        case 124: matrix.moveFocus(rows: 0, columns: 1)
        case 125: matrix.moveFocus(rows: 1, columns: 0)
        case 126: matrix.moveFocus(rows: -1, columns: 0)
        case 36, 76, 49: matrix.activateFocusedCell()
        case 51, 117: matrix.removeFocusedSend()
        default: super.keyDown(with: event)
        }
    }
}

/// Two native view-based tables share vertical scrolling. Channel names and
/// column headers remain visible; NSTableView creates/reuses onscreen cells.
/// Native buttons expose individual routing cells to accessibility clients.
@MainActor
final class MixerRoutingMatrixView: NSView, NSSearchFieldDelegate, NSTableViewDataSource, NSTableViewDelegate {
    enum Mode: Int { case main, sends }
    struct Cell: Equatable { let rowID: UInt64; let destinationID: UInt64 }
    private enum Intent { case primary, remove, toggleTap }

    var strips: [MixerStripModel] = [] {
        didSet { if oldValue != strips { generation &+= 1; rebuild() } }
    }
    var onOutput: ((UInt64, UInt64) -> Void)?
    var onSend: ((UInt64, MixerSendAction) -> Void)?
    /// Refresh and check the owning UI on the main thread immediately before an edit.
    var onWillInteract: (() -> Void)?
    var editingAllowed: (() -> Bool)?
    var editingEnabled = true {
        didSet { if oldValue != editingEnabled { routingTable.reloadData(); updateStatus() } }
    }

    private let mode = NSSegmentedControl(labels: ["Main outputs", "Sends"], trackingMode: .selectOne, target: nil, action: nil)
    let search = NSSearchField()
    let routingScroll = NSScrollView()
    let channelScroll = NSScrollView()
    let routingTable: NSTableView = MixerRoutingTable()
    let channelTable: NSTableView = MixerRoutingTable()
    private let status = NSTextField(wrappingLabelWithString: "")
    private let empty = NSTextField(labelWithString: "")
    private var synchronizingScroll = false
    private var synchronizingSelection = false
    private var generation: UInt64 = 0
    private(set) var activeMode: Mode = .main
    private(set) var rows: [MixerStripModel] = []
    private(set) var destinations: [MixerStripModel] = []
    private(set) var focusedCell: Cell?

    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = DAWDesignTokens.Color.canvas.cgColor
        mode.selectedSegment = 0; mode.target = self; mode.action = #selector(modeChanged)
        search.placeholderString = "Find channel / output"; search.delegate = self
        mode.setAccessibilityLabel("Routing matrix mode")
        search.setAccessibilityLabel("Find routing channel or output")
        for (table, scroll) in [(routingTable, routingScroll), (channelTable, channelScroll)] {
            (table as? MixerRoutingTable)?.matrix = self
            table.delegate = self; table.dataSource = self
            table.headerView = NSTableHeaderView(frame: NSRect(x: 0, y: 0, width: 100, height: 30))
            table.rowHeight = 28; table.intercellSpacing = .zero
            table.usesAutomaticRowHeights = false; table.usesAlternatingRowBackgroundColors = true
            table.allowsMultipleSelection = false; table.allowsEmptySelection = true
            table.allowsColumnReordering = false; table.style = .plain
            table.backgroundColor = DAWDesignTokens.Color.canvas
            table.columnAutoresizingStyle = .noColumnAutoresizing
            scroll.documentView = table; scroll.drawsBackground = false
            // Matching, non-autohiding horizontal scrollers keep both row viewports aligned.
            scroll.scrollerStyle = .legacy; scroll.autohidesScrollers = false
            scroll.hasHorizontalScroller = true; scroll.hasVerticalScroller = table === routingTable
            scroll.horizontalScrollElasticity = .none; scroll.verticalScrollElasticity = .none
            scroll.contentView.postsBoundsChangedNotifications = true
            NotificationCenter.default.addObserver(self, selector: #selector(scrollChanged(_:)),
                name: NSView.boundsDidChangeNotification, object: scroll.contentView)
        }
        let names = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("channel"))
        names.title = "CHANNEL"; names.width = 178; names.resizingMask = []
        channelTable.addTableColumn(names)
        channelTable.setAccessibilityLabel("Pinned channel names")
        routingTable.setAccessibilityLabel("Routing destinations. Use arrows, then Return or Space.")
        status.font = .systemFont(ofSize: 11); status.textColor = DAWDesignTokens.Color.secondaryText
        status.maximumNumberOfLines = 2
        empty.textColor = DAWDesignTokens.Color.secondaryText; empty.alignment = .center
        for view in [mode, search, channelScroll, routingScroll, status, empty] { addSubview(view) }
        rebuild()
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func controlTextDidChange(_ obj: Notification) { rebuild() }
    @objc private func modeChanged() { setMode(Mode(rawValue: mode.selectedSegment) ?? .main) }
    func setMode(_ value: Mode) {
        guard activeMode != value else { return }
        activeMode = value; mode.selectedSegment = value.rawValue; generation &+= 1; rebuild()
    }
    func setSearch(_ text: String) { search.stringValue = text; rebuild() }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.modifierFlags.intersection([.command, .option, .control, .shift]) == .command,
           event.charactersIgnoringModifiers?.lowercased() == "f" {
            window?.makeFirstResponder(search); return true
        }
        return super.performKeyEquivalent(with: event)
    }

    private func rebuild() {
        let oldDestinationIDs = destinations.map(\.id)
        let oldDestinationNames = destinations.map(\.title)
        let query = search.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)
        rows = strips.filter {
            (activeMode == .sends ? $0.kind == .track : $0.kind != .master) &&
            (query.isEmpty || $0.title.localizedCaseInsensitiveContains(query) || $0.outputName.localizedCaseInsensitiveContains(query))
        }
        let buses = strips.filter { $0.kind == .bus }
        destinations = activeMode == .main ? [MixerStripModel(id: 0, kind: .master, title: "Master")] + buses : buses
        synchronizingSelection = true
        defer { synchronizingSelection = false }
        if oldDestinationIDs != destinations.map(\.id) || oldDestinationNames != destinations.map(\.title) {
            for column in routingTable.tableColumns { routingTable.removeTableColumn(column) }
            for destination in destinations {
                let column = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("destination-\(destination.id)"))
                column.title = destination.title; column.headerToolTip = destination.title
                column.width = 112; column.minWidth = 88; column.maxWidth = 240
                column.resizingMask = .userResizingMask
                routingTable.addTableColumn(column)
            }
        }
        let rowID = rows.first { $0.id == focusedCell?.rowID }?.id ?? rows.first { $0.isSelected }?.id ?? rows.first?.id
        let destinationID = destinations.first { $0.id == focusedCell?.destinationID }?.id ?? destinations.first?.id
        focusedCell = rowID.flatMap { row in destinationID.map { Cell(rowID: row, destinationID: $0) } }
        channelTable.reloadData(); routingTable.reloadData()
        if let focusedCell, let row = rows.firstIndex(where: { $0.id == focusedCell.rowID }) {
            channelTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
            routingTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        } else { channelTable.deselectAll(nil); routingTable.deselectAll(nil) }
        empty.stringValue = rows.isEmpty ? "No matching channels" : "Create a bus to add sends"
        empty.isHidden = !rows.isEmpty && !destinations.isEmpty
        updateStatus(); needsLayout = true
    }
    private func updateStatus() {
        status.stringValue = editingEnabled
            ? "\(rows.count) channels · \(destinations.count) destinations. Arrows navigate; Return/Space connects or edits. Delete removes a send. Routing changes may stop playback."
            : "Read-only while recording or another edit is active. Search and navigation remain available."
    }
    @objc private func scrollChanged(_ notification: Notification) {
        guard !synchronizingScroll, let source = notification.object as? NSClipView else { return }
        synchronizingScroll = true; defer { synchronizingScroll = false }
        let target = source === routingScroll.contentView ? channelScroll : routingScroll
        var origin = target.contentView.bounds.origin; origin.y = source.bounds.origin.y
        if target === channelScroll { origin.x = 0 }
        target.contentView.scroll(to: origin); target.reflectScrolledClipView(target.contentView)
    }
    func numberOfRows(in tableView: NSTableView) -> Int { rows.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row index: Int) -> NSView? {
        guard rows.indices.contains(index) else { return nil }
        let row = rows[index]
        if tableView === channelTable {
            let identifier = NSUserInterfaceItemIdentifier("routing-name")
            let label = tableView.makeView(withIdentifier: identifier, owner: self) as? NSTextField ?? NSTextField(labelWithString: "")
            label.identifier = identifier; label.stringValue = row.title
            label.font = .systemFont(ofSize: 12, weight: row.kind == .bus ? .semibold : .regular)
            label.textColor = row.color ?? DAWDesignTokens.Color.text
            label.lineBreakMode = .byTruncatingTail; label.toolTip = row.title
            label.frame.size = NSSize(width: max(1, (tableColumn?.width ?? 178) - 8), height: 24)
            label.setAccessibilityLabel("\(row.title), \(row.kind == .bus ? "bus" : "track")")
            return label
        }
        guard let tableColumn, let column = routingTable.tableColumns.firstIndex(of: tableColumn),
              destinations.indices.contains(column) else { return nil }
        let destination = destinations[column]
        let identifier = NSUserInterfaceItemIdentifier("routing-cell")
        let button = tableView.makeView(withIdentifier: identifier, owner: self) as? MixerActionButton ?? MixerActionButton("")
        button.identifier = identifier
        let send = row.sends.first { $0.busID == destination.id }
        let connected = activeMode == .main ? row.outputID == destination.id : send != nil
        let reason = blockReason(rowID: row.id, destinationID: destination.id)
        button.title = reason != nil ? "—" : activeMode == .main ? (connected ? "● MAIN" : "Connect")
            : send.map { "\($0.preFader ? "PRE" : "POST") \(MixerScale.label($0.gainDb))" } ?? "+ Send"
        button.isEnabled = editingEnabled && reason == nil
        button.contentTintColor = connected ? .systemMint : .secondaryLabelColor
        button.setAccessibilityLabel("\(row.title) → \(destination.title), \(activeMode == .main ? "main output" : "send")")
        button.setAccessibilityValue(connected ? (send.map { "\($0.preFader ? "Pre-fader" : "Post-fader"), \(MixerScale.label($0.gainDb)) dB" } ?? "Connected") : "Not connected")
        button.toolTip = reason ?? (activeMode == .main ? "Route \(row.title) to \(destination.title)." : connected ? "Edit send level. Right-click for PRE/POST or Remove." : "Add a post-fader send at −12 dB.")
        button.setAccessibilityHelp(button.toolTip)
        button.wantsLayer = true
        button.layer?.borderWidth = focusedCell == Cell(rowID: row.id, destinationID: destination.id) ? 1.5 : 0
        button.layer?.borderColor = NSColor.controlAccentColor.cgColor; button.layer?.cornerRadius = 4
        let token = generation
        button.invoke = { [weak self] in
            self?.perform(.primary, rowID: row.id, destinationID: destination.id, expectedGeneration: token)
        }
        button.contextMenu = { [weak self] in self?.sendMenu(rowID: row.id, destinationID: destination.id) ?? NSMenu() }
        return button
    }
    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !synchronizingSelection, let table = notification.object as? NSTableView,
              rows.indices.contains(table.selectedRow), let destination = focusedCell?.destinationID ?? destinations.first?.id else { return }
        focus(rowID: rows[table.selectedRow].id, destinationID: destination, reveal: false)
    }
    func focus(rowID: UInt64, destinationID: UInt64, reveal: Bool = true) {
        guard let row = rows.firstIndex(where: { $0.id == rowID }),
              let column = destinations.firstIndex(where: { $0.id == destinationID }) else { return }
        let previous = focusedCell.flatMap { cell in rows.firstIndex { $0.id == cell.rowID } }
        focusedCell = Cell(rowID: rowID, destinationID: destinationID)
        synchronizingSelection = true
        channelTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        routingTable.selectRowIndexes(IndexSet(integer: row), byExtendingSelection: false)
        synchronizingSelection = false
        var changed = IndexSet(integer: row); if let previous { changed.insert(previous) }
        routingTable.reloadData(forRowIndexes: changed, columnIndexes: IndexSet(integersIn: 0..<destinations.count))
        if reveal { routingTable.scrollRowToVisible(row); routingTable.scrollColumnToVisible(column) }
    }
    func moveFocus(rows rowDelta: Int, columns columnDelta: Int) {
        guard !rows.isEmpty, !destinations.isEmpty else { return }
        let row = rows.firstIndex { $0.id == focusedCell?.rowID } ?? 0
        let column = destinations.firstIndex { $0.id == focusedCell?.destinationID } ?? 0
        let nextRow = max(0, min(rows.count - 1, row + max(-rows.count, min(rows.count, rowDelta))))
        let nextColumn = max(0, min(destinations.count - 1, column + max(-destinations.count, min(destinations.count, columnDelta))))
        focus(rowID: rows[nextRow].id, destinationID: destinations[nextColumn].id)
    }
    func activateFocusedCell() { if let focusedCell { activate(rowID: focusedCell.rowID, destinationID: focusedCell.destinationID) } }
    func removeFocusedSend() { if let focusedCell { removeSend(rowID: focusedCell.rowID, destinationID: focusedCell.destinationID) } }
    func activate(rowID: UInt64, destinationID: UInt64) { perform(.primary, rowID: rowID, destinationID: destinationID) }
    func removeSend(rowID: UInt64, destinationID: UInt64) { perform(.remove, rowID: rowID, destinationID: destinationID) }

    func blockReason(rowID: UInt64, destinationID: UInt64) -> String? {
        guard let row = rows.first(where: { $0.id == rowID }), destinations.contains(where: { $0.id == destinationID }) else {
            return "Channel or destination is no longer available."
        }
        if activeMode == .main { return MixerRoutingPolicy.outputBlockReason(row: row, destinationID: destinationID, strips: strips) }
        guard row.kind == .track, destinationID != 0 else { return "Sends require a track and a bus." }
        if !row.sends.contains(where: { $0.busID == destinationID }), row.sends.count >= 8 { return "This track already has eight sends." }
        return nil
    }
    private func perform(_ intent: Intent, rowID: UInt64, destinationID: UInt64, expectedGeneration: UInt64? = nil) {
        onWillInteract?()
        guard expectedGeneration == nil || expectedGeneration == generation,
              editingEnabled, editingAllowed?() != false,
              blockReason(rowID: rowID, destinationID: destinationID) == nil,
              let row = rows.first(where: { $0.id == rowID }) else { return }
        focus(rowID: rowID, destinationID: destinationID)
        if activeMode == .main {
            guard intent == .primary, row.outputID != destinationID else { return }
            onOutput?(rowID, destinationID)
        } else {
            let send = row.sends.first { $0.busID == destinationID }
            switch intent {
            case .primary: onSend?(rowID, send == nil ? .add(destinationID) : .edit(destinationID))
            case .remove: if send != nil { onSend?(rowID, .remove(destinationID)) }
            case .toggleTap: if let send { onSend?(rowID, .tap(destinationID, !send.preFader)) }
            }
        }
    }
    func sendMenu(rowID: UInt64, destinationID: UInt64) -> NSMenu? {
        onWillInteract?()
        guard activeMode == .sends, let row = rows.first(where: { $0.id == rowID }),
              let send = row.sends.first(where: { $0.busID == destinationID }),
              destinations.contains(where: { $0.id == destinationID }) else { return nil }
        let token = generation
        let menu = NSMenu(); menu.autoenablesItems = false
        let enabled = editingEnabled && editingAllowed?() != false
        for (title, intent) in [("Edit level…", Intent.primary), (send.preFader ? "Switch to POST" : "Switch to PRE", Intent.toggleTap), ("Remove send", Intent.remove)] {
            menu.addItem(MixerMenuItem(title, enabled: enabled) { [weak self] in
                self?.perform(intent, rowID: rowID, destinationID: destinationID, expectedGeneration: token)
            })
        }
        return menu
    }
    override func layout() {
        super.layout()
        let w = max(0, bounds.width), h = max(0, bounds.height)
        mode.frame = NSRect(x: 10, y: 8, width: min(230, max(100, w * 0.44)), height: 28)
        search.frame = NSRect(x: mode.frame.maxX + 10, y: 9, width: max(0, w - mode.frame.maxX - 20), height: 26)
        let namesWidth = min(178, max(70, w * 0.30)), tableHeight = max(0, h - 100)
        channelTable.tableColumns.first?.width = namesWidth
        // Explicit document width is required for the fixed, non-autoresizing names table.
        channelTable.setFrameSize(NSSize(width: namesWidth, height: channelTable.frame.height))
        channelScroll.frame = NSRect(x: 0, y: 44, width: namesWidth, height: tableHeight)
        routingScroll.frame = NSRect(x: namesWidth, y: 44, width: max(0, w - namesWidth), height: tableHeight)
        status.frame = NSRect(x: 10, y: max(44, h - 50), width: max(0, w - 20), height: 44)
        empty.frame = NSRect(x: namesWidth + 8, y: 90, width: max(0, w - namesWidth - 16), height: 24)
        channelScroll.tile(); routingScroll.tile()
        scrollChanged(Notification(name: NSView.boundsDidChangeNotification, object: routingScroll.contentView))
    }
}
