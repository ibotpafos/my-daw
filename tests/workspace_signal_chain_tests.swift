import AppKit

@MainActor
func runSignalChainTests(_ controller: DraftApp) -> Int {
    var assertions = 0
    func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        assertions += 1
        if !condition() { fatalError("Signal chain: \(message)") }
    }
    guard let session = controller.session, let trackID = controller.selectedMixerID else { fatalError("Missing selected track") }
    let inspector = controller.inspectorBrowser
    let chain = inspector.signalChain
    func revision() -> UInt64 {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &snapshot) == 0, "Read revision")
        return snapshot.revision
    }
    func context() -> InspectorChainContext {
        guard let value = chain.context else { fatalError("Missing chain context") }; return value
    }
    func refresh() { controller.refresh(); controller.selectedMixerID = trackID; controller.updateMixerInspector(trackID) }
    expect(chain.devices == controller.channelRack.devices && chain.devices.count == 2, "Inspector projects actual AU inserts")
    expect(chain.bypassButtons.count == 2 && chain.pluginButtons.allSatisfy(\.isEnabled), "Native plugin row controls")
    let firstID = chain.devices[0].id
    let beforeBypass = revision()
    let oldButton = chain.bypassButtons[0]
    oldButton.performClick(nil)
    expect(revision() == beforeBypass + 1 && chain.devices[0].bypassed, "Inspector power changes actual plugin bypass")
    let afterBypass = revision()
    oldButton.performClick(nil)
    expect(revision() == afterBypass, "Detached old row cannot apply at a later revision")
    controller.undo(); expect(!chain.devices[0].bypassed, "Bypass undo reflected in both surfaces")
    chain.performRack(.move(firstID, 1), context: context())
    expect(chain.devices[1].id == firstID && controller.channelRack.devices[1].id == firstID, "Reorder shared with dock")
    controller.undo()
    chain.performRack(.remove(firstID), context: context()); expect(chain.devices.count == 1, "Inspector removes real insert")
    controller.undo(); expect(chain.devices.count == 2, "Remove undo restores row")
    let staleChannel = context()
    controller.selectedMixerID = 0; controller.updateMixerInspector(0)
    let masterRevision = revision()
    chain.performRack(.remove(firstID), context: staleChannel)
    controller.performInspectorSend(staleChannel, .add(1))
    expect(revision() == masterRevision && chain.context?.target.owner == Int32(DAW_INSERT_OWNER_MASTER), "Old channel callbacks cannot hit master")
    expect(chain.addSendMenu?.isEnabled == false, "Master cannot create track sends")
    controller.selectedMixerID = trackID; controller.updateMixerInspector(trackID)

    var buses: [UInt64] = []
    for name in ["Reverb", "Delay"] {
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        expect(daw_get_snapshot(session, &snapshot) == 0, "Get bus count")
        expect(daw_add_bus(session, name, snapshot.revision) == 0, "Create actual send destination")
        var bus = daw_bus(); bus.struct_size = UInt32(MemoryLayout<daw_bus>.size)
        expect(daw_get_bus(session, snapshot.bus_count, &bus) == 0, "Read actual destination")
        buses.append(bus.id)
    }
    refresh()
    expect(chain.destinations.count == 2 && chain.addSendMenu?.isEnabled == true, "Native menu lists unused buses")
    let beforeSend = revision()
    chain.addSendMenu?.menu?.performActionForItem(at: 1)
    expect(revision() == beforeSend + 1 && chain.sends.count == 1 && chain.sends[0].busID == buses[0], "Native add menu reaches domain send command")
    expect(chain.sends[0].gainDb == -12 && !chain.sends[0].preFader, "Existing send defaults retained")
    expect(chain.destinations.count == 1, "Duplicate destination excluded")
    let slider = chain.sendSliders[0]
    expect(!slider.isContinuous, "One gain command per completed drag")
    slider.doubleValue = -18.4
    let beforeGain = revision()
    slider.sendAction(slider.action, to: slider.target)
    expect(revision() == beforeGain + 1 && chain.sends[0].gainDb == -18.4, "Native send fader changes domain gain")
    controller.undo(); expect(chain.sends[0].gainDb == -12, "Send gain undo")
    chain.sendPreButtons[0].performClick(nil); expect(chain.sends[0].preFader, "Native PRE/POST changes actual route")
    controller.undo(); expect(!chain.sends[0].preFader, "Send mode undo")
    let beforeInvalid = revision()
    for value in [Double.nan, .infinity, -121, 25] {
        chain.performSend(.gain(buses[0], value), context: context())
        controller.performInspectorSend(context(), .gain(buses[0], value))
    }
    chain.performSend(.add(buses[0]), context: context())
    chain.performSend(.remove(UInt64.max), context: context())
    expect(revision() == beforeInvalid, "Invalid numbers, missing routes and duplicate adds do not mutate")
    let staleSendContext = context()
    chain.sendRemoveButtons[0].performClick(nil); expect(chain.sends.isEmpty, "Native remove deletes send")
    controller.performInspectorSend(staleSendContext, .gain(buses[0], -6))
    expect(chain.sends.isEmpty, "Stale gain cannot resurrect removed send")
    controller.undo(); expect(chain.sends.count == 1, "Send removal undo")
    for midi in [false, true] {
        let old = context()
        controller.isRecording = !midi; controller.midiTakeArmed = midi
        controller.refreshDeviceRack()
        let locked = revision()
        expect(!chain.editable && chain.bypassButtons.allSatisfy { !$0.isEnabled }, "Recording disables native controls")
        chain.performRack(.remove(firstID), context: old)
        controller.performInspectorSend(old, .remove(buses[0]))
        expect(revision() == locked, "Controller also blocks stale recording callbacks")
        controller.isRecording = false; controller.midiTakeArmed = false
        controller.refreshDeviceRack()
    }
    chain.performSend(.add(buses[1]), context: context())
    expect(chain.sends.count == 2 && chain.destinations.isEmpty, "Two real sends available in inspector")
    controller.updateClipInspector(trackID, 0)
    // Clip-focused channel retains track capabilities (kind is TRACK · CLIP).
    func descendants(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(descendants) }
    guard let solo = descendants(inspector).compactMap({ $0 as? NSButton }).first(where: { $0.title == "Solo" }) else { fatalError("Missing inspector solo") }
    expect(solo.isEnabled, "Track Solo remains enabled with a clip selected")
    let beforeSolo = revision(); solo.performClick(nil)
    expect(revision() == beforeSolo + 1 && inspector.channel?.solo == true, "Clip-focused Solo dispatches track command")
    controller.undo()
    expect(controller.laneViews[trackID]?.selectionActive == true, "Selected audio track shows remembered clip selection")
    expect(controller.laneViews.filter { $0.key != trackID }.values.allSatisfy { !$0.selectionActive }, "Unselected tracks do not look selected")
    expect(controller.laneViews[trackID]?.trackTitle == inspector.channel?.title, "Audio clip caption uses real track name")
    expect(chain.context?.target.owner == Int32(DAW_INSERT_OWNER_TRACK), "Clip selection preserves target")
    let stable = revision(); controller.refreshInspectorSignalChain()
    expect(revision() == stable, "Reading inspector never changes project")
    let controlIdentity = ObjectIdentifier(chain.bypassButtons[0])
    controller.refreshInspectorSignalChain()
    expect(ObjectIdentifier(chain.bypassButtons[0]) == controlIdentity, "Unchanged snapshot retains control identity")

    var clip = ClipGeometry(start: 0, sourceOffset: 0, length: 48000, fadeIn: 0, fadeOut: 0)
    let wave = WaveformView(frame: NSRect(x: 0, y: 0, width: 200, height: 56)); wave.projectFrames = 96000
    let rect = wave.clipRect(clip)
    expect(rect.height == 48 && rect.minY == 4 && rect.maxX == 100, "Audio clips fill lane without changing time mapping")
    expect(AudioClipDrawing.fadePositions(clip, in: rect).0 == nil && AudioClipDrawing.fadePositions(clip, in: rect).1 == nil, "No invented fade ramps")
    clip.fadeIn = 12000; clip.fadeOut = 24000
    let fades = AudioClipDrawing.fadePositions(clip, in: rect)
    expect(fades.0 == 25 && fades.1 == 50, "Fade projection uses real frame counts")
    expect(AudioClipDrawing.color(for: clip, accent: .systemPurple) == .systemPurple, "Unset clip color uses track accent")
    clip.color = 0xAA5522
    expect(AudioClipDrawing.color(for: clip, accent: .systemPurple) == dawColorFromHex(0xAA5522), "Explicit clip color wins")
    controller.updateMixerInspector(trackID)
    print("PASS: \(assertions) inspector signal-chain and audio appearance assertions")
    return assertions
}
