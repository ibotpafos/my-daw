import AppKit

@MainActor
func runLibraryFolderTests(_ controller: DraftApp) -> Int {
    var assertions = 0
    func expect(_ condition: @autoclosure () -> Bool, _ text: String) {
        assertions += 1
        precondition(condition(), "Library folder: " + text)
    }
    func wait(_ ready: () -> Bool) {
        let deadline = Date().addingTimeInterval(8)
        while !ready(), Date() < deadline { RunLoop.current.run(until: Date().addingTimeInterval(0.01)) }
        expect(ready(), "Background operation finishes within a bounded wait")
    }
    guard let session = controller.session, let root = controller.window.contentView else { fatalError("Controller") }
    let browser = controller.libraryBrowser, folder = browser.folder
    let defaults = UserDefaults.standard
    let saved = defaults.data(forKey: LibraryFolderBookmark.key)
    let previousAudio = browser.audioItems, previousTargets = controller.browserAudioURLs
    let previousKeys = browser.favorites.keys
    let fm = FileManager()
    let directory = fm.temporaryDirectory.appendingPathComponent("mydaw-folder-\(UUID().uuidString)")
    let drums = directory.appendingPathComponent("Studio Samples")
    let other = directory.appendingPathComponent("Other")
    do {
        try fm.createDirectory(at: drums.appendingPathComponent("Drums"), withIntermediateDirectories: true)
        try fm.createDirectory(at: other, withIntermediateDirectories: true)
        for name in ["Drums/Kick.wav", "Drums/Snare.WAV", "Bass.aiff", "Texture.aifc", ".hidden.wav", "ignore.txt"] {
            try Data().write(to: drums.appendingPathComponent(name))
        }
        try Data().write(to: other.appendingPathComponent("Only.wav"))
    } catch { fatalError("Filesystem fixture: \(error)") }
    defer {
        browser.isImportBusy = false; browser.mutationEnabled = true
        controller.isRecording = false; controller.midiTakeArmed = false
        folder.forget()
        if let saved { defaults.set(saved, forKey: LibraryFolderBookmark.key) }
        else { defaults.removeObject(forKey: LibraryFolderBookmark.key) }
        for key in browser.favorites.keys.subtracting(previousKeys) { browser.favorites.set(key, favorite: false) }
        for key in previousKeys.subtracting(browser.favorites.keys) { browser.favorites.set(key, favorite: true) }
        controller.browserAudioURLs = previousTargets; browser.audioItems = previousAudio
        browser.selectCollection(.all); browser.setQuery(""); browser.selectFormat(nil)
        controller.window.makeFirstResponder(nil)
        try? fm.removeItem(at: directory)
    }
    func revision() -> UInt64 {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &snapshot) == 0, "Read real session")
        return snapshot.revision
    }
    let originalRevision = revision()
    browser.selectCollection(.all); browser.setQuery(""); browser.selectCategory(.audio); browser.selectFormat(nil)
    expect(folder.isConfigured, "Production workspace wires new folder coordinator")
    folder.open(drums)
    expect(folder.state.busy && browser.audioItems == previousAudio, "Open returns immediately, previous snapshot not cleared")
    wait { !folder.state.busy }
    expect(browser.audioItems.count == 4 && !folder.state.warning, "Real worker publishes metadata-only file catalog")
    expect(Set(browser.audioItems.compactMap(\.sourceURL)) == Set(controller.browserAudioURLs.values), "Targets and views are one snapshot")
    expect(folder.activeURL?.lastPathComponent == "Studio Samples", "Folder title reflects committed location")
    guard let first = browser.audioItems.first else { fatalError("No audio item") }
    browser.selectItem(id: first.id); browser.toggleFavorite(id: first.id)
    let firstKey = first.resourceKey
    browser.folderBar.refreshButton.performClick(nil)
    wait { !folder.state.busy }
    expect(browser.selectedItem?.resourceKey == firstKey && browser.selectedItem?.id != first.id, "Rescan retains stable selection but replaces stale command UUID")
    expect(browser.favorites.contains(firstKey), "Rescan preserves favorites")
    expect(!folder.state.message.isEmpty && browser.folderBar.status.stringValue == folder.state.message, "Real status view reflects completion")
    let initial = browser.audioItems
    folder.open(other); folder.cancel()
    RunLoop.current.run(until: Date().addingTimeInterval(0.1))
    expect(browser.audioItems == initial && !folder.state.busy, "Cancel cannot publish late results")
    folder.open(other); folder.open(drums)
    wait { !folder.state.busy }
    expect(browser.audioItems.count == 4 && folder.activeURL?.lastPathComponent == "Studio Samples", "Latest request wins even when previous completion is queued")

    folder.open(other); browser.isImportBusy = true
    wait { folder.waitingToPublish }
    expect(browser.audioItems.count == 4, "Completion deferred while an import may need old folder access")
    expect(!browser.folderBar.forgetButton.isEnabled && !browser.folderBar.refreshButton.isEnabled, "Busy state disables lease-changing controls")
    browser.isImportBusy = false
    expect(browser.audioItems.count == 1 && folder.activeURL?.lastPathComponent == "Other", "Pending snapshot published when import finishes")
    folder.open(drums); controller.midiTakeArmed = true; controller.updateWorkspaceChrome()
    wait { folder.waitingToPublish }
    expect(browser.audioItems.count == 1, "MIDI capture blocks publication")
    folder.forget()
    expect(browser.audioItems.count == 1 && defaults.data(forKey: LibraryFolderBookmark.key) != nil, "Stale forget callback respects capture guard")
    controller.midiTakeArmed = false; controller.updateWorkspaceChrome()
    expect(browser.audioItems.count == 4 && !folder.state.busy, "MIDI release publishes pending result")
    controller.isRecording = true; controller.updateWorkspaceChrome(); folder.open(other)
    expect(!folder.state.busy && browser.audioItems.count == 4, "Recording rejects new folder mutation")
    controller.isRecording = false; controller.updateWorkspaceChrome()

    let beforeFailure = browser.audioItems
    folder.open(directory.appendingPathComponent("missing"))
    wait { !folder.state.busy }
    expect(folder.state.warning && browser.audioItems == beforeFailure, "Missing folder preserves previous catalog and shows error")
    expect(folder.activeURL?.lastPathComponent == "Studio Samples", "Failed replacement does not replace active folder")
    guard let bookmark = LibraryFolderBookmark.decode(defaults.data(forKey: LibraryFolderBookmark.key)) else { fatalError("No persisted bookmark") }
    expect(bookmark.version == 1 && !bookmark.data.isEmpty, "Versioned system bookmark persisted only after success")
    expect(LibraryFolderBookmark.decode(nil) == nil && LibraryFolderBookmark.decode(Data([1, 2])) == nil, "Corrupt preferences rejected")
    var future = bookmark; future.version = 999
    expect(LibraryFolderBookmark.decode(future.encoded) == nil, "Unknown archive version rejected")
    expect(LibraryFolderBookmark.decode(Data(repeating: 0, count: LibraryFolderBookmark.maximumBytes * 2 + 1)) == nil, "Preference size bounded")
    expect(LibraryFolderBookmark.decode(LibraryFolderBookmark(data: Data(), scoped: false, title: "Empty").encoded) == nil, "Empty bookmark rejected")

    // New coordinator reads the actual system bookmark; no path fallback/spies.
    let restored = LibraryFolderController(defaults: defaults)
    var restoredURLs: [URL] = []
    restored.onPublish = { restoredURLs = $0 }
    restored.restore(); expect(restored.state.busy, "Restoration schedules filesystem work, not a synchronous enumeration")
    wait { !restored.state.busy }
    expect(restoredURLs.count == 4 && !restored.state.warning, "Actual bookmark restores folder through fresh coordinator")
    restored.cancel()
    defaults.set(Data([1,2,3]), forKey: LibraryFolderBookmark.key)
    let damaged = LibraryFolderController(defaults: defaults); var damagedPublishes = 0
    damaged.onPublish = { _ in damagedPublishes += 1 }; damaged.restore()
    expect(damaged.state.warning && !damaged.state.busy && damagedPublishes == 0, "Damaged bookmark does not scan a guessed directory")
    defaults.set(bookmark.encoded, forKey: LibraryFolderBookmark.key)

    expect(controller.window.makeFirstResponder(browser.folderBar.refreshButton), "Folder control accepts keyboard focus")
    let oldDelete = controller.window.onDeleteSelectedClip, oldTrackDelete = controller.window.onDeleteSelectedTrack
    var deletes = 0
    controller.window.onDeleteSelectedClip = { deletes += 1 }; controller.window.onDeleteSelectedTrack = { deletes += 1 }
    if let event = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [.command], timestamp: 0,
        windowNumber: controller.window.windowNumber, context: nil, characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: 51) {
        expect(controller.window.performKeyEquivalent(with: event), "Folder-focused Command-Delete consumed locally")
        expect(deletes == 0, "Folder action focus cannot delete arrangement content")
    } else { fatalError("Key event") }
    controller.window.onDeleteSelectedClip = oldDelete; controller.window.onDeleteSelectedTrack = oldTrackDelete
    controller.window.makeFirstResponder(nil)
    browser.folderBar.forgetButton.performClick(nil)
    expect(browser.audioItems.isEmpty && controller.browserAudioURLs.isEmpty && folder.activeURL == nil, "Forget clears snapshot and releases active folder")
    expect(defaults.data(forKey: LibraryFolderBookmark.key) == nil && browser.favorites.contains(firstKey), "Forget removes bookmark but not favorites")
    expect(fm.fileExists(atPath: drums.appendingPathComponent("Drums/Kick.wav").path), "Forget never deletes files")
    expect(revision() == originalRevision, "Folder open/rescan/cancel/restore/forget never mutate project or Undo")

    folder.open(drums); wait { !folder.state.busy }
    for item in browser.audioItems { if !browser.favorites.contains(item.resourceKey) { browser.toggleFavorite(id: item.id) } }
    browser.selectCollection(.favorites)
    for (width, height) in [(1536, 1000), (1060, 700)] {
        controller.window.setContentSize(NSSize(width: width, height: height))
        for _ in 0..<4 { root.layoutSubtreeIfNeeded(); controller.workspace?.applyGeometry(); controller.fitTrackHeaderWidth(); controller.fitTimelineViewport() }
        for button in [browser.folderBar.refreshButton, browser.folderBar.forgetButton] {
            let rect = button.convert(button.bounds, to: browser)
            expect(rect.minX >= 0 && rect.maxX <= browser.bounds.width && rect.width >= 24, "Folder actions fit actual sidebar bounds")
        }
        guard let bitmap = root.bitmapImageRepForCachingDisplay(in: root.bounds) else { fatalError("Bitmap") }
        root.cacheDisplay(in: root.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else { fatalError("PNG") }
        do { try data.write(to: URL(fileURLWithPath: "build/workspace-ui/workspace-folder-\(width).png")) } catch { fatalError("PNG write") }
        print("SNAPSHOT workspace-folder-\(width)")
    }
    print("PASS: \(assertions) native background folder lifecycle, bookmark, publication, focus and geometry assertions")
    return assertions
}
