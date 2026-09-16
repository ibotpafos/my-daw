import AppKit

@main
struct MixerUITests {
    @MainActor
    static func main() throws {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
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
        var tracks = (1...12).map { index in
            MixerStripModel(id:UInt64(index),kind:.track,title:["Kick","Snare","Hi-hat","Percussion","Bass","Piano","Pad","Lead Vocal","Double L","Double R","Adlibs","Guitar"][index-1],color:index < 5 ? .systemTeal:index < 8 ? .systemPurple:.systemOrange,volumeDb:-Double(index)/2,outputName:index > 7 ? "Vocal Bus":"Master",sends:index > 7 ? [MixerSendSummary(destination:"Vocal Reverb",gainDb:-18,preFader:false,busID:100)]:[],isSelected:index==8,outputID:index > 7 ? 101:0)
        }
        tracks.append(MixerStripModel(id:100,kind:.bus,title:"Vocal Reverb",color:.systemPurple,volumeDb:-3))
        tracks.append(MixerStripModel(id:101,kind:.bus,title:"Vocal Bus",color:.systemOrange))
        tracks.append(MixerStripModel(id:0,kind:.master,title:"MASTER",color:.systemMint,volumeDb:0,outputName:"Output 1–2"))
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
        var mainChanges = 0, sendChanges = 0
        mixer.onVolume = { _,_ in mainChanges += 1 }
        mixer.onSendGain = { id,bus,value in
            precondition(id == 8 && bus == 100 && abs(value+6)<0.00001)
            sendChanges += 1
        }
        mixer.setSendTarget(100)
        precondition(!mixer.stripViews[1]!.fader.isEnabled)
        precondition(track.fader.isEnabled && abs(track.fader.valueDb+18)<0.00001)
        precondition(master.fader.isEnabled) // Return + master keep main controls.
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
        // No phantom sends after a destination disappears.
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
        // Offscreen rendering is evidence of actual AppKit layout, not a mockup.
        mixer.setFrameSize(NSSize(width:1450,height:900)); mixer.needsLayout=true; mixer.layoutSubtreeIfNeeded()
        var meters:[UInt64:MixerMeterSnapshot]=[:]
        for id in 1...12 { meters[UInt64(id)] = MixerMeterSnapshot(leftPeak:Float(id)/24,rightPeak:Float(id)/29,leftHold:Float(id)/22,rightHold:Float(id)/25) }
        meters[0]=MixerMeterSnapshot(leftPeak:0.74,rightPeak:0.7,leftHold:0.8,rightHold:0.79,momentaryLufs:-16.2,shortTermLufs:-15.4)
        mixer.updateMeters(meters)
        mixer.contentView.scroll(to:.zero)
        let url=URL(fileURLWithPath:"build/mixer-ui.png")
        if let rep=mixer.bitmapImageRepForCachingDisplay(in:mixer.bounds) {
            mixer.cacheDisplay(in:mixer.bounds,to:rep)
            if let data=rep.representation(using:.png,properties:[:]) { try data.write(to:url) }
        }
        // Large session presentation; this is not a 256-audio-track DSP claim.
        mixer.strips=(1...256).map { MixerStripModel(id:UInt64($0),kind:.track,title:"Track \($0)") } + [tracks.last!]
        mixer.needsLayout=true; mixer.layoutSubtreeIfNeeded()
        precondition(mixer.visibleIDs.count == 256 && mixer.stripViews.count == 257)
        print("Mixer AppKit tests PASS: scales, search, filters, identities, send mapping, disable, clip reset, resize, 256 strips")
    }
}
