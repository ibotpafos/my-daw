import AppKit

@MainActor
private final class PaletteTargetView: NSView, NSMenuItemValidation {
    var allowed = true
    var invocations: [Int] = []
    override var acceptsFirstResponder: Bool { true }
    @objc func paletteFixtureAction(_ sender: NSMenuItem) { invocations.append(sender.tag) }
    func validateMenuItem(_ item: NSMenuItem) -> Bool { allowed }
}

@MainActor
private final class PaletteMarkedTextView: NSTextView {
    override func hasMarkedText() -> Bool { true }
}

/// A native AppKit integration harness, independent of C++/audio devices.
/// It uses real menus, windows, field editors and target/action dispatch. Manual
/// visual/listening/VoiceOver acceptance is deliberately not claimed by it.
@main
@MainActor
struct CommandPaletteAppKitTests {
    private static var checks = 0

    static func main() {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.regular)
        // LaunchServices owns activation; run AppKit's event loop rather than
        // a Foundation-only loop that leaves NSApp.keyWindow/mainWindow nil.
        DispatchQueue.global().asyncAfter(deadline: .now() + 120) { exit(124) }
        DispatchQueue.main.async {
            runTests()
            guard (2...3).contains(CommandLine.arguments.count) else { fatalError("missing result path") }
            do {
                try Data("PASS\n".utf8).write(to: URL(fileURLWithPath: CommandLine.arguments[1]), options: .atomic)
            } catch { fatalError("cannot write test result: \(error)") }
            exit(0)
        }
        NSApp.run()
    }

    private static func runTests() {
        let savedMenu = NSApp.mainMenu
        defer { NSApp.mainMenu = savedMenu }
        let suite = "my-daw.palette-appkit-tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let usage = DAWCommandUsageStore(defaults: defaults)
        let controller = DAWCommandPaletteController(usageStore: usage)
        let host = DAWWindow(contentRect: NSRect(x: 100, y: 100, width: 900, height: 650),
                             styleMask: [.titled, .closable], backing: .buffered, defer: false)
        host.isReleasedWhenClosed = false
        let target = PaletteTargetView(frame: NSRect(x: 0, y: 0, width: 900, height: 650))
        host.contentView = target
        let menu = NSMenu(title: "Test")
        let parent = NSMenuItem(title: "Test", action: nil, keyEquivalent: "")
        parent.submenu = menu
        let root = NSMenu()
        root.autoenablesItems = false
        root.addItem(parent)
        NSApp.mainMenu = root
        let alpha = NSMenuItem(title: "Alpha", action: #selector(PaletteTargetView.paletteFixtureAction(_:)), keyEquivalent: "")
        alpha.identifier = NSUserInterfaceItemIdentifier("fixture.alpha")
        alpha.target = target
        alpha.tag = 1
        let beta = NSMenuItem(title: "Beta", action: #selector(PaletteTargetView.paletteFixtureAction(_:)), keyEquivalent: "")
        beta.identifier = NSUserInterfaceItemIdentifier("fixture.beta")
        beta.tag = 2 // nil target: must resolve against the host view, not search.
        menu.addItem(alpha)
        menu.addItem(beta)
        let paletteItem = DAWWindow.makeCommandPaletteMenuItem()
        menu.addItem(paletteItem)
        expect(paletteItem.target == nil && paletteItem.keyEquivalent == "k", "visible palette item uses the document responder chain")
        host.makeKeyAndOrderFront(nil)
        host.makeMain()
        NSApp.activate()
        host.makeFirstResponder(target)
        drain()

        func openPalette() -> (NSWindow, NSSearchField, NSTableView, NSSegmentedControl) {
            host.makeKeyAndOrderFront(nil)
            host.makeMain()
            host.makeFirstResponder(target)
            drain()
            expect(host.firstResponder === target, "fixture restores source responder")
            expect(NSApp.keyWindow === host && NSApp.mainWindow === host,
                   "fixture has an actual key/main window before menu resolution")
            let nilTarget = NSApp.target(forAction: beta.action!, to: nil, from: beta) as AnyObject?
            expect(nilTarget === target, "fixture's unbound action resolves to source responder")
            controller.present(from: host)
            drain()
            expect(controller.isVisible, "palette opens")
            guard let panel = host.childWindows?.first, let view = panel.contentView,
                  let search: NSSearchField = find(in: view),
                  let table: NSTableView = find(in: view),
                  let scope: NSSegmentedControl = find(in: view) else { fatalError("missing palette controls") }
            expect(search.currentEditor() is NSTextView, "search uses a native field editor")
            return (panel, search, table, scope)
        }
        func boundKey(_ search: NSSearchField, _ selector: Selector) -> Bool {
            guard let editor = search.currentEditor() as? NSTextView else { fatalError("field editor missing") }
            return controller.control(search, textView: editor, doCommandBy: selector)
        }
        func filter(_ search: NSSearchField, _ query: String) {
            search.stringValue = query
            controller.controlTextDidChange(Notification(name: NSControl.textDidChangeNotification, object: search))
        }
        func scope(_ control: NSSegmentedControl, _ value: DAWCommandPaletteScope) {
            control.selectedSegment = value.rawValue
            expect(NSApp.sendAction(control.action!, to: control.target, from: control), "scope action dispatch")
        }

        let (_, search, table, _) = openPalette()
        expect(table.numberOfRows == 2, "explicit and nil-target menu commands are indexed: rows=\(table.numberOfRows), alpha=\(alpha.isEnabled), beta=\(beta.isEnabled)")
        expect(usage.history.entries.isEmpty, "opening palette does not record usage")
        if let panel = host.childWindows?.first {
            inspectLayoutAndCapture(panel)
        }
        expect(boundKey(search, #selector(NSResponder.moveDown(_:))), "field-editor down handled")
        expect(table.selectedRow == 1, "field-editor down selects next result")
        expect(boundKey(search, #selector(NSResponder.moveUp(_:))) && table.selectedRow == 0, "field-editor up")
        expect(!controller.control(search, textView: PaletteMarkedTextView(), doCommandBy: #selector(NSResponder.insertNewline(_:))), "IME composition is not a command")
        expect(!boundKey(search, #selector(NSResponder.moveLeft(_:))), "ordinary text navigation belongs to field editor")
        filter(search, "Beta")
        expect(table.numberOfRows == 1, "search filters native results")
        expect(boundKey(search, #selector(NSResponder.insertNewline(_:))), "field-editor Return handled")
        expect(target.invocations == [2], "nil-target action goes to original responder")
        expect(!controller.isVisible && host.firstResponder === target, "dismiss restores host responder")
        expect(usage.history.entries.count == 1, "only dispatched action enters history")
        expect(DAWCommandUsageStore(defaults: defaults).history == usage.history, "native dispatch persists history")

        let (recentPanel, _, recentTable, scopes) = openPalette()
        scope(scopes, .recent)
        expect(recentTable.numberOfRows == 1, "recent scope only shows invoked command")
        scope(scopes, .frequent)
        expect(recentTable.numberOfRows == 1, "frequent scope is wired")
        guard let clear = descendants(of: recentPanel.contentView!).compactMap({ $0 as? NSButton })
            .first(where: { $0.title == "Очистить историю" }) else { fatalError("missing clear button") }
        clear.performClick(nil)
        expect(usage.history.entries.isEmpty && recentTable.numberOfRows == 0, "clear-history control updates store and results")
        scope(scopes, .all)
        expect(recentTable.numberOfRows == 2, "clearing history does not delete real commands")
        expect(recentPanel.performKeyEquivalent(with: key(code: 40, characters: "л", flags: [.command, .capsLock], window: recentPanel)), "Cmd-K handled by palette window on RU layout")
        expect(!controller.isVisible, "Cmd-K closes palette even when search owns focus")

        let (_, disabledSearch, _, _) = openPalette()
        target.allowed = false
        _ = boundKey(disabledSearch, #selector(NSResponder.insertNewline(_:)))
        expect(target.invocations == [2], "action disabled after opening cannot execute")
        expect(usage.history.entries.isEmpty, "rejected dispatch does not enter history")
        target.allowed = true

        let (_, staleSearch, _, _) = openPalette()
        let replacement = PaletteTargetView()
        alpha.target = replacement
        _ = boundKey(staleSearch, #selector(NSResponder.insertNewline(_:)))
        expect(replacement.invocations.isEmpty && target.invocations == [2], "changed target is not silently substituted")
        alpha.target = target

        let (_, changedTagSearch, _, _) = openPalette()
        alpha.tag = 99
        _ = boundKey(changedTagSearch, #selector(NSResponder.insertNewline(_:)))
        expect(target.invocations == [2], "changed sender tag does not change the action context")
        alpha.tag = 1

        let (_, detachedSearch, _, _) = openPalette()
        menu.removeItem(alpha)
        _ = boundKey(detachedSearch, #selector(NSResponder.insertNewline(_:)))
        expect(target.invocations == [2], "detached menu item cannot execute")
        menu.insertItem(alpha, at: 0)

        parent.isHidden = true
        let (_, emptySearch, emptyTable, _) = openPalette()
        expect(emptyTable.numberOfRows == 0, "hidden ancestor excludes its commands")
        _ = boundKey(emptySearch, #selector(NSResponder.insertNewline(_:)))
        expect(target.invocations == [2], "Return on empty results is a no-op")
        _ = boundKey(emptySearch, #selector(NSResponder.cancelOperation(_:)))
        expect(!controller.isVisible, "Escape closes from a field editor")
        parent.isHidden = false

        let (tablePanel, _, focusedTable, _) = openPalette()
        focusedTable.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false)
        tablePanel.makeFirstResponder(focusedTable)
        focusedTable.keyDown(with: key(code: 36, characters: "\r", flags: [], window: tablePanel))
        expect(target.invocations == [2, 1], "Return works with table focus too")

        let (lostFocusPanel, _, _, _) = openPalette()
        controller.windowDidResignKey(Notification(name: NSWindow.didResignKeyNotification, object: lostFocusPanel))
        expect(!controller.isVisible && host.childWindows?.isEmpty != false, "focus loss detaches the panel")
        expect(!DAWCommandPaletteKey.isToggle(key(code: 40, characters: "k", flags: [.command, .shift], window: host)), "other shortcuts are not captured")
        host.makeKeyAndOrderFront(nil)
        host.makeFirstResponder(target)
        drain()
        expect(NSApp.sendAction(paletteItem.action!, to: nil, from: paletteItem), "visible menu action resolves through document window")
        expect(host.childWindows?.count == 1, "visible menu action opens palette")
        if let menuPanel = host.childWindows?.first {
            expect(menuPanel.performKeyEquivalent(with: key(code: 40, characters: "k", flags: .command, window: menuPanel)), "keyboard closes menu-opened palette")
        }
        let marked = PaletteMarkedTextView(frame: .zero)
        target.addSubview(marked)
        host.makeFirstResponder(marked)
        host.showCommandPalette(paletteItem)
        expect(host.childWindows?.isEmpty != false, "menu action cannot interrupt marked text")
        host.makeFirstResponder(target)
        marked.removeFromSuperview()
        expect(host.performKeyEquivalent(with: key(code: 40, characters: "k", flags: .command, window: host)), "main window shortcut opens palette")
        expect(host.childWindows?.count == 1, "one attached palette window")
        host.close()
        expect(host.childWindows?.isEmpty != false, "closing host removes owned palette")
        print("command palette AppKit: \(checks) checks passed")
    }

    private static func inspectLayoutAndCapture(_ panel: NSWindow) {
        guard let view = panel.contentView else { fatalError("missing palette view") }
        view.layoutSubtreeIfNeeded()
        expect(!view.hasAmbiguousLayout, "palette root has unambiguous layout")
        for control in view.subviews {
            expect(!control.hasAmbiguousLayout && control.frame.width > 0 && control.frame.height > 0,
                   "palette controls have resolved nonempty geometry")
            expect(view.bounds.insetBy(dx: -1, dy: -1).contains(control.frame),
                   "palette controls are contained in the panel")
        }
        guard CommandLine.arguments.count == 3, !CommandLine.arguments[2].isEmpty else { return }
        let output = CommandLine.arguments[2]
        do {
            let directory = URL(fileURLWithPath: output, isDirectory: true)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            guard let image = view.bitmapImageRepForCachingDisplay(in: view.bounds) else {
                fatalError("cannot create palette snapshot")
            }
            view.cacheDisplay(in: view.bounds, to: image)
            guard let png = image.representation(using: .png, properties: [:]) else {
                fatalError("cannot encode palette snapshot")
            }
            try png.write(to: directory.appendingPathComponent("command-palette.png"))
        } catch { fatalError("cannot capture native palette: \(error)") }
    }

    private static func key(code: UInt16, characters: String, flags: NSEvent.ModifierFlags,
                            window: NSWindow) -> NSEvent {
        NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: window.windowNumber, context: nil, characters: characters,
            charactersIgnoringModifiers: characters, isARepeat: false, keyCode: code)!
    }
    private static func drain() {
        let deadline = Date(timeIntervalSinceNow: 0.05)
        while Date() < deadline {
            if let event = NSApp.nextEvent(matching: .any, until: deadline, inMode: .default, dequeue: true) {
                NSApp.sendEvent(event)
            }
        }
    }
    private static func descendants(of view: NSView) -> [NSView] {
        [view] + view.subviews.flatMap { descendants(of: $0) }
    }
    private static func find<T: NSView>(in view: NSView) -> T? {
        descendants(of: view).compactMap { $0 as? T }.first
    }
    private static func expect(_ condition: Bool, _ message: String) {
        checks += 1
        guard condition else {
            FileHandle.standardError.write(Data("FAIL: \(message)\n".utf8))
            exit(1)
        }
    }
}
