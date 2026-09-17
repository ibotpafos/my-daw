import AppKit

extension DraftApp {
    func wireInspectorSignalChain() {
        inspectorBrowser.signalChain.onRackAction = { [weak self] context, action in
            guard let self, self.acceptsInspectorCommand(context) else { return }
            // The existing rack validates availability and dispatches the same
            // AU/VST3 parameter editor, bypass, reorder and removal commands.
            self.channelRack.perform(action)
        }
        inspectorBrowser.signalChain.onSendAction = { [weak self] context, action in
            self?.performInspectorSend(context, action)
        }
    }
    func acceptsInspectorCommand(_ context: InspectorChainContext) -> Bool {
        !isRecording && !midiTakeArmed && session != nil &&
            context.revision == revision && context.target == channelRack.target &&
            context == inspectorBrowser.signalChain.context
    }
    func refreshInspectorSignalChain() {
        guard let session, let target = channelRack.target else {
            inspectorBrowser.signalChain.update(context: nil, devices: [], sends: [], destinations: [], editable: false)
            return
        }
        var sends: [InspectorSend] = []
        var destinations: [InspectorSendDestination] = []
        if target.owner == Int32(DAW_INSERT_OWNER_TRACK) {
            var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
            if daw_get_snapshot(session, &snapshot) == 0 {
                for index in 0..<snapshot.bus_count {
                    var bus = daw_bus(); bus.struct_size = UInt32(MemoryLayout<daw_bus>.size)
                    guard daw_get_bus(session, index, &bus) == 0 else { continue }
                    let name = withUnsafeBytes(of: bus.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                    destinations.append(InspectorSendDestination(id: bus.id, name: name))
                }
                for index in 0..<snapshot.track_count {
                    var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
                    guard daw_get_track(session, index, &track) == 0, track.id == target.ownerID else { continue }
                    for sendIndex in 0..<track.send_count {
                        var send = daw_send(); send.struct_size = UInt32(MemoryLayout<daw_send>.size)
                        guard daw_get_send(session, track.id, sendIndex, &send) == 0 else { continue }
                        let name = destinations.first(where: { $0.id == send.bus_id })?.name ?? "Недоступная шина"
                        sends.append(InspectorSend(busID: send.bus_id, name: name, gainDb: send.gain_db, preFader: send.pre_fader != 0))
                    }
                    break
                }
            }
        }
        destinations.removeAll { destination in sends.contains(where: { $0.busID == destination.id }) }
        inspectorBrowser.signalChain.update(context: InspectorChainContext(target: target, revision: revision),
            devices: channelRack.devices, sends: sends, destinations: destinations, editable: !isRecording && !midiTakeArmed)
    }
    func performInspectorSend(_ context: InspectorChainContext, _ action: InspectorSendAction) {
        guard acceptsInspectorCommand(context), context.target.owner == Int32(DAW_INSERT_OWNER_TRACK) else { return }
        let chain = inspectorBrowser.signalChain
        if case let .add(busID) = action {
            guard chain.destinations.contains(where: { $0.id == busID }), !chain.sends.contains(where: { $0.busID == busID }) else { return }
            let sender = NSPopUpButton(); sender.addItem(withTitle: "Send")
            sender.lastItem?.representedObject = NSNumber(value: busID)
            let key = ObjectIdentifier(sender); newSendTargets[key] = context.target.ownerID
            defer { newSendTargets.removeValue(forKey: key) }
            addSend(sender)
            return
        }
        let busID: UInt64
        switch action {
        case let .gain(id, value): guard value.isFinite, (-120...24).contains(value) else { return }; busID = id
        case let .pre(id), let .remove(id): busID = id
        case .add: return
        }
        guard let send = chain.sends.first(where: { $0.busID == busID }) else { return }
        let sender: NSControl
        if case let .gain(_, value) = action { let slider = NSSlider(); slider.minValue = -120; slider.maxValue = 24; slider.doubleValue = value; sender = slider }
        else { sender = NSButton() }
        let key = ObjectIdentifier(sender)
        sendControlTargets[key] = (context.target.ownerID, busID, send.gainDb, send.preFader)
        defer { sendControlTargets.removeValue(forKey: key) }
        switch action {
        case .gain: if let slider = sender as? NSSlider { changeSendGain(slider) }
        case .pre: if let button = sender as? NSButton { toggleSendPre(button) }
        case .remove: if let button = sender as? NSButton { removeSend(button) }
        case .add: break
        }
    }
}
