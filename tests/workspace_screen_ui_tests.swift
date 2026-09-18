import AppKit

/// Exercise the real application picker, mounted editors and session. No second
/// screen model, fake DAW controls or screenshot-only production fixtures.
@MainActor
func runWorkspaceScreenTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        if !condition() { fatalError("Workspace screens: \(message)") }
    }
    guard let workspace = app.workspace, let dock = app.workspaceDock,
          let root = app.window.contentView, let session = app.session else {
        fatalError("Missing production workspace")
    }
    func revision() -> UInt64 {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &snapshot) == 0, "Read real project revision")
        return snapshot.revision
    }
    func settle(_ width: CGFloat, _ height: CGFloat) {
        app.window.setContentSize(NSSize(width: width, height: height))
        for _ in 0..<4 { root.layoutSubtreeIfNeeded(); workspace.applyGeometry() }
    }
    func choose(_ screen: WorkspaceScreen) {
        app.workspaceMode.selectedSegment = screen.rawValue
        expect(NSApp.sendAction(app.workspaceMode.action!, to: app.workspaceMode.target,
                                from: app.workspaceMode), "Dispatch actual header picker")
        root.layoutSubtreeIfNeeded()
    }
    func screenshot(_ name: String) {
        root.layoutSubtreeIfNeeded()
        guard let bitmap = root.bitmapImageRepForCachingDisplay(in: root.bounds) else {
            fatalError("No native screen bitmap")
        }
        root.cacheDisplay(in: root.bounds, to: bitmap)
        guard let png = bitmap.representation(using: .png, properties: [:]) else {
            fatalError("No native screen PNG")
        }
        do { try png.write(to: URL(fileURLWithPath: "build/workspace-ui/\(name).png")) }
        catch { fatalError("Screen screenshot: \(error)") }
        print("SNAPSHOT \(name) \(Int(root.bounds.width))x\(Int(root.bounds.height))")
    }

    workspace.resetLayout(); settle(1536, 1000)
    let originalSelection = app.selectedMixerID
    let originalZoom = app.timelineZoom
    let originalViewport = app.timelineScroll.contentView.bounds.origin
    let originalPlayhead = app.playheadFrame
    let identities = [ObjectIdentifier(app.inspectorBrowser.midiEditor),
                      ObjectIdentifier(app.mixerWorkspace), ObjectIdentifier(app.channelRack),
                      ObjectIdentifier(app.libraryBrowser)]
    let initialRevision = revision()
    let initialDirty = app.dirty
    let initialPlaying = app.isPlaying
    let initialRecording = app.isRecording
    let initialLayout = workspace.preference
    expect(app.workspaceMode.segmentCount == 5, "Five real destinations in the header")
    expect(app.workspaceMode.action == #selector(DraftApp.changeWorkspaceScreen(_:)),
           "Header no longer dispatches the legacy transport preset action")
    guard let menu = NSApp.mainMenu?.items.first(where: {
        $0.identifier?.rawValue == "workspace.screens"
    })?.submenu else { fatalError("Missing native screen menu") }
    let destinations = menu.items.filter { $0.action == #selector(DraftApp.pickWorkspaceScreen(_:)) }
    expect(destinations.count == 5, "Every screen has a discoverable native command")
    expect(destinations.allSatisfy { $0.keyEquivalentModifierMask == [.control, .command] },
           "Screen shortcuts do not replace existing mixer or tool shortcuts")

    // The same clip and editor receive notes before and after full-screen mode.
    if let midi = app.midiArrangementViews.first {
        midi.onSelect?(0, true)
        expect(!app.inspectorBrowser.midiEditor.notes.isEmpty, "Real MIDI clip is bound")
    } else { fatalError("Missing actual MIDI test material") }
    let midiSelection = app.selectedMixerID
    let midiClip = app.midiClipIndex
    let midiNoteCount = app.inspectorBrowser.midiEditor.notes.count
    let layoutWithMidi = workspace.preference
    for (width, height) in [(CGFloat(1060), CGFloat(700)), (1536,1000), (1920,1080)] {
        settle(width, height)
        for screen in WorkspaceScreen.allCases {
            choose(screen)
            let g = workspace.geometry
            expect(workspace.screen == screen && app.workspaceMode.selectedSegment == screen.rawValue,
                   "Picker and accepted destination agree")
            expect(destinations.filter { $0.state == .on }.map(\.tag) == [screen.rawValue],
                   "Exactly one menu destination is selected")
            expect(workspace.preference == layoutWithMidi, "Screen changes preserve normal pane dimensions and dock tab")
            if screen == .browser {
                expect(g.library == workspace.bounds.width && g.center == 0 && g.dock == 0,
                       "Browser receives all available workspace")
                expect(!app.libraryBrowser.isHiddenOrHasHiddenAncestor, "Actual library is visible")
            } else if let tab = screen.dockTab {
                expect(g.dock == workspace.bounds.height && g.center == workspace.bounds.width,
                       "Editor receives all workspace width and height")
                expect(g.library == 0 && g.inspector == 0 && g.arrangement == 0,
                       "Dedicated editor has no hidden-width gutters")
                expect(dock.selected == tab && dock.expanded, "Correct existing dock view is expanded")
            } else {
                expect(g.center >= 520 && g.arrangement >= 220, "Arrange remains usable")
                expect(!dock.expanded, "Arrange returns to docked controls")
            }
            expect(app.workspaceToggleButtons[.library]?.state == (g.library > 0 ? .on : .off),
                   "Library toggle reflects actual responsive visibility")
            expect(app.workspaceToggleButtons[.inspector]?.state == (g.inspector > 0 ? .on : .off),
                   "Inspector toggle reflects actual responsive visibility")
            expect(app.workspaceToggleButtons[.dock]?.state == (g.dock > 0 ? .on : .off),
                   "Dock toggle reflects actual responsive visibility")
            if screen != .arrange {
                expect(!app.shouldHandleWorkspaceClipDelete, "Delete cannot edit the hidden arrangement")
            }
            let transportRect = app.playButton.convert(app.playButton.bounds, to: root)
            let pickerRect = app.workspaceMode.convert(app.workspaceMode.bounds, to: root)
            expect(pickerRect.minX >= -1 && pickerRect.maxX <= root.bounds.width + 1,
                   "Header picker stays inside minimum window")
            expect(!pickerRect.intersects(transportRect), "Screen picker does not cover transport")
            if width == 1536 { screenshot("screen-\(screen.rawValue)-\(Int(width))") }
        }
    }
    expect(revision() == initialRevision && app.dirty == initialDirty, "Navigation is not an Undo/project mutation")
    expect(app.playheadFrame == originalPlayhead && app.isPlaying == initialPlaying && app.isRecording == initialRecording,
           "Navigation never starts, stops or seeks transport")
    expect(app.selectedMixerID == midiSelection && app.midiClipIndex == midiClip,
           "Selected MIDI clip survives switching all screens")
    expect(app.inspectorBrowser.midiEditor.notes.count == midiNoteCount, "Bound MIDI notes survive")
    expect(identities == [ObjectIdentifier(app.inspectorBrowser.midiEditor),
                          ObjectIdentifier(app.mixerWorkspace), ObjectIdentifier(app.channelRack),
                          ObjectIdentifier(app.libraryBrowser)], "All production view identities are retained")
    expect(app.inspectorBrowser.midiEditor.isDescendant(of: dock) && app.mixerWorkspace.isDescendant(of: dock),
           "Editors remain in their original dock instead of duplicated windows")

    // Click the native dock action and Close; both restore ordinary layout.
    choose(.arrange); workspace.selectDock(.mixer)
    let dockedLayout = workspace.preference
    dock.focusButton.performClick(nil)
    expect(workspace.screen == .mixer && dock.expanded, "Native expand button opens the actual mixer")
    dock.focusButton.performClick(nil)
    expect(workspace.screen == .arrange && workspace.preference == dockedLayout, "Native return button preserves arrangement")
    choose(.pianoRoll); dock.closeButton.performClick(nil)
    expect(workspace.screen == .arrange && workspace.preference == dockedLayout, "Closing dedicated editor returns without hiding normal dock")

    // Menu dispatch follows the same route; Escape works from the workspace,
    // while a field editor retains its own native Escape semantics.
    guard let browserItem = destinations.first(where: { $0.tag == WorkspaceScreen.browser.rawValue }) else {
        fatalError("No browser menu item")
    }
    expect(NSApp.sendAction(browserItem.action!, to: browserItem.target, from: browserItem), "Dispatch native menu")
    expect(workspace.screen == .browser, "Menu opens actual browser")
    _ = app.window.makeFirstResponder(workspace)
    guard let escape = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [],
        timestamp: 0, windowNumber: app.window.windowNumber, context: nil,
        characters: "\u{1b}", charactersIgnoringModifiers: "\u{1b}", isARepeat: false, keyCode: 53) else {
        fatalError("No Escape event")
    }
    workspace.keyDown(with: escape)
    expect(workspace.screen == .arrange, "Escape returns to arrange from the workspace responder")

    // Native UI changes must not strand a real unfinished console gesture.
    choose(.mixer)
    guard let track = app.mixerWorkspace.strips.first(where: { $0.kind == .track }) else {
        fatalError("No actual mixer track")
    }
    app.consoleBegin(track.id, send: nil)
    expect(app.consoleGesture != nil, "Real console gesture begins")
    choose(.browser)
    expect(workspace.screen == .mixer && app.workspaceMode.selectedSegment == WorkspaceScreen.mixer.rawValue,
           "Unfinished gesture rejects and re-projects header selection")
    let blockedRevision = revision()
    app.mixerWorkspace.onVolumeGestureCancel?()
    expect(app.consoleGesture == nil && revision() == blockedRevision, "Cancelled gesture neither commits nor becomes stranded")
    choose(.browser)
    expect(workspace.screen == .browser, "Navigation resumes after cancellation")

    // Viewport is preserved, including after focused content expands the center.
    choose(.arrange); workspace.selectDock(.devices); settle(1060,700)
    app.setTimelineZoom(2); root.layoutSubtreeIfNeeded()
    app.restoreArrangementViewport(NSPoint(x:100,y:100))
    let viewport = app.timelineScroll.contentView.bounds.origin
    expect(viewport.x > 50 && viewport.y > 50, "Viewport regression fixture scrolls both axes")
    let beforeFocusLayout = workspace.preference
    choose(.pianoRoll); choose(.mixer); choose(.browser); choose(.arrange)
    let returned = app.timelineScroll.contentView.bounds.origin
    expect(abs(returned.x - viewport.x) < 1 && abs(returned.y - viewport.y) < 1,
           "Arrangement scroll survives editor/browser round trip")
    expect(workspace.preference == beforeFocusLayout, "Full-window dimensions never overwrite docked preferences")
    expect(revision() == initialRevision, "All UI actions leave session revision unchanged")

    app.selectedMixerID = originalSelection
    if let id = originalSelection { app.updateMixerInspector(id) }
    workspace.resetLayout(); app.setTimelineZoom(originalZoom); settle(1536,1000)
    app.restoreArrangementViewport(originalViewport)
    expect(workspace.preference == initialLayout, "Test restores normal workspace layout")
    print("PASS: \(checks) native workspace screen assertions")
    return checks
}
