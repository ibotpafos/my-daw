import AppKit

@MainActor
func runRackParameterTests(_ controller: DraftApp) -> Int {
    var count = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        count += 1
        if !condition() { fatalError("Inline parameters: \(message)") }
    }
    guard let session = controller.session, let track = controller.selectedMixerID,
          let plugin = controller.channelRack.devices.first else { fatalError("No actual AU fixture") }
    let rack = controller.channelRack, panel = rack.parameterPanel
    func revision() -> UInt64 {
        var out = daw_snapshot(); out.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &out) == 0, "Read revision"); return out.revision
    }
    func request() -> RackParameterRequest { guard let request = panel.request else { fatalError("No parameter request") }; return request }
    let read = rack.onReadParameters
    var reads = 0
    rack.onReadParameters = { request in reads += 1; return read?(request) ?? .unavailable("No reader") }
    controller.refreshDeviceRack()
    expect(reads == 0, "Collapsed cards never instantiate plug-ins for parameter metadata")
    let before = revision()
    rack.parameterButtons[0].performClick(nil)
    expect(reads == 1 && rack.parameterPluginID == plugin.id, "Native card button loads only chosen plug-in")
    expect(revision() == before && panel.page?.parameters.count == 4, "Read does not mutate; bounded page")
    expect(panel.isDescendant(of: rack), "Actual parameter panel embedded in the device rack")
    expect(panel.controls.allSatisfy { !$0.slider.isContinuous && $0.slider.sliderType == .circular }, "Native knobs commit at release only")
    guard let control = panel.controls.first(where: { $0.parameter.writable }) else { fatalError("No writable system AU parameter") }
    expect(controller.window.makeFirstResponder(control.slider), "Native knob can receive keyboard focus")
    expect(!controller.shouldHandleWorkspaceClipDelete, "Delete focused in the rack cannot delete arrangement clips")
    controller.window.makeFirstResponder(nil)
    let id = control.parameter.id, original = control.parameter.value
    let initial = request()
    let desiredPosition = abs(control.parameter.position - 0.37) > 0.1 ? 0.37 : 0.73
    control.slider.doubleValue = desiredPosition
    control.slider.sendAction(control.slider.action, to: control.slider.target)
    expect(revision() == before + 1, "One native knob action creates one domain revision")
    let applied = panel.page!.parameters.first(where: { $0.id == id })!.value
    expect(abs(applied - original) > 0.00001, "Readback comes from changed AU state")
    expect(panel.request != initial, "Applied command invalidates detached controls")
    expect(!controller.commitRackParameter(initial, id: id, value: original), "Stale old knob cannot replay its command")
    controller.undo()
    expect(abs(panel.page!.parameters.first(where: { $0.id == id })!.value - original) < 0.00001, "Undo restores visible AU value")
    controller.redo()
    expect(abs(panel.page!.parameters.first(where: { $0.id == id })!.value - applied) < 0.00001, "Redo readback")
    let unchanged = revision()
    expect(controller.commitRackParameter(request(), id: id, value: applied), "Same value accepted as no-op")
    expect(revision() == unchanged, "No-op does not dirty project or create history")
    for invalid in [Double.nan, .infinity, -Double.greatestFiniteMagnitude, Double.greatestFiniteMagnitude] {
        expect(!controller.commitRackParameter(request(), id: id, value: invalid), "Reject invalid native parameter value")
    }
    expect(!controller.commitRackParameter(request(), id: id, value: .nan), "NaN rejected")
    expect(!controller.commitRackParameter(request(), id: UInt32.max, value: 0), "Reject unknown parameter")
    let numeric = panel.controls.first(where: { $0.parameter.id == id })!
    numeric.valueField.stringValue = "nan"
    numeric.valueField.sendAction(numeric.valueField.action, to: numeric.valueField.target)
    expect(numeric.valueField.stringValue == numeric.parameter.displayValue && revision() == unchanged, "Invalid native text field restores actual value")
    numeric.valueField.stringValue = String(original)
    numeric.valueField.sendAction(numeric.valueField.action, to: numeric.valueField.target)
    expect(revision() == unchanged + 1, "Numeric entry uses same domain command")
    expect(abs(panel.page!.parameters.first(where: { $0.id == id })!.value - original) < 0.00001, "Numeric entry is read back, not optimistically displayed")
    let paging = revision(), priorPage = request()
    panel.nextButton.performClick(nil)
    expect(panel.request?.offset == 4 && revision() == paging, "Native pagination does not change project")
    expect(!panel.commit(id, value: applied, request: priorPage), "Off-page control invalidated")
    panel.previousButton.performClick(nil)
    expect(panel.request?.offset == 0, "Previous page")
    for midi in [false, true] {
        let unlocked = request()
        controller.isRecording = !midi; controller.midiTakeArmed = midi; controller.updateWorkspaceChrome()
        expect(panel.controls.allSatisfy { !$0.slider.isEnabled && !$0.valueField.isEnabled }, "Capture disables parameter controls")
        expect(!controller.commitRackParameter(unlocked, id: id, value: applied), "Controller capture guard also rejects a stale callback")
        controller.isRecording = false; controller.midiTakeArmed = false; controller.updateWorkspaceChrome()
    }
    expect(revision() == paging, "Invalid, paging and capture actions preserve project history")
    let staleTrack = request()
    controller.selectedMixerID = 0; controller.updateMixerInspector(0)
    expect(rack.parameterPluginID == nil, "Selecting another channel closes old parameter page")
    expect(!controller.commitRackParameter(staleTrack, id: id, value: applied), "Detached track callback cannot affect master")
    controller.selectedMixerID = track; controller.updateMixerInspector(track); rack.openParameters(plugin.id)
    // Exercise actual saved state across project replacement, without
    // opening dialogs, hardware or vendor editors in an offscreen test process.
    let path = FileManager.default.temporaryDirectory.appendingPathComponent("rack-\(UUID().uuidString).mydawdraft")
    defer { try? FileManager.default.removeItem(at: path) }
    expect(daw_save_draft(session, path.path) == 0, "Save actual session containing parameter state")
    expect(controller.commitRackParameter(request(), id: id, value: applied), "Change state after saving")
    let beforeOpen = request()
    expect(daw_open_draft(session, path.path) == 0, "Reopen original state in same bridge handle")
    controller.refresh(); controller.selectedMixerID = track; controller.updateMixerInspector(track)
    expect(abs(panel.page!.parameters.first(where: { $0.id == id })!.value - original) < 0.00001, "Open restores stored state; no previous project's value leaks")
    expect(!controller.commitRackParameter(beforeOpen, id: id, value: applied), "Reopen invalidates captured request regardless of numeric revision")
    let requestsBeforePolling = reads
    for _ in 0..<5 { controller.pollTransport(); controller.updateWorkspaceChrome() }
    expect(reads == requestsBeforePolling, "Transport polling never enumerates plug-in parameters")
    controller.window.contentView?.layoutSubtreeIfNeeded()
    expect(panel.controls.allSatisfy { $0.frame.width > 60 && $0.frame.maxX <= panel.bounds.width + 1 }, "Native knobs fit within the embedded card")
    expect(rack.parameterButtons.filter { $0.isDescendant(of: rack) }.allSatisfy { $0.frame.width >= 120 }, "Inline button captions retain readable width")
    rack.onReadParameters = read
    print("PASS: \(count) native inline parameter, Undo, pagination, capture guard and save/open assertions")
    return count
}
