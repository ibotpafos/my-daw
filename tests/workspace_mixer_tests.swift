import AppKit

/// Exercises the joined application, not a second console or mixer model.
@MainActor
func runWorkspaceMixerTests(_ app: DraftApp) -> Int {
    var checks = 0
    func expect(_ value: @autoclosure () -> Bool, _ message: String) {
        checks += 1
        if !value() { fatalError("Workspace/mixer integration: \(message)") }
    }
    guard let session = app.session, let workspace = app.workspace,
          let dock = app.workspaceDock, let root = app.window.contentView else {
        fatalError("Actual mixer workspace missing")
    }
    func snapshot() -> daw_snapshot {
        var value = daw_snapshot(); value.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &value) == 0, "Read actual session")
        return value
    }
    func track() -> daw_track {
        var value = daw_track(); value.struct_size = UInt32(MemoryLayout<daw_track>.size)
        expect(daw_get_track(session, 0, &value) == 0, "Read actual first track")
        return value
    }
    func settle() {
        for _ in 0..<3 { root.layoutSubtreeIfNeeded(); workspace.applyGeometry() }
    }
    app.refresh(); workspace.resetLayout(); workspace.selectDock(.mixer); settle()
    let preference = workspace.preference
    let beforeFocus = snapshot().revision
    expect(app.mixerWorkspace.isDescendant(of: dock), "Production console mounted in existing dock")
    expect(workspace.geometry.dock >= 400 && workspace.geometry.arrangement >= 220,
           "Ordinary console and arrangement remain usable together")
    app.mixerWorkspace.onFocus?(true); settle()
    expect(workspace.dockFocused && workspace.geometry.arrangement == 0,
           "Focus uses actual workspace instead of obsolete splitters")
    expect(workspace.geometry.library == 0 && workspace.geometry.inspector == 0,
           "Focused console owns the available workspace")
    expect(workspace.geometry.dock == workspace.bounds.height, "Focus retains full console height")
    expect(workspace.preference == preference, "Focus does not overwrite normal pane preferences")
    app.mixerWorkspace.onFocus?(false); settle()
    expect(!workspace.dockFocused && workspace.geometry.arrangement >= 220,
           "Exiting focus restores arrangement")
    expect(workspace.geometry.library > 0 && workspace.geometry.inspector > 0,
           "Exiting focus restores both sidebars")
    expect(workspace.preference == preference && snapshot().revision == beforeFocus,
           "Window presentation is not a project edit")
    app.mixerWorkspace.onFocus?(true); workspace.selectDock(.devices); settle()
    expect(!workspace.dockFocused && workspace.geometry.arrangement > 0,
           "Leaving console tab exits focus")

    app.refresh(); app.updateMixExportAvailability()
    let initial = track(); let before = snapshot().revision
    expect(app.exportButton.isEnabled, "Material can export before mixer edit")
    app.consoleBegin(initial.id, send: nil)
    expect(app.consoleGesture != nil && !app.exportButton.isEnabled,
           "Console gesture immediately disables export")
    app.consoleWrite(initial.gain_db - 1)
    expect(snapshot().revision == before && track().gain_db == initial.gain_db,
           "Preview never leaks into committed snapshot")
    expect(app.mixExportPolicy().gesture, "Direct export shares console gesture guard")
    app.consoleEnd()
    expect(snapshot().revision == before + 1 && track().gain_db == initial.gain_db - 1,
           "One real console commit consumes one revision")
    expect(app.exportButton.isEnabled && app.consoleGesture == nil,
           "Commit restores export availability")
    app.undo()
    expect(track().gain_db == initial.gain_db, "Project Undo restores committed console level")

    let beforeCapture = snapshot().revision
    app.midiTakeArmed = true; app.updateWorkspaceChrome()
    expect(!app.mixerWorkspace.editingEnabled, "MIDI capture disables console editing")
    app.consoleBegin(initial.id, send: nil)
    app.mixerWorkspace.onMute?(initial.id, initial.muted == 0)
    app.mixerWorkspace.onSolo?(initial.id, true)
    expect(app.consoleGesture == nil && snapshot().revision == beforeCapture,
           "Stale console callbacks cannot mutate during MIDI capture")
    app.midiTakeArmed = false; app.updateWorkspaceChrome()
    app.consoleBegin(initial.id, send: nil); app.consoleWrite(initial.gain_db - 2)
    app.midiTakeArmed = true; app.consoleEnd()
    expect(app.consoleGesture == nil && snapshot().revision == beforeCapture && track().gain_db == initial.gain_db,
           "Capture starting during gesture cancels instead of committing")
    app.midiTakeArmed = false; app.refresh(); workspace.resetLayout(); settle()
    expect(app.exportButton.isEnabled && app.mixerWorkspace.editingEnabled,
           "Cancelled capture preview leaves application usable")
    print("PASS: \(checks) joined workspace/mixer assertions")
    return checks
}
