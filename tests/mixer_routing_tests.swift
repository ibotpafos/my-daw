import AppKit

struct MixerRoutingTests {
    @MainActor
    static func run() throws {
        var vocal = MixerStripModel(id: 1, kind: .track, title: "Lead Vocal", color: .systemOrange, outputName: "Vocal Bus", outputID: 100)
        vocal.sends = [MixerSendSummary(destination: "Plate", gainDb: -18.5, preFader: false, busID: 102)]
        let drums = MixerStripModel(id: 2, kind: .track, title: "Drums", color: .systemTeal)
        let a = MixerStripModel(id: 100, kind: .bus, title: "Vocal Bus", outputID: 101)
        let b = MixerStripModel(id: 101, kind: .bus, title: "Mix Bus")
        let c = MixerStripModel(id: 102, kind: .bus, title: "Plate", outputID: 100)
        let master = MixerStripModel(id: 0, kind: .master, title: "Master")
        let fixture = [vocal, drums, a, b, c, master]
        let matrix = MixerRoutingMatrixView(frame: NSRect(x: 0, y: 0, width: 900, height: 540))
        let window = NSWindow(contentRect: matrix.frame, styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        window.contentView = matrix; matrix.strips = fixture
        matrix.layoutSubtreeIfNeeded()
        var outputs: [String] = [], sends: [String] = []
        matrix.onOutput = { outputs.append("\($0):\($1)") }
        matrix.onSend = { id, action in
            switch action {
            case .add(let bus): sends.append("add:\(id):\(bus)")
            case .edit(let bus): sends.append("edit:\(id):\(bus)")
            case .remove(let bus): sends.append("remove:\(id):\(bus)")
            case .tap(let bus, let pre): sends.append("tap:\(id):\(bus):\(pre)")
            }
        }
        precondition(matrix.blockReason(rowID: 101, destinationID: 102) != nil, "Indirect feedback B → C → A → B")
        matrix.activate(rowID: 101, destinationID: 102)
        matrix.activate(rowID: 100, destinationID: 100)
        matrix.activate(rowID: 1, destinationID: 100) // Already connected: no mutation.
        precondition(outputs.isEmpty)
        matrix.activate(rowID: 100, destinationID: 0)
        precondition(outputs == ["100:0"])
        var broken = fixture
        broken[3].outputID = 102
        precondition(MixerRoutingPolicy.outputBlockReason(row: drums, destinationID: 102, strips: broken) != nil)
        broken[3].outputID = 999
        precondition(MixerRoutingPolicy.outputBlockReason(row: drums, destinationID: 102, strips: broken) != nil)
        precondition(MixerRoutingPolicy.outputBlockReason(row: master, destinationID: 0, strips: fixture) != nil)

        matrix.setMode(.sends)
        matrix.activate(rowID: 1, destinationID: 102)
        matrix.removeSend(rowID: 1, destinationID: 102)
        matrix.removeSend(rowID: 2, destinationID: 102) // Missing send: no mutation.
        matrix.activate(rowID: 2, destinationID: 102)
        matrix.activate(rowID: 1, destinationID: 0)
        precondition(sends == ["edit:1:102", "remove:1:102", "add:2:102"], "Primary click must not delete an existing send")

        matrix.focus(rowID: 1, destinationID: 102)
        matrix.layoutSubtreeIfNeeded()
        let cell = matrix.routingTable.view(atColumn: 2, row: 0, makeIfNecessary: true) as! MixerActionButton
        precondition(cell.accessibilityLabel()?.contains("Lead Vocal → Plate") == true)
        precondition((cell.accessibilityValue() as? String)?.contains("18.5") == true)
        let beforeClick = sends.count
        cell.performClick(nil)
        precondition(sends.count == beforeClick + 1 && sends.last == "edit:1:102", "Real native cell click")
        matrix.editingEnabled = false
        matrix.activate(rowID: 2, destinationID: 102); matrix.removeFocusedSend()
        let disabled = matrix.routingTable.view(atColumn: 2, row: 0, makeIfNecessary: true) as! MixerActionButton
        precondition(!disabled.isEnabled)
        disabled.performClick(nil)
        precondition(sends.count == beforeClick + 1, "Recording lock covers programmatic and native actions")
        matrix.editingEnabled = true
        var allowed = false
        matrix.editingAllowed = { allowed }
        matrix.activateFocusedCell()
        precondition(sends.count == beforeClick + 1, "Live guard works before a visual refresh")
        allowed = true; matrix.activateFocusedCell()
        precondition(sends.last == "edit:1:102" && sends.count == beforeClick + 2)
        matrix.editingAllowed = nil

        // A menu opened against an older snapshot cannot edit a changed destination/tap.
        let staleMenu = matrix.sendMenu(rowID: 1, destinationID: 102)!
        var changed = fixture; changed[0].sends[0].preFader = true
        matrix.onWillInteract = { matrix.strips = changed }
        let staleCount = sends.count
        let staleItem = staleMenu.items[1]
        _ = NSApp.sendAction(staleItem.action!, to: staleItem.target, from: staleItem)
        precondition(sends.count == staleCount, "Stale menu rejected after synchronous provider refresh")
        let currentMenu = matrix.sendMenu(rowID: 1, destinationID: 102)!
        let currentItem = currentMenu.items[1]
        _ = NSApp.sendAction(currentItem.action!, to: currentItem.target, from: currentItem)
        precondition(sends.last == "tap:1:102:false")
        changed.removeAll { $0.id == 102 }
        let removedCount = sends.count
        matrix.activate(rowID: 1, destinationID: 102)
        precondition(sends.count == removedCount, "Deleted destination never emits an edit")
        matrix.onWillInteract = nil; matrix.strips = fixture

        // Keyboard dispatch uses the same stable IDs and does not steal modified shortcuts.
        matrix.focus(rowID: 1, destinationID: 100)
        func key(_ code: UInt16, _ chars: String = "") -> NSEvent {
            NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
                windowNumber: window.windowNumber, context: nil, characters: chars,
                charactersIgnoringModifiers: chars, isARepeat: false, keyCode: code)!
        }
        matrix.routingTable.keyDown(with: key(124))
        precondition(matrix.focusedCell == .init(rowID: 1, destinationID: 101))
        matrix.routingTable.keyDown(with: key(125))
        precondition(matrix.focusedCell == .init(rowID: 2, destinationID: 101))
        matrix.routingTable.keyDown(with: key(36, "\r"))
        precondition(sends.last == "add:2:101")
        matrix.focus(rowID: 1, destinationID: 102)
        matrix.routingTable.keyDown(with: key(51))
        precondition(sends.last == "remove:1:102")
        matrix.setSearch("vocal bus")
        precondition(matrix.rows.map(\.id) == [1])
        matrix.setSearch("")
        matrix.focus(rowID: 1, destinationID: 102)
        matrix.strips = [drums, vocal, c, b, a, master]
        precondition(matrix.focusedCell == .init(rowID: 1, destinationID: 102), "Reorder preserves both focus identities")
        matrix.moveFocus(rows: Int.max, columns: Int.max)
        precondition(matrix.focusedCell == .init(rowID: 1, destinationID: 100))
        matrix.setSearch("нет такого канала")
        precondition(matrix.rows.isEmpty && matrix.focusedCell == nil)
        matrix.moveFocus(rows: 1, columns: 1); matrix.activateFocusedCell(); matrix.removeFocusedSend()
        matrix.setSearch(""); matrix.strips = [vocal, master]
        precondition(matrix.destinations.isEmpty && matrix.focusedCell == nil)

        // Send capacity is an add limit, not a block on editing/removing existing sends.
        let buses = (100...115).map { MixerStripModel(id: UInt64($0), kind: .bus, title: "Bus \($0)") }
        var full = vocal
        full.sends = (100...107).map { MixerSendSummary(destination: "Bus \($0)", busID: UInt64($0)) }
        matrix.strips = [full] + buses + [master]
        precondition(matrix.blockReason(rowID: 1, destinationID: 108) != nil)
        precondition(matrix.blockReason(rowID: 1, destinationID: 100) == nil)
        let atLimit = sends.count
        matrix.activate(rowID: 1, destinationID: 108)
        precondition(sends.count == atLimit)
        matrix.activate(rowID: 1, destinationID: 100)
        precondition(sends.last == "edit:1:100")

        // Render actual native AppKit tables, not a generated UI illustration.
        matrix.strips = fixture
        matrix.focus(rowID: 1, destinationID: 102)
        matrix.needsLayout = true; matrix.layoutSubtreeIfNeeded()
        let nameCell = matrix.channelTable.view(atColumn: 0, row: 0, makeIfNecessary: true) as! NSTextField
        precondition(nameCell.stringValue == "Lead Vocal")
        precondition(matrix.channelTable.frame.width >= 170 && nameCell.frame.width > 100, "Channel names need a nonzero document and cell width")
        precondition(nameCell.convert(nameCell.bounds, to: matrix.channelTable).intersects(matrix.channelTable.visibleRect), "The channel name must be in the visible document")
        print("Routing label geometry: table=\(matrix.channelTable.frame), cell=\(nameCell.frame), visible=\(matrix.channelTable.visibleRect)")
        guard let rep = matrix.bitmapImageRepForCachingDisplay(in: matrix.bounds) else { fatalError("Routing bitmap unavailable") }
        matrix.cacheDisplay(in: matrix.bounds, to: rep)
        guard let png = rep.representation(using: .png, properties: [:]) else { fatalError("Routing PNG encoding failed") }
        try png.write(to: URL(fileURLWithPath: "build/mixer-routing-ui.png"))
        precondition(png.count > 5000)

        let large = (1...256).map { MixerStripModel(id: UInt64($0), kind: .track, title: "Track \($0)") }
        let returns = (1001...1016).map { MixerStripModel(id: UInt64($0), kind: .bus, title: "Return \($0)") }
        matrix.strips = large + returns + [master]
        matrix.needsLayout = true; matrix.layoutSubtreeIfNeeded()
        precondition(matrix.routingTable.numberOfRows == 256 && matrix.routingTable.numberOfColumns == 16)
        _ = matrix.routingTable.view(atColumn: 0, row: 0, makeIfNecessary: true)
        func buttons(_ view: NSView) -> Int { (view is MixerActionButton ? 1 : 0) + view.subviews.reduce(0) { $0 + buttons($1) } }
        let realized = buttons(matrix.routingTable)
        precondition(realized > 0 && realized < 2048, "View-based table must not instantiate all 4096 cells")
        let header = matrix.routingTable.headerView!
        let headerY = header.convert(header.bounds, to: matrix).minY
        matrix.routingScroll.contentView.scroll(to: NSPoint(x: 600, y: 1500))
        NotificationCenter.default.post(name: NSView.boundsDidChangeNotification, object: matrix.routingScroll.contentView)
        precondition(abs(matrix.channelScroll.contentView.bounds.minY - matrix.routingScroll.contentView.bounds.minY) < 1)
        precondition(matrix.channelScroll.contentView.bounds.minX == 0, "Channel names do not scroll horizontally")
        precondition(abs(header.convert(header.bounds, to: matrix).minY - headerY) < 1, "Destination headers stay fixed vertically")
        print("Routing native tests PASS: feedback, safe sends, recording lock, stale menus, keys, accessibility metadata, stable focus, 4096-cell table (\(realized) realized buttons), pinned headers")

        // The presenter keeps geometry when reopened, refreshes external edits, and stops polling on close.
        let presenter = MixerRoutingPresenter()
        var source = fixture
        var editable = true
        presenter.present(strips: { source }, editingAllowed: { editable }, onOutput: { _,_ in }, onSend: { _,_ in })
        let presented = presenter.presentedWindow!
        let destinationView = presented.contentView as! MixerRoutingMatrixView
        presented.setFrame(NSRect(x: 100, y: 100, width: 800, height: 480), display: false)
        let savedFrame = presented.frame
        presenter.present(strips: { source }, editingAllowed: { editable }, onOutput: { _,_ in }, onSend: { _,_ in })
        precondition(presenter.presentedWindow === presented && presented.frame == savedFrame && presenter.isPolling)
        source.removeAll { $0.id == 102 }; editable = false; presenter.reload()
        precondition(!destinationView.editingEnabled && !destinationView.destinations.contains { $0.id == 102 })
        presented.performClose(nil)
        precondition(presenter.presentedWindow == nil && !presenter.isPolling)
        presenter.close()
        print("Routing presenter tests PASS: window reuse, live snapshot/read-only refresh, timer teardown")
    }
}
