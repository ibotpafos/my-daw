import AppKit

@MainActor
func runLibraryBrowserTests(_ controller: DraftApp) -> Int {
    var assertions = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        assertions += 1
        if !condition() { fatalError("Library: \(message)") }
    }
    guard let session = controller.session, let root = controller.window.contentView else { fatalError("Actual controller missing") }
    let browser = controller.libraryBrowser
    let oldAudio = browser.audioItems, oldPlugins = browser.pluginItems
    let oldKeys = browser.favorites.keys
    let oldAdd = browser.onAdd, oldSelect = browser.onBrowserSelect, oldPreview = browser.onPreview, oldStop = browser.onStopPreview
    let oldPlay = controller.window.onPlayStop, oldClipDelete = controller.window.onDeleteSelectedClip, oldTrackDelete = controller.window.onDeleteSelectedTrack
    func revision() -> UInt64 {
        var s = daw_snapshot(); s.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &s) == 0, "Read actual project")
        return s.revision
    }
    defer {
        controller.isRecording = false; controller.midiTakeArmed = false
        browser.mutationEnabled = true; browser.isImportBusy = false
        for key in browser.favorites.keys.subtracting(oldKeys) { browser.favorites.set(key, favorite: false) }
        for key in oldKeys.subtracting(browser.favorites.keys) { browser.favorites.set(key, favorite: true) }
        browser.setQuery(""); browser.selectCollection(.all); browser.selectCategory(.audio); browser.selectFormat(nil); browser.setAvailableOnly(false)
        browser.audioItems = oldAudio; browser.pluginItems = oldPlugins
        browser.onAdd = oldAdd; browser.onBrowserSelect = oldSelect; browser.onPreview = oldPreview; browser.onStopPreview = oldStop
        controller.window.onPlayStop = oldPlay; controller.window.onDeleteSelectedClip = oldClipDelete; controller.window.onDeleteSelectedTrack = oldTrackDelete
        controller.window.makeFirstResponder(nil)
    }
    browser.onBrowserSelect = { _, _ in }
    var added = 0, previewed = 0, stopped = 0, global = 0
    browser.onAdd = { _, _ in added += 1 }
    browser.onPreview = { item in previewed += 1; browser.updateAudioPreview(isPlaying: true, selectedID: item?.id, error: nil) }
    browser.onStopPreview = { stopped += 1; browser.updateAudioPreview(isPlaying: false, selectedID: nil, error: nil) }
    controller.window.onPlayStop = { global += 1 }; controller.window.onDeleteSelectedClip = { global += 1 }; controller.window.onDeleteSelectedTrack = { global += 1 }
    let baseline = revision()
    let file = URL(fileURLWithPath: "/library-test-\(UUID().uuidString)/Café Kick.wav")
    var a = InspectorBrowserItem(title: "Café Kick", detail: "Drums", sourceURL: file)
    let b = InspectorBrowserItem(title: "Café Kick", detail: "Other folder", sourceURL: file.deletingLastPathComponent().appendingPathComponent("other/Café Kick.wav"))
    let c = InspectorBrowserItem(title: "Snare", detail: "Drums", available: false, sourceURL: file.deletingLastPathComponent().appendingPathComponent("Snare.aiff"))
    browser.selectCategory(.audio); browser.audioItems = [a, b, c]
    browser.selectItem(id: a.id)
    guard let cell = browser.tableView(browser.table, viewFor: browser.table.tableColumns[0], row: 0) as? LibraryItemCell else { fatalError("Real native cell missing") }
    cell.favoriteButton.performClick(nil)
    expect(browser.favorites.contains(a.resourceKey) && !browser.favorites.contains(b.resourceKey), "Native star distinguishes identically named files")
    browser.selectCollection(.favorites)
    expect(browser.visibleItems.map(\.id) == [a.id], "Favorites intersects current catalog")
    var selected: InspectorBrowserItem?
    browser.onBrowserSelect = { _, item in selected = item }
    let oldID = a.id; a.id = UUID(); browser.audioItems = [a, b, c]
    expect(browser.selectedItem?.id == a.id && selected?.id == a.id, "Selection rebound to new UUID by stable resource identity")
    expect(browser.selectedItem?.id != oldID, "Dispatch never retains old row UUID")
    cell.favoriteButton.performClick(nil)
    expect(browser.favorites.contains(a.resourceKey), "Old row star cannot affect replacement catalog entry")
    browser.setQuery("cafe drums wav")
    expect(browser.visibleItems.count == 1, "Search spans name, format and folder; diacritics")
    browser.setQuery("bass")
    expect(browser.visibleItems.isEmpty && browser.emptyMessage != nil && !browser.addButton.isEnabled, "Empty query result clears action target")
    browser.setQuery(""); browser.selectCollection(.all); browser.selectFormat(.aiff)
    expect(browser.visibleItems.map(\.id) == [c.id], "AIFF format projection")
    browser.selectItem(id: c.id)
    expect(!browser.addButton.isEnabled && browser.tableView(browser.table, pasteboardWriterForRow: 0) == nil, "Unavailable resources cannot add or drag")
    browser.setAvailableOnly(true)
    expect(browser.visibleItems.isEmpty, "Availability filter removes missing files")
    browser.setAvailableOnly(false); browser.selectFormat(nil); browser.selectItem(id: a.id)
    guard let menu = browser.contextMenu(forRow: 0), let menuAdd = menu.items.first(where: { ($0.representedObject as? LibraryMenuPayload)?.action == .add }) else { fatalError("Native context menu missing") }
    browser.mutationEnabled = false; browser.dispatchMenu(menuAdd)
    expect(added == 0, "Stale context action rechecks recording guard")
    browser.mutationEnabled = true; browser.isImportBusy = true; browser.dispatchMenu(menuAdd)
    expect(added == 0, "Stale context action rechecks busy import")
    browser.isImportBusy = false; browser.dispatchMenu(menuAdd)
    expect(added == 1, "Context menu dispatches to existing import callback")
    a.id = UUID(); browser.audioItems = [a, b, c]; browser.dispatchMenu(menuAdd)
    expect(added == 1, "Menu from old catalog is rejected, not retargeted by row")
    expect(browser.contextMenu(forRow: -1) == nil && browser.contextMenu(forRow: 999) == nil, "No menu for background rows")
    expect(browser.tableView(browser.table, pasteboardWriterForRow: 0) != nil, "File drag payload preserved")
    root.layoutSubtreeIfNeeded()
    expect(controller.window.makeFirstResponder(browser.table), "Native table accepts focus")
    func key(_ code: UInt16, text: String = "", flags: NSEvent.ModifierFlags = [], repeatKey: Bool = false) -> NSEvent {
        guard let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
            windowNumber: controller.window.windowNumber, context: nil, characters: text,
            charactersIgnoringModifiers: text, isARepeat: repeatKey, keyCode: code) else { fatalError("Key event") }
        return event
    }
    controller.window.sendEvent(key(49, text: " "))
    expect(previewed == 1 && global == 0, "Focused Space auditions, not project transport")
    controller.window.sendEvent(key(49, text: " ", repeatKey: true))
    expect(stopped == 0, "Key repeat does not toggle audition repeatedly")
    expect(controller.window.performKeyEquivalent(with: key(49, text: " ")), "Space intercepted before global menu equivalent")
    expect(stopped == 1 && global == 0, "Space stops audition only")
    controller.window.sendEvent(key(36, text: "\r")); expect(added == 2, "Return invokes Add")
    controller.window.sendEvent(key(36, text: "\r", repeatKey: true)); expect(added == 2, "Repeat cannot add multiple tracks")
    controller.window.sendEvent(key(51)); _ = controller.window.performKeyEquivalent(with: key(51, flags: [.command]))
    expect(global == 0 && revision() == baseline, "Delete and Command-Delete never delete project content from library")
    browser.mutationEnabled = false
    controller.window.sendEvent(key(36)); controller.window.sendEvent(key(49, text: " "))
    expect(added == 2 && previewed == 1 && global == 0, "Recording disables add/audition without leaking Space to transport")
    browser.mutationEnabled = true
    expect(controller.window.makeFirstResponder(browser.search), "Search accepts focus")
    expect(!browser.handleFocusedKey(key(49, text: " ")), "Text editor is not a browser shortcut target")
    controller.window.makeFirstResponder(nil)
    let auKey = LibraryResourceKey.audioUnit(type: 0x61756678, subtype: 1, manufacturer: 2)
    let vstKey = LibraryResourceKey.vst3(path: "/Library/Audio/Plug-Ins/VST3/Test.vst3", classID: "0123456789ABCDEF0123456789ABCDEF")
    let au = InspectorBrowserItem(title: "Same", category: .effects, pluginResourceKey: auKey, pluginFormat: .au)
    let vst = InspectorBrowserItem(title: "Same", category: .effects, pluginResourceKey: vstKey, pluginFormat: .vst3)
    browser.pluginItems = [au, vst]; browser.selectCategory(.effects)
    expect(browser.formatFilter == nil && browser.formats.numberOfItems == 3, "Changing category resets incompatible format")
    browser.selectFormat(.vst3); expect(browser.visibleItems.map(\.id) == [vst.id], "VST3 filter is metadata-based")
    browser.selectFormat(.au); expect(browser.visibleItems.map(\.id) == [au.id], "AU filter distinguishes same display name")
    browser.toggleFavorite(id: au.id); browser.selectCollection(.favorites)
    expect(browser.visibleItems.count == 1, "Plug-in favorite")
    browser.pluginItems = []
    expect(browser.visibleItems.isEmpty && browser.selectedItem == nil && browser.favorites.contains(auKey), "Missing plug-in retained only as preference, not playable catalog row")

    // Actual catalog integration and a real add/undo via the existing AU host.
    browser.selectCollection(.all); browser.selectFormat(nil); controller.loadSupportedAudioUnits(); controller.refreshBrowserCatalog()
    guard let installed = browser.visibleItems.first(where: { $0.available && $0.pluginFormat == .au }) else { fatalError("Required system AU catalog") }
    browser.selectItem(id: installed.id); browser.toggleFavorite(id: installed.id)
    controller.refreshBrowserCatalog()
    expect(browser.selectedItem?.resourceKey == installed.resourceKey && browser.selectedItem?.id != installed.id, "Real scanner refresh preserves selected AU identity")
    guard let current = browser.selectedItem else { fatalError("Selected installed AU") }
    expect(controller.browserPluginTargets[current.id] != nil && controller.browserPluginTargets[installed.id] == nil, "Actual command map updated before selection callback")
    let beforeAdd = revision()
    controller.midiTakeArmed = true; controller.addBrowserItem(.plugins, current); controller.midiTakeArmed = false
    expect(revision() == beforeAdd, "Controller independently rejects MIDI-capture insertion")
    browser.onAdd = oldAdd
    browser.addSelected(); expect(revision() == beforeAdd + 1, "Real browser Add inserts an Audio Unit")
    controller.undo()
    // Preferences/filtering must not generate any extra project commands.
    let afterUndo = revision()
    browser.selectCollection(.favorites); browser.selectFormat(.au); browser.setQuery("")
    expect(revision() == afterUndo, "Favorites and filters do not touch Undo or revision")
    browser.audioItems = oldAudio; browser.selectCategory(.audio); browser.selectCollection(.all)
    for item in oldAudio.prefix(2) where !browser.favorites.contains(item.resourceKey) { browser.toggleFavorite(id: item.id) }
    browser.selectCollection(.favorites)
    func screenshot(_ width: CGFloat, _ height: CGFloat, _ name: String) {
        controller.window.setContentSize(NSSize(width: width, height: height))
        for _ in 0..<4 { root.layoutSubtreeIfNeeded(); controller.workspace?.applyGeometry(); controller.fitTrackHeaderWidth(); controller.fitTimelineViewport() }
        root.layoutSubtreeIfNeeded()
        expect(browser.formats.frame.width > 90 && browser.collections.frame.width >= 170, "Filter controls remain usable in a narrow sidebar")
        if let row = browser.table.view(atColumn: 0, row: 0, makeIfNecessary: true) as? LibraryItemCell {
            row.layoutSubtreeIfNeeded()
            expect(row.favoriteButton.frame.width >= 24 && row.nameLabel.frame.width > 45, "Native star and file title fit")
            let star = row.favoriteButton.convert(row.favoriteButton.bounds, to: browser.table)
            let viewport = browser.table.visibleRect
            expect(star.minX >= viewport.minX && star.maxX <= viewport.maxX, "Entire star is inside the visible table, not clipped by the scroll viewport")
            expect(!row.detailLabel.stringValue.contains("WAV · WAV"), "Format caption is not duplicated")
        } else { fatalError("Visible favorite cell missing") }
        guard let bitmap = root.bitmapImageRepForCachingDisplay(in: root.bounds) else { fatalError("Bitmap") }
        root.cacheDisplay(in: root.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else { fatalError("PNG") }
        do { try data.write(to: URL(fileURLWithPath: "build/workspace-ui/\(name).png")) } catch { fatalError("PNG write") }
        print("SNAPSHOT \(name)")
    }
    screenshot(1536, 1000, "workspace-library-favorites")
    screenshot(1060, 700, "workspace-library-1060")
    print("PASS: \(assertions) native library controls, stable catalog selection, keyboard scopes and AU command assertions")
    assertions += runLibraryFolderTests(controller)
    return assertions
}
