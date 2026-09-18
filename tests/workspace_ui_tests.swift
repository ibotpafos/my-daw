import AppKit
import Foundation

/// Compiled with the production source manifest. Only external startup side
/// effects (recovery, vendor scanning, timers) are excluded by the test flag.
@MainActor
func runWorkspaceIntegrationTests() {
    var assertions = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        assertions += 1
        if !condition() { fatalError("Workspace assertion: \(message)") }
    }
    let app = NSApplication.shared
    app.setActivationPolicy(.prohibited)
    let controller = DraftApp()
    let previousDelegate = app.delegate
    app.delegate = controller
    controller.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    guard let session = controller.session, let root = controller.window.contentView,
          let workspace = controller.workspace, let dock = controller.workspaceDock else { fatalError("Missing actual workspace") }
    let temporary = FileManager.default.temporaryDirectory.appendingPathComponent("mydaw-workspace-\(UUID().uuidString)", isDirectory: true)
    do { try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true) } catch { fatalError("Temporary fixtures: \(error)") }
    defer {
        app.delegate = previousDelegate
        NotificationCenter.default.removeObserver(controller)
        controller.window.orderOut(nil); controller.window.delegate = nil
        daw_destroy(session); controller.session = nil
        try? FileManager.default.removeItem(at: temporary)
    }
    func revision() -> UInt64 {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &snapshot) == 0, "Read session revision")
        return snapshot.revision
    }
    func settle(_ width: CGFloat, _ height: CGFloat) {
        controller.window.setContentSize(NSSize(width: width, height: height))
        for _ in 0..<4 {
            root.layoutSubtreeIfNeeded(); workspace.applyGeometry()
            controller.fitTrackHeaderWidth(); controller.fitTimelineViewport(); root.layoutSubtreeIfNeeded()
        }
    }
    func screenshot(_ name: String) {
        root.layoutSubtreeIfNeeded()
        guard let bitmap = root.bitmapImageRepForCachingDisplay(in: root.bounds) else { fatalError("No AppKit bitmap") }
        root.cacheDisplay(in: root.bounds, to: bitmap)
        guard let data = bitmap.representation(using: .png, properties: [:]) else { fatalError("No PNG") }
        do { try data.write(to: URL(fileURLWithPath: "build/workspace-ui/\(name).png")) } catch { fatalError("PNG write: \(error)") }
        print("SNAPSHOT \(name) \(Int(root.bounds.width))x\(Int(root.bounds.height))")
    }
    controller.restoreWorkspaceLayout(); workspace.resetLayout(); settle(1536, 1000)
    expect(workspace.geometry.library > 0 && workspace.geometry.inspector > 0, "Simultaneous library and inspector")
    expect(controller.inspectorBrowser.midiEditor.isDescendant(of: dock), "Existing MIDI editor is actually mounted in dock")
    let editorIdentity = ObjectIdentifier(controller.inspectorBrowser.midiEditor)
    let beforeLayout = revision()
    for _ in 0..<3 {
        for pane in [WorkspaceLayout.Pane.library, .inspector, .dock] { workspace.toggle(pane); workspace.toggle(pane) }
        for tab in WorkspaceDockTab.allCases { workspace.selectDock(tab) }
    }
    expect(revision() == beforeLayout, "Layout and tab changes never mutate project")
    expect(ObjectIdentifier(controller.inspectorBrowser.midiEditor) == editorIdentity, "Editor identity retained")
    expect(controller.timelineRuler.cycleRange.playableFrames == 0, "Empty project has no editable loop duration")
    screenshot("workspace-empty")

    // Menu commands retain the original control as sender and recheck state.
    var menuCalls = 0
    let source = WorkspaceActionButton("Test command") { menuCalls += 1 }
    let popup = WorkspaceCommandMenu("Test", sources: [source])
    guard let commandMenu = popup.menu else { fatalError("Missing native menu") }
    popup.menuNeedsUpdate(commandMenu)
    expect(commandMenu.numberOfItems == 2, "One source command plus native pull-down heading")
    let staleItem = commandMenu.items[1]
    popup.dispatch(staleItem); expect(menuCalls == 1, "Menu dispatches the original button")
    source.isEnabled = false
    popup.dispatch(staleItem); expect(menuCalls == 1, "Stale menu cannot bypass a disabled control")
    popup.menuNeedsUpdate(commandMenu)
    expect(!commandMenu.items[1].isEnabled, "Menu refresh projects current availability")
    source.isEnabled = true; source.isHidden = true
    popup.dispatch(staleItem); expect(menuCalls == 1, "Hidden command cannot be dispatched")
    popup.menuNeedsUpdate(commandMenu)
    expect(commandMenu.numberOfItems == 1, "Hidden command is absent")
    let foreign = NSMenuItem(title: "Foreign", action: nil, keyEquivalent: "")
    foreign.representedObject = WorkspaceActionButton("Foreign") { menuCalls += 100 }
    popup.dispatch(foreign); expect(menuCalls == 1, "Unrelated controls rejected")

    // Exercise real browser table selection, search, availability and callback guards.
    let browser = controller.libraryBrowser
    let wave = temporary.appendingPathComponent("Pulse.wav")
    let audio = InspectorBrowserItem(title: "Pulse.wav", detail: "Test fixture · WAV", available: true, category: .audio, sourceURL: wave)
    let missing = InspectorBrowserItem(title: "Missing.wav", available: false, category: .audio)
    browser.audioItems = [audio, missing]
    let instrument = InspectorBrowserItem(title: "Installed instrument", detail: "AU MusicDevice", available: true, category: .instruments)
    let effect = InspectorBrowserItem(title: "Installed effect", detail: "VST3", available: true, category: .effects)
    browser.pluginItems = [instrument, effect]
    browser.selectCategory(.audio); browser.selectItem(id: audio.id)
    expect(browser.table.selectedRow == 0 && browser.addButton.isEnabled, "Audio selection")
    expect(browser.tableView(browser.table, pasteboardWriterForRow: 0) != nil, "Native file URL drag payload")
    browser.setQuery("missing"); expect(browser.visibleItems.count == 1, "Case-insensitive search")
    browser.selectItem(id: missing.id); expect(!browser.addButton.isEnabled && !browser.previewButton.isEnabled, "Missing file cannot add/preview")
    browser.setQuery(""); browser.selectItem(id: audio.id)
    let originalAdd = browser.onAdd
    var additions = 0; browser.onAdd = { _, _ in additions += 1 }
    browser.isImportBusy = true; browser.addSelected(); expect(additions == 0, "Busy import guard")
    browser.isImportBusy = false; browser.addSelected(); expect(additions == 1, "Enabled add dispatch")
    browser.mutationEnabled = false; browser.addSelected(); expect(additions == 1, "Recording mutation guard")
    browser.mutationEnabled = true; browser.onAdd = originalAdd
    browser.selectCategory(.instruments); expect(browser.visibleItems.map(\.id) == [instrument.id], "Instrument/effect distinction")
    browser.selectCategory(.effects); expect(browser.visibleItems.map(\.id) == [effect.id], "Effect category")
    browser.selectCategory(.audio)

    // This material exists only in the test. It becomes actual session media
    // through the public ABI; production never auto-populates example tracks.
    func writeWav(_ url: URL, variation: Int) throws {
        let frames = 48000 * 12
        var bytes = Data()
        func tag(_ text: String) { bytes.append(contentsOf: text.utf8) }
        func put(_ value: UInt32, _ count: Int) { for byte in 0..<count { bytes.append(UInt8(truncatingIfNeeded: value >> (byte * 8))) } }
        tag("RIFF"); put(UInt32(36 + frames * 2), 4); tag("WAVEfmt "); put(16,4); put(1,2); put(1,2)
        put(48000,4); put(96000,4); put(2,2); put(16,2); tag("data"); put(UInt32(frames * 2),4)
        for frame in 0..<frames {
            let seconds = Double(frame) / 48000
            let pulse = exp(-Double(frame % (6000 + variation * 1100)) / Double(900 + variation * 180))
            let signal = sin(seconds * 2 * .pi * Double(80 + variation * 51)) * (0.12 + 0.65 * pulse)
            put(UInt32(truncatingIfNeeded: Int32(signal * 28000)), 2)
        }
        try bytes.write(to: url)
    }
    let names = ["Drums", "Bass", "Synth Lead", "Chord Pad", "Vocals", "Guitar", "FX", "Atmos"]
    let colors: [UInt32] = [0x528FE6,0xD16477,0x62BA89,0x9D75E5,0xB37CE8,0xD7B35A,0x4CB8C6,0x6290CC]
    var tracks: [UInt64] = []
    for (index, name) in names.enumerated() {
        let midi = (1...3).contains(index)
        if midi { expect(daw_add_track(session, name, revision()) == 0, "Add MIDI track") }
        else {
            let path = temporary.appendingPathComponent("\(name).wav")
            do { try writeWav(path, variation: index) } catch { fatalError("WAV fixture") }
            expect(daw_import_wav(session, path.path, name, revision()) == 0, "Import audio through ABI")
            let item = InspectorBrowserItem(title: path.lastPathComponent, detail: "WAV · тестовый материал", category: .audio, sourceURL: path)
            browser.audioItems.append(item); controller.browserAudioURLs[item.id] = path
        }
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(session, UInt32(index), &track) == 0, "Read new track")
        tracks.append(track.id)
        expect(daw_set_track_color(session, track.id, colors[index], revision()) == 0, "Persist track color")
        if midi {
            for clipIndex in 0..<3 {
                var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size); clip.version = UInt32(DAW_MIDI_CLIP_VERSION)
                clip.start = UInt64(clipIndex * 192000); clip.length = 192000; clip.note_count = 24
                let intervals = [0, 4, 7]
                var notes: [daw_midi_note] = []
                notes.reserveCapacity(24)
                for number in 0..<24 {
                    var note = daw_midi_note()
                    note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
                    note.version = UInt32(DAW_MIDI_NOTE_VERSION)
                    let group = number / 3
                    note.start = UInt64(group * 24000)
                    note.length = 18000
                    let interval = intervals[number % intervals.count]
                    let octaveOffset = (number / 6 % 2) * 2
                    let pitch = 40 + index * 9 + interval + octaveOffset
                    note.pitch = UInt8(pitch)
                    note.velocity = 90
                    notes.append(note)
                }
                let addMidiResult = notes.withUnsafeBufferPointer { buffer in
                    daw_add_midi_clip(session, track.id, &clip, buffer.baseAddress, UInt32(buffer.count), revision())
                }
                expect(addMidiResult == 0, "Create MIDI clip")
            }
        } else {
            expect(daw_split_clip(session, track.id, 0, 192000, revision()) == 0, "Split first audio section")
            expect(daw_split_clip(session, track.id, 1, 384000, revision()) == 0, "Split second audio section")
        }
    }
    for (frame, name) in [(UInt64(0), "Intro"), (192000, "Verse"), (384000, "Chorus")] {
        expect(daw_add_marker(session, frame, name, revision()) == 0, "Create real marker")
    }
    controller.selectedMixerID = tracks[4]; controller.refresh(); controller.pollTransport()
    settle(1536, 1000)
    let ruler = controller.timelineRuler
    func mouse(_ point: NSPoint, clicks: Int = 1, right: Bool = false) -> NSEvent {
        let location = ruler.convert(point, to: nil)
        guard let event = NSEvent.mouseEvent(with: right ? .rightMouseDown : .leftMouseDown,
            location: location, modifierFlags: [], timestamp: 0, windowNumber: controller.window.windowNumber,
            context: nil, eventNumber: 0, clickCount: clicks, pressure: 1) else { fatalError("Mouse fixture") }
        return event
    }
    expect(ruler.markerRects.count == 3, "All session point markers projected")
    let verseRect = ruler.markerRects[1].rect
    let versePoint = NSPoint(x: verseRect.midX, y: verseRect.midY)
    let beforeSeek = revision()
    ruler.mouseDown(with: mouse(versePoint))
    expect(controller.playheadFrame == 192000, "Real marker click seeks the actual session")
    expect(revision() == beforeSeek, "Marker navigation does not mutate project")
    controller.isRecording = true
    ruler.mouseDown(with: mouse(NSPoint(x: 0, y: 12)))
    expect(controller.playheadFrame == 192000, "Ruler retains existing recording seek guard")
    controller.isRecording = false
    expect((ruler.menu(for: mouse(versePoint, right: true))?.numberOfItems ?? 0) >= 2,
           "Right click returns actual marker rename/delete menu")
    let originalAddMarker = ruler.onMarkerAdd
    var addedMarker: UInt64?
    ruler.onMarkerAdd = { addedMarker = $0 }
    let addPoint = NSPoint(x: ruler.bounds.width * 0.75, y: 10)
    ruler.mouseDown(with: mouse(addPoint, clicks: 2))
    expect(addedMarker == ruler.frame(atX: addPoint.x), "Double click dispatches bounded marker insertion position")
    ruler.onMarkerAdd = originalAddMarker
    expect(ruler.frame(atX: -10) == 0 && ruler.frame(atX: .nan) == 0, "Invalid/negative ruler coordinate")
    expect(ruler.frame(atX: ruler.bounds.width) == ruler.projectFrames, "Right edge is exact project endpoint")
    expect(ruler.marker(at: NSPoint(x: versePoint.x, y: TimelineRulerView.markerBand + 1)) == nil,
           "Marker hit tests do not leak into musical ruler")
    for pair in zip(ruler.markerRects, ruler.markerRects.dropFirst()) {
        expect(pair.0.rect.maxX <= pair.1.rect.minX, "Marker chips never overlap")
    }
    controller.seekAudio(0)

    expect(controller.midiArrangementViews.count == 3, "MIDI tracks have real arrangement overview")
    expect(controller.midiArrangementViews[0].clips.count == 3 && controller.midiArrangementViews[0].clips[0].notes.count == 24, "All MIDI clips/notes projected")
    controller.midiArrangementViews[0].onSelect?(1, true)
    expect(controller.selectedMixerID == tracks[1] && controller.midiClipIndex == 1 && dock.selected == .midi, "MIDI overview selects same clip in dock")
    expect(controller.inspectorBrowser.midiEditor.notes.count == 24, "Existing editor receives selected clip")
    controller.selectedMixerID = tracks[4]; controller.updateMixerInspector(tracks[4]); workspace.selectDock(.devices)
    expect(controller.inspectorBrowser.midiEditor.notes.isEmpty, "Audio selection clears stale MIDI editor")

    controller.updateClipInspector(tracks[4], 0)
    let beforeInvalid = revision()
    for value in ["nan", "inf", "-1", "1e300", "not a number"] {
        expect(!controller.inspectorBrowser.commitClipValues([value,"0","4","0","0"]), "Malformed clip edit rejected")
    }
    expect(revision() == beforeInvalid, "Malformed numeric edits do not mutate session")
    controller.inspectorBrowser.volume.doubleValue = -3
    controller.inspectorBrowser.changeChannel()
    expect(revision() == beforeInvalid + 1, "Inspector fader dispatches actual domain command")
    controller.undo(); expect(controller.inspectorBrowser.channel?.volumeDb == 0, "Inspector change undo")

    expect(controller.inspectorBrowser.validationMessage == nil, "A new selection clears stale validation")

    // Selection and ordinary edits must retain a zoomed/scrolled arrangement.
    settle(1060, 700); controller.setTimelineZoom(2)
    root.layoutSubtreeIfNeeded()
    controller.restoreArrangementViewport(NSPoint(x: 100, y: 100))
    let viewport = controller.timelineScroll.contentView.bounds.origin
    expect(viewport.x > 50 && viewport.y > 50, "Scroll test actually uses both axes")
    controller.selectedMixerID = tracks[1]; controller.inspectorTrackID = tracks[4]
    controller.refresh()
    let selectedHeaders = controller.trackHeaderRows.arrangedSubviews.compactMap { $0 as? PinnedTrackHeaderView }.filter { $0.model.selected }
    expect(selectedHeaders.count == 1 && selectedHeaders[0].model.id == tracks[1], "One authoritative selected track")
    expect(controller.timelineScroll.contentView.bounds.origin == viewport, "Selection refresh preserves viewport")
    controller.mixerSetVolume(tracks[4], -6)
    expect(controller.timelineScroll.contentView.bounds.origin == viewport, "Gain edit preserves viewport")
    controller.undo()
    expect(controller.timelineScroll.contentView.bounds.origin == viewport, "Undo preserves viewport")
    expect(abs(controller.trackHeaderScroll.contentView.bounds.origin.y - viewport.y) < 1, "Pinned headers follow retained viewport")
    controller.restoreArrangementViewport(NSPoint(x: 100000, y: 100000))
    let limited = controller.timelineScroll.contentView.bounds
    expect(limited.maxX <= (controller.timelineScroll.documentView?.bounds.width ?? 0) + 1, "Scroll restoration clamps to shortened width")
    controller.restoreArrangementViewport(.zero); controller.setTimelineZoom(1)
    controller.selectedMixerID = tracks[4]; controller.updateMixerInspector(tracks[4])
    settle(1536, 1000)

    // System AU only, no external plugin or audio device. Use the existing catalog.
    controller.loadSupportedAudioUnits(); controller.refreshBrowserCatalog()
    if let au = controller.auCatalog.first(where: { $0.type == 0x61756678 }) {
        for _ in 0..<2 { expect(daw_add_insert_au(session, Int32(DAW_INSERT_OWNER_TRACK), tracks[4], au.type, au.subtype, au.manufacturer, revision()) == 0, "Add system AU") }
        controller.refresh(); controller.updateMixerInspector(tracks[4])
        expect(controller.channelRack.devices.count == 2, "Rack shows real inserts")
        let first = controller.channelRack.devices[0].id
        let before = revision(); controller.channelRack.perform(.bypass(first))
        expect(revision() == before + 1 && controller.channelRack.devices[0].bypassed, "Rack bypass hits C ABI")
        controller.undo(); expect(!controller.channelRack.devices[0].bypassed, "Rack bypass undo")
        controller.channelRack.perform(.move(first, 1)); expect(controller.channelRack.devices[1].id == first, "Rack reorder")
        controller.undo()
        controller.channelRack.perform(.remove(first)); expect(controller.channelRack.devices.count == 1, "Rack remove")
        controller.undo(); expect(controller.channelRack.devices.count == 2, "Rack remove undo")
        controller.isRecording = true; controller.refreshDeviceRack()
        let locked = revision(); controller.channelRack.perform(.remove(first)); expect(revision() == locked, "Rack recording guard")
        controller.isRecording = false; controller.refreshDeviceRack()
    } else { fatalError("Required system AU effect missing; not a passing skip") }
    assertions += runSignalChainTests(controller)
    controller.window.setContentSize(NSSize(width: 1536, height: 1000))
    assertions += runWorkspaceMixerTests(controller)
    browser.audioItems.removeAll(where: { $0.id == audio.id || $0.id == missing.id })
    controller.currentURL = temporary.appendingPathComponent("Midnight Feelings.mydawdraft")
    controller.updateWorkspaceChrome(); workspace.resetLayout()
    for (width, height) in [(CGFloat(1536), CGFloat(1000)), (1060,700), (1920,1080)] {
        settle(width, height)
        expect(workspace.geometry.center >= 520, "Center remains usable")
        expect(controller.timelineScroll.frame.width >= 320, "Timeline viewport minimum")
        for pane in workspace.columns.subviews where !pane.isHidden {
            expect(pane.frame.minX >= -1 && pane.frame.maxX <= workspace.bounds.width + 1, "Pane inside workspace")
        }
        expect(abs((controller.timelineWidthConstraint?.constant ?? 0) - controller.timelineScroll.contentSize.width) < 2, "Zoom 1 fits viewport")
        expect(controller.trackHeaderRows.arrangedSubviews.count == controller.rows.arrangedSubviews.count, "Pinned header and lane counts align")
        func descendants(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(descendants) }
        guard let bar = descendants(root).compactMap({ $0 as? WorkspaceCommandBar }).first else { fatalError("Actual command bar missing") }
        expect(bar.commands.arrangedSubviews.contains(where: { $0 === controller.undoButton }), "Original Undo control retained")
        expect(bar.commands.frame.width <= bar.scroll.contentSize.width + 1, "Frequent commands fit without horizontal scrolling")
        expect(controller.playButton.frame.height >= 28 && controller.recordButton.frame.height >= 28, "Readable transport hit targets")
        let headers = controller.trackHeaderRows.arrangedSubviews.compactMap { $0 as? PinnedTrackHeaderView }
        expect(headers.allSatisfy { abs($0.frame.height - PinnedTrackHeaderView.laneHeight) < 1 }, "Consistent compact lane height")
        screenshot("workspace-\(Int(width))")
    }
    settle(1536,1000)
    workspace.selectDock(.mixer); root.layoutSubtreeIfNeeded(); screenshot("workspace-mixer")
    controller.selectedMixerID = tracks[2]; controller.updateMixerInspector(tracks[2]); workspace.selectDock(.midi)
    root.layoutSubtreeIfNeeded(); screenshot("workspace-midi")
    // Comping imports a dedicated audio source. Run it before the arrangement
    // stress fixtures, which intentionally leave many temporary tracks/regions
    // in this shared integration session. This keeps both suites independent
    // without weakening either production path or assertion set.
    assertions += runRecordCompWorkspaceTests(controller)
    assertions += runTimelineEditingTests(controller)
    controller.selectedMixerID = tracks[4]; controller.updateMixerInspector(tracks[4]); workspace.selectDock(.devices)
    settle(1536, 1000); screenshot("workspace-cycle")
    assertions += runRackParameterTests(controller)
    settle(1536, 1000); screenshot("workspace-parameters")
    settle(1060, 700); screenshot("workspace-parameters-1060")
    assertions += runLibraryBrowserTests(controller)
    assertions += runArrangementOverviewTests(controller)
    assertions += runAudioDeviceSettingsTests(controller)
    assertions += runAudioHardwareSettingsTests(controller)
    settle(1536, 1000); screenshot("workspace-overview")
    settle(1060, 700); screenshot("workspace-overview-1060")
    print("PASS: \(assertions) native workspace assertions; actual AppKit composition, model commands, selection and screenshots")
}
