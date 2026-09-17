import AppKit

@main
struct MixerUITests {
    @MainActor
    static func main() throws {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
        NSApp.appearance = NSAppearance(named: .darkAqua)
        for i in 0...1440 {
            let db = -120 + Double(i) / 10
            precondition(abs(MixerScale.decibels(MixerScale.position(db)) - db) < 0.000001)
            if i > 0 { precondition(MixerScale.position(db) > MixerScale.position(db-0.1)) }
        }
        precondition(MixerScale.parseDb("−6,5") == -6.5)
        precondition(MixerScale.parseDb("nan") == nil && MixerScale.parseDb("25") == nil)
        precondition(MixerScale.position(.infinity) == 0)
        precondition(MixerScale.meterPosition(0) == 0)
        precondition(MixerScale.meterPosition(1) > 0.9)

        let window = NSWindow(contentRect:NSRect(x:0,y:0,width:1450,height:930),styleMask:[.titled,.resizable],backing:.buffered,defer:false)
        let mixer = MixerWorkspaceView(frame:NSRect(x:0,y:0,width:1450,height:900))
        window.contentView = mixer
        let names = ["Kick","Snare","Hi-hat","Percussion","Bass","Piano","Pad","Lead Vocal","Double L","Double R","Adlibs","Guitar"]
        var tracks: [MixerStripModel] = []
        for index in 1...12 {
            var model = MixerStripModel(id: UInt64(index), kind: .track, title: names[index-1])
            model.color = index < 5 ? NSColor.systemTeal : (index < 8 ? NSColor.systemPurple : NSColor.systemOrange)
            model.volumeDb = -Double(index) / 2
            model.isSelected = index == 8
            model.outputName = index > 7 ? "Vocal Bus" : "Master"
            model.outputID = index > 7 ? 101 : 0
            model.hasMidi = (5...7).contains(index)
            model.inserts = [MixerInsertSummary(name: "AUParametricEQ", id: UInt64(1000+index), latencyFrames:index == 8 ? 144:0)]
            if index > 7 {
                model.sends = [MixerSendSummary(destination: "Vocal Reverb", gainDb: -18, preFader: false, busID: 100)]
                model.inserts.append(MixerInsertSummary(name: "AUDynamics", id: UInt64(2000+index), latencyFrames:index == 8 ? 48:0))
            }
            tracks.append(model)
        }
        precondition(tracks[7].totalInsertLatencyFrames == 192)
        precondition(abs(tracks[7].totalInsertLatencyMilliseconds - 4.0) < 0.0001)
        tracks.append(MixerStripModel(id:100,kind:.bus,title:"Vocal Reverb",color:.systemPurple,volumeDb:-3))
        tracks.append(MixerStripModel(id:101,kind:.bus,title:"Vocal Bus",color:.systemOrange))
        tracks.append(MixerStripModel(id:0,kind:.master,title:"MASTER",color:.systemMint,volumeDb:0,outputName:"Output 1–2"))

        // The matrix is a second presentation of the same stable routing/send IDs.
        let matrix=MixerRoutingMatrixView(frame:NSRect(x:0,y:0,width:900,height:520))
        matrix.strips=tracks
        precondition(matrix.rows.count == 14 && matrix.destinations.map(\.id) == [0,100,101])
        var matrixOutput:(UInt64,UInt64)?
        var matrixSends:[String]=[]
        matrix.onOutput={matrixOutput=($0,$1)}
        matrix.onSend={ id,action in
            switch action {
            case .mute(let bus,let muted): matrixSends.append("mute:\(id):\(bus):\(muted)")
            case .pan(let bus,let pan,let independent): matrixSends.append("pan:\(id):\(bus):\(pan):\(independent)")
            case .add(let bus): matrixSends.append("add:\(id):\(bus)")
            case .remove(let bus): matrixSends.append("remove:\(id):\(bus)")
            case .edit(let bus): matrixSends.append("edit:\(id):\(bus)")
            case .tap(let bus,let pre): matrixSends.append("tap:\(id):\(bus):\(pre)")
            }
        }
        matrix.activate(rowID:1,destinationID:101)
        precondition(matrixOutput?.0 == 1 && matrixOutput?.1 == 101)
        matrixOutput=nil;matrix.activate(rowID:100,destinationID:100)
        precondition(matrixOutput == nil,"Self-route must never be emitted by the matrix")
        matrix.setMode(.sends)
        precondition(matrix.rows.count == 12 && matrix.destinations.map(\.id) == [100,101])
        matrix.activate(rowID:8,destinationID:100)
        matrix.activate(rowID:1,destinationID:100)
        precondition(matrixSends == ["edit:8:100","add:1:100"])

        mixer.strips = tracks
        mixer.layoutSubtreeIfNeeded()
        precondition(mixer.stripViews.count == 15 && mixer.visibleIDs.count == 14)
        let track = mixer.stripViews[8]!
        let master = mixer.stripViews[0]!
        let masterX = master.frame.minX
        precondition(master.superview === mixer && master.frame.maxX <= mixer.bounds.maxX)
        mixer.contentView.scroll(to:NSPoint(x:300,y:0))
        precondition(master.frame.minX == masterX)

        mixer.filter.selectedSegment = 2
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(mixer.visibleIDs == [100,101])
        mixer.filter.selectedSegment = 0
        mixer.search.stringValue = "vocal bus"
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(mixer.visibleIDs.contains(8) && mixer.visibleIDs.contains(101))
        mixer.search.stringValue = ""
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()

        mixer.consoleState.pin(8,to:.left)
        mixer.consoleState.pin(100,to:.right)
        mixer.search.stringValue = "nothing matches"
        mixer.filter.selectedSegment = 1
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(mixer.visibleIDs == [8,100])
        precondition(mixer.stripViews[8]?.superview === mixer && mixer.stripViews[100]?.superview === mixer)
        precondition(mixer.consoleState.zone(of:8) == .left && mixer.consoleState.zone(of:100) == .right)
        mixer.consoleState.setHidden(true,id:8)
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(!mixer.visibleIDs.contains(8) && !mixer.consoleState.isPinned(8))
        mixer.consoleState.showAll();mixer.consoleState.clearPins();mixer.search.stringValue="";mixer.filter.selectedSegment=0
        mixer.needsLayout=true;mixer.layoutSubtreeIfNeeded()
        precondition(mixer.visibleIDs.count == 14 && mixer.stripViews[8]?.superview === mixer.documentView)

        var mainChanges = 0, sendChanges = 0
        mixer.onVolume = { _,_ in mainChanges += 1 }
        mixer.onSendGain = { id,bus,value in
            precondition(id == 8 && bus == 100 && abs(value+6)<0.00001)
            sendChanges += 1
        }
        var insertEdits: [UInt64] = [], sendEdits: [UInt64] = []
        mixer.onInsert = { id, action in
            precondition(id == 8)
            if case .edit(let pluginID) = action { insertEdits.append(pluginID) }
        }
        mixer.onSend = { id, action in
            precondition(id == 8)
            if case .edit(let busID) = action { sendEdits.append(busID) }
        }
        let buttons = track.subviews.compactMap { $0 as? MixerActionButton }
        let eqButton = buttons.first { $0.title.contains("AUParametricEQ") }!
        let sendButton = buttons.first { $0.title.contains("Vocal Reverb") }!

        mixer.setRackMode(.faders)
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(eqButton.isHidden && sendButton.isHidden)
        mixer.setRackMode(.inserts)
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(!eqButton.isHidden && sendButton.isHidden)
        mixer.setRackMode(.sends)
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(eqButton.isHidden && !sendButton.isHidden)
        mixer.setRackMode(.full)
        mixer.needsLayout = true; mixer.layoutSubtreeIfNeeded()
        precondition(!eqButton.isHidden && !sendButton.isHidden)

        eqButton.performClick(nil); sendButton.performClick(nil)
        precondition(insertEdits == [1008] && sendEdits == [100], "Stable insert/send action IDs")
        mixer.setSendTarget(100)
        precondition(!mixer.stripViews[1]!.fader.isEnabled)
        precondition(track.fader.isEnabled && abs(track.fader.valueDb+18)<0.00001)
        precondition(master.fader.isEnabled)
        track.fader.commit(-6)
        precondition(sendChanges == 1 && mainChanges == 0)
        mixer.setSendTarget(nil)
        precondition(track.fader.isEnabled && abs(track.fader.valueDb+4)<0.00001)
        mixer.editingEnabled = false
        track.fader.commit(-3)
        precondition(mainChanges == 0)
        mixer.editingEnabled = true
        track.fader.commit(-3)
        precondition(mainChanges == 1)
        mixer.updateMeters([8:MixerMeterSnapshot(leftPeak:1.2,rightPeak:0.5)])
        precondition(track.meter.clipped)
        mixer.updateMeters([8:MixerMeterSnapshot(leftPeak:0.2,rightPeak:0.1)])
        precondition(track.meter.clipped && mixer.stripViews[8] === track)
        mixer.resetPeaks(); precondition(!track.meter.clipped)
        mixer.strips = tracks
        precondition(mixer.stripViews[8] === track)
        mixer.setSendTarget(100)
        mixer.strips = tracks.filter { $0.id != 100 }
        precondition(mixer.sendTargetID == nil)
        mixer.strips = tracks

        for size in [NSSize(width:640,height:300),NSSize(width:1024,height:480),NSSize(width:1450,height:900)] {
            mixer.setFrameSize(size); mixer.needsLayout=true; mixer.layoutSubtreeIfNeeded()
            for view in mixer.stripViews.values where !view.isHidden {
                precondition(view.fader.frame.minY >= 0 && view.fader.frame.maxY <= view.bounds.height)
                precondition(view.gainField.frame.maxY <= view.bounds.height)
                precondition(view.fader.frame.maxY <= view.gainField.frame.minY)
            }
            precondition(master.frame.maxY <= mixer.bounds.height)
        }
        mixer.setFrameSize(NSSize(width:1450,height:900)); mixer.needsLayout=true; mixer.layoutSubtreeIfNeeded()
        var meters:[UInt64:MixerMeterSnapshot]=[:]
        for id in 1...12 { meters[UInt64(id)] = MixerMeterSnapshot(leftPeak:Float(id)/24,rightPeak:Float(id)/29,leftHold:Float(id)/22,rightHold:Float(id)/25) }
        meters[0]=MixerMeterSnapshot(leftPeak:0.74,rightPeak:0.7,leftHold:0.8,rightHold:0.79,momentaryLufs:-16.2,shortTermLufs:-15.4)
        mixer.updateMeters(meters)
        mixer.contentView.scroll(to:.zero)
        let url=URL(fileURLWithPath:"build/mixer-ui.png")
        guard let rep = mixer.bitmapImageRepForCachingDisplay(in: mixer.bounds) else { fatalError("Could not allocate real AppKit screenshot") }
        mixer.cacheDisplay(in: mixer.bounds, to: rep)
        guard let data = rep.representation(using: .png, properties: [:]) else { fatalError("AppKit PNG encoding failed") }
        try data.write(to: url)
        precondition(data.count > 10000, "Screenshot must contain actual rendered content")

        mixer.consoleState.showAll();mixer.consoleState.clearPins();mixer.search.stringValue="";mixer.filter.selectedSegment=0
        mixer.strips=(1...256).map { MixerStripModel(id:UInt64($0),kind:.track,title:"Track \($0)") } + [tracks.last!]
        mixer.setFrameSize(NSSize(width:1024,height:700));mixer.needsLayout=true;mixer.layoutSubtreeIfNeeded()
        precondition(mixer.visibleIDs.count == 256 && mixer.stripViews.count == 257)
        let composited=mixer.stripViews.values.filter{$0.model.kind != .master && !$0.isHidden}.count
        precondition(composited < 40, "Offscreen channels should be culled from AppKit compositing")
        mixer.contentView.scroll(to:NSPoint(x:12000,y:0));mixer.contentView.postsBoundsChangedNotifications=true
        NotificationCenter.default.post(name:NSView.boundsDidChangeNotification,object:mixer.contentView)
        precondition(mixer.stripViews[1]!.isHidden)
        try MixerRoutingTests.run()
        try MixerSendTests.run()
        print("Mixer AppKit tests PASS: scale, routing matrix, search, visibility, zones, section focus, latency, inserts/sends, send mapping, metering, resize, 256-strip virtualization")
    }
}
