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
    controller.applicationDidFinishLaunching(Notification(name: NSApplication.didFinishLaunchingNotification))
    guard let session = controller.session, let root = controller.window.contentView,
          let workspace = controller.workspace, let dock = controller.workspaceDock else { fatalError("Missing actual workspace") }
    let temporary = FileManager.default.temporaryDirectory.appendingPathComponent("mydaw-workspace-\(UUID().uuidString)", isDirectory: true)
    do { try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true) } catch { fatalError("Temporary fixtures: \(error)") }
    defer {
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
    screenshot("workspace-empty")

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
                var notes = (0..<24).map { number -> daw_midi_note in
                    var note = daw_midi_note(); note.struct_size = UInt32(MemoryLayout<daw_midi_note>.size); note.version = UInt32(DAW_MIDI_NOTE_VERSION)
                    note.start = UInt64(number / 3 * 24000); note.length = 18000
                    note.pitch = UInt8(40 + index * 9 + [0, 4, 7][number % 3] + (number / 6 % 2) * 2); note.velocity = 90; return note
                }
                expect(daw_add_midi_clip(session, track.id, &clip, &notes, UInt32(notes.count), revision()) == 0, "Create MIDI clip")
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
        screenshot("workspace-\(Int(width))")
    }
    settle(1536,1000)
    workspace.selectDock(.mixer); root.layoutSubtreeIfNeeded(); screenshot("workspace-mixer")
    controller.selectedMixerID = tracks[2]; controller.updateMixerInspector(tracks[2]); workspace.selectDock(.midi)
    root.layoutSubtreeIfNeeded(); screenshot("workspace-midi")
    print("PASS: \(assertions) native workspace assertions; actual AppKit composition, model commands, selection and screenshots")
}
