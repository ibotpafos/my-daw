import AppKit

struct MixerSendTests {
    @MainActor static func run() throws {
        var lead = MixerStripModel(id: 1, kind: .track, title: "Lead Vocal", color: .systemOrange, volumeDb: -3, pan: -0.6, isSelected: true)
        lead.sends = [MixerSendSummary(destination: "Plate", gainDb: -18, busID: 10)]
        let other = MixerStripModel(id: 2, kind: .track, title: "Dry only")
        let bus = MixerStripModel(id: 10, kind: .bus, title: "Plate")
        let master = MixerStripModel(id: 0, kind: .master, title: "Master")
        var models = [lead,other,bus,master]
        let mixer = MixerWorkspaceView(frame:NSRect(x:0,y:0,width:1450,height:900))
        let window = NSWindow(contentRect:mixer.frame,styleMask:[.titled,.resizable],backing:.buffered,defer:false)
        window.contentView = mixer; mixer.strips = models
        mixer.layoutSubtreeIfNeeded(); mixer.setSendTarget(10); mixer.layoutSubtreeIfNeeded()
        let strip = mixer.stripViews[1]!
        let mode = strip.subviews.compactMap{$0 as? MixerActionButton}.first{$0.title == "LINK"}!
        let mute = strip.subviews.compactMap{$0 as? MixerActionButton}.first{$0.title == "SM"}!
        var commands: [String] = [], panEvents: [String] = []
        var mainPan = 0, mainMute = 0
        var mainMuteValues: [Bool] = []
        mixer.onMute = { _,value in mainMute += 1; mainMuteValues.append(value) }
        mixer.onPan = { _,_ in mainPan += 1 }
        mixer.onSend = { id,action in
            switch action {
            case .mute(let bus,let value): commands.append("mute:\(id):\(bus):\(value)")
            case .pan(let bus,let value,let independent): commands.append("pan:\(id):\(bus):\(value):\(independent)")
            default: break
            }
        }
        mixer.onSendPanBegin = { id,bus in panEvents.append("begin:\(id):\(bus)") }
        mixer.onSendPan = { id,bus,value in panEvents.append("value:\(id):\(bus):\(value)") }
        mixer.onSendPanEnd = { id,bus,_ in panEvents.append("end:\(id):\(bus)") }
        precondition(!strip.pan.isEnabled && strip.pan.value == -0.6)
        precondition(mode.isEnabled && !mode.isHidden && mute.isEnabled)
        precondition(!mixer.stripViews[2]!.pan.isEnabled)
        let dryMute = mixer.stripViews[2]!.subviews.compactMap{$0 as? MixerActionButton}.first{$0.title == "SM"}!
        precondition(!dryMute.isEnabled)
        mode.performClick(nil); mute.performClick(nil)
        precondition(commands == ["pan:1:10:0.0:true","mute:1:10:true"] && mainMute == 0)
        models[0].sends[0].independentPan = true; models[0].sends[0].pan = 0.5
        mixer.strips = models; mixer.layoutSubtreeIfNeeded()
        precondition(strip.pan.isEnabled && strip.pan.value == 0.5 && mode.title == "IND")
        strip.pan.commit(-0.25)
        precondition(panEvents == ["begin:1:10","value:1:10:-0.25","end:1:10"] && mainPan == 0)
        precondition(strip.model.pan == -0.6 && strip.model.volumeDb == -3)
        // Selected inspector mirrors the send preview, not the main channel balance.
        let inspector = mixer.subviews.compactMap{$0 as? MixerStripView}.first{$0.model.id == 1}!
        precondition(inspector.pan.value == -0.25)
        let before = panEvents.count
        strip.pan.commit(.nan)
        precondition(panEvents.count == before)
        mixer.editingEnabled = false
        strip.pan.commit(1); mute.performClick(nil); mode.performClick(nil)
        precondition(panEvents.count == before && commands.count == 2)
        mixer.editingEnabled = true
        models[0].sends[0].muted = true; mixer.strips = models
        precondition(mute.state == .on && strip.fader.valueDb == -18 && strip.pan.isEnabled)
        mute.performClick(nil)
        precondition(commands.last == "mute:1:10:false" && mainMute == 0)
        mode.performClick(nil)
        precondition(commands.last == "pan:1:10:0.5:false", "Mode toggle uses latest stored send balance")
        // Switching back restores main control semantics and does not itself emit edits.
        mixer.setSendTarget(nil); mixer.layoutSubtreeIfNeeded()
        precondition(strip.pan.value == -0.6 && mute.title == "M" && mode.isHidden && mute.state == .off)
        strip.pan.commit(0.1); mute.performClick(nil)
        precondition(mainPan == 1 && mainMute == 1)
        var soloValues: [Bool] = [], armValues: [Bool] = []
        mixer.onSolo = { _,value in soloValues.append(value) }
        mixer.onArm = { _,value in armValues.append(value) }
        models[0].isMuted = true; models[0].isSolo = true; models[0].isArmed = true
        mixer.strips = models; mixer.layoutSubtreeIfNeeded()
        mute.performClick(nil)
        let solo = strip.subviews.compactMap{$0 as? MixerActionButton}.first{$0.title == "S"}!
        let arm = strip.subviews.compactMap{$0 as? MixerActionButton}.first{$0.title == "●"}!
        solo.performClick(nil); arm.performClick(nil)
        precondition(mainMuteValues == [true,false] && soloValues == [false] && armValues == [false], "Retained controls must toggle the refreshed model, not initializer values")
        mixer.setSendTarget(10); mixer.layoutSubtreeIfNeeded()
        let currentPan = strip.pan.value
        precondition(strip.pan.accessibilityPerformIncrement())
        precondition(abs(strip.pan.value - (currentPan+0.02)) < 0.0001)
        // Native routing menus show muted routes and retain their level/connection.
        let matrix = MixerRoutingMatrixView(frame:NSRect(x:0,y:0,width:900,height:500))
        matrix.strips = models; matrix.setMode(.sends); matrix.layoutSubtreeIfNeeded()
        var matrixActions = 0
        matrix.onSend = { id,action in
            precondition(id == 1)
            switch action {
            case .mute(let bus,let muted): precondition(bus == 10 && !muted); matrixActions += 1
            case .pan(let bus,let pan,let independent): precondition(bus == 10 && pan == 0.5 && !independent); matrixActions += 1
            default: preconditionFailure("Unexpected matrix action")
            }
        }
        let cell = matrix.routingTable.view(atColumn:0,row:0,makeIfNecessary:true) as! MixerActionButton
        precondition((cell.accessibilityValue() as? String)?.contains("MUTED") == true && cell.title.contains("-18"))
        let menu = matrix.sendMenu(rowID:1,destinationID:10)!
        for text in ["Unmute send","Use channel pan / original tap"] {
            let item = menu.items.first{$0.title == text}!
            _ = NSApp.sendAction(item.action!,to:item.target,from:item)
        }
        precondition(matrixActions == 2)
        // Offscreen fixture shows the real send control layout.
        mixer.layoutSubtreeIfNeeded()
        guard let rep=mixer.bitmapImageRepForCachingDisplay(in:mixer.bounds) else { fatalError("Send bitmap unavailable") }
        mixer.cacheDisplay(in:mixer.bounds,to:rep)
        guard let png=rep.representation(using:.png,properties:[:]) else { fatalError("Send PNG encoding failed") }
        try png.write(to:URL(fileURLWithPath:"build/mixer-send-ui.png"))
        print("Send UI tests PASS: mute/mode dispatch, independent pan gestures, inspector mirroring, missing sends, disabled editing, main restoration, AX increment, matrix menu")
    }
}
