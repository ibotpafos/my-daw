import AppKit

@MainActor
final class MixerStripView: NSView, NSTextFieldDelegate {
    private(set) var model: MixerStripModel
    weak var workspace: MixerWorkspaceView?
    let fader = MixerFaderView(frame: .zero)
    let meter = MixerMeterView(frame: .zero)
    let gainField = NSTextField(string: "0.0")
    private let icon = NSImageView()
    private let title = MixerActionButton("")
    private let footer = MixerActionButton("")
    private let insertLabel = NSTextField(labelWithString: "INSERTS")
    private let sendsLabel = NSTextField(labelWithString: "SENDS")
    private let balanceLabel = NSTextField(labelWithString: "BALANCE")
    private let autoLabel = NSTextField(labelWithString: "Read")
    private let loudness = NSTextField(labelWithString: "")
    private let route = NSPopUpButton(frame: .zero, pullsDown: false)
    private let pan = NSSlider(value: 0, minValue: -1, maxValue: 1, target: nil, action: nil)
    private let mute = MixerActionButton("M"), solo = MixerActionButton("S"), arm = MixerActionButton("●")
    private let addInsert = MixerActionButton("+ Insert"), addSend = MixerActionButton("+ Send")
    private var insertButtons: [MixerActionButton] = [], sendButtons: [MixerActionButton] = []
    private var editingGain = false
    var sendTarget: UInt64?
    var rackMode: MixerConsoleState.RackMode = .full { didSet { if oldValue != rackMode { needsLayout = true } } }
    init(model: MixerStripModel) {
        self.model = model
        super.init(frame: .zero)
        wantsLayer = true
        for view in [icon,title,footer,insertLabel,sendsLabel,balanceLabel,autoLabel,loudness,route,pan,mute,solo,arm,addInsert,addSend,gainField,fader,meter] { addSubview(view) }
        for label in [insertLabel,sendsLabel,balanceLabel,autoLabel,loudness] {
            label.font = .monospacedSystemFont(ofSize: 9, weight: .medium)
            label.textColor = DAWDesignTokens.Color.secondaryText
            label.lineBreakMode = .byTruncatingTail
        }
        autoLabel.alignment = .center; loudness.alignment = .center
        gainField.font = .monospacedDigitSystemFont(ofSize: 12, weight: .medium)
        gainField.alignment = .center; gainField.isBordered = false; gainField.drawsBackground = false
        gainField.delegate = self; gainField.target = self; gainField.action = #selector(commitGain)
        gainField.toolTip = "Enter an exact level in dB (−120…+24). Decimal point or comma."
        route.controlSize = .small; route.font = .systemFont(ofSize: 10)
        route.target = self; route.action = #selector(routeChanged)
        pan.controlSize = .small; pan.isContinuous = false; pan.target = self; pan.action = #selector(panChanged)
        pan.toolTip = "Stereo balance. Center preserves both channels at unity."
        title.isBordered = false; title.alignment = .center
        footer.isBordered = false; footer.alignment = .center
        title.invoke = { [weak self] in self?.select() }; footer.invoke = { [weak self] in self?.select() }
        title.contextMenu = { [weak self] in self?.channelMenu() ?? NSMenu() }
        footer.contextMenu = title.contextMenu
        mute.setButtonType(.toggle); solo.setButtonType(.toggle); arm.setButtonType(.toggle)
        mute.invoke = { [weak self] in guard let self else { return }; workspace?.onMute?(model.id, !model.isMuted) }
        solo.invoke = { [weak self] in guard let self else { return }; workspace?.onSolo?(model.id, !model.isSolo) }
        arm.invoke = { [weak self] in guard let self else { return }; workspace?.onArm?(model.id, !model.isArmed) }
        addInsert.invoke = { [weak self] in guard let self else { return }; workspace?.onInsert?(model.id, .add) }
        addSend.invoke = { [weak self] in self?.showSendMenu() }
        fader.onBegin = { [weak self] in self?.beginGain() }
        fader.onChange = { [weak self] in self?.changeGain($0) }
        fader.onEnd = { [weak self] in self?.endGain($0) }
        apply(model)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override var isFlipped: Bool { true }
    private func select() { workspace?.onSelect?(model.id) }
    override func mouseDown(with event: NSEvent) { select() }
    func apply(_ next: MixerStripModel) {
        let old = model; model = next
        let tint = model.color ?? DAWDesignTokens.Color.accent
        title.title = model.title; footer.title = model.title
        title.toolTip = model.title; footer.toolTip = model.title
        icon.image = NSImage(systemSymbolName: model.kind == .master ? "waveform.path" : model.kind == .bus ? "arrow.triangle.branch" : model.hasMidi ? "pianokeys" : "waveform", accessibilityDescription: nil)
        icon.contentTintColor = tint
        footer.wantsLayer = true; footer.layer?.backgroundColor = tint.withAlphaComponent(0.22).cgColor
        mute.state = model.isMuted ? .on : .off; solo.state = model.isSolo ? .on : .off; arm.state = model.isArmed ? .on : .off
        mute.contentTintColor = model.isMuted ? .systemOrange : .labelColor
        solo.contentTintColor = model.isSolo ? .systemYellow : .labelColor
        arm.contentTintColor = model.isArmed ? .systemRed : .secondaryLabelColor
        mute.isHidden = model.kind == .master; solo.isHidden = model.kind != .track; arm.isHidden = model.kind != .track
        pan.isHidden = model.kind == .master; balanceLabel.isHidden = model.kind == .master
        pan.doubleValue = model.pan
        balanceLabel.stringValue = model.pan == 0 ? "BAL · CENTER" : String(format: "BAL · %@ %.0f", model.pan < 0 ? "L" : "R", abs(model.pan) * 100)
        autoLabel.stringValue = model.automationLabel
        autoLabel.toolTip = "Project automation mode. Arm its target in the automation toolbar to write."
        if old.inserts != model.inserts || insertButtons.isEmpty { rebuildInserts() }
        if old.sends != model.sends || sendButtons.isEmpty { rebuildSends() }
        updateDestinations(); updateFaderMode(); updateMeter(model.meter)
        setAccessibilityLabel("\(model.title), \(model.kind) channel")
        fader.setAccessibilityLabel("\(model.title) level")
        gainField.setAccessibilityLabel("\(model.title) exact dB")
        meter.setAccessibilityLabel("\(model.title) stereo sample peak")
        pan.setAccessibilityLabel("\(model.title) stereo balance")
        for (button, name) in [(mute,"Mute"),(solo,"Solo"),(arm,"Record arm")] { button.setAccessibilityLabel("\(name) \(model.title)") }
        route.setAccessibilityLabel("Output of \(model.title)")
        layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        layer?.borderWidth = model.isSelected ? 1.5 : 0.5
        layer?.borderColor = (model.isSelected ? tint : DAWDesignTokens.Color.border.withAlphaComponent(0.6)).cgColor
        needsLayout = true; needsDisplay = true
    }
    func updateMeter(_ snapshot: MixerMeterSnapshot) {
        meter.snapshot = snapshot // Deliberately not model.didSet / whole-strip refresh.
        if model.kind == .master, let m = snapshot.momentaryLufs, let s = snapshot.shortTermLufs {
            loudness.stringValue = m < -99 ? "LUFS · silence" : String(format: "M %.1f  S %.1f", m, s)
            loudness.toolTip = "BS.1770 momentary / short-term LUFS. Not integrated loudness."
        } else { loudness.stringValue = "SAMPLE PEAK" }
    }
    func updateFaderMode() {
        sendTarget = model.kind == .track ? workspace?.sendTargetID : nil
        let selectedSend = model.sends.first { $0.busID == sendTarget }
        let hasControl = sendTarget == nil || selectedSend != nil
        fader.isEnabled = hasControl && workspace?.editingEnabled != false
        gainField.isEnabled = fader.isEnabled
        pan.isEnabled = sendTarget == nil && model.kind != .master && workspace?.editingEnabled != false
        mute.isEnabled = workspace?.editingEnabled != false
        solo.isEnabled = workspace?.editingEnabled != false
        arm.isEnabled = workspace?.editingEnabled != false
        addInsert.isEnabled = workspace?.editingEnabled != false && model.inserts.count < 4
        addSend.isEnabled = workspace?.editingEnabled != false && model.sends.count < 8
        if !editingGain {
            let db = selectedSend?.gainDb ?? model.volumeDb
            fader.valueDb = db
            gainField.stringValue = hasControl ? MixerScale.label(db) : "No send"
        }
        balanceLabel.stringValue = sendTarget == nil ? (model.pan == 0 ? "BAL · CENTER" : String(format:"BAL · %@ %.0f",model.pan < 0 ? "L" : "R",abs(model.pan)*100)) : "MAIN BALANCE"
        fader.toolTip = sendTarget == nil ? "Main level. Double-click: 0 dB. Option: fine control." : (selectedSend.map { "Send to \($0.destination) · \($0.preFader ? "PRE" : "POST"). Main level unchanged." } ?? "This channel has no send to the selected destination.")
        gainField.textColor = sendTarget == nil ? DAWDesignTokens.Color.text : .systemMint
    }
    private func beginGain() {
        guard fader.isEnabled else { return }
        if let bus = sendTarget { workspace?.onSendGainBegin?(model.id,bus) }
        else { workspace?.onVolumeGestureBegin?(model.id) }
    }
    private func changeGain(_ value: Double) {
        gainField.stringValue = MixerScale.label(value)
        workspace?.previewLevel(model.id,send:sendTarget,value:value,source:self)
        if let bus = sendTarget { workspace?.onSendGain?(model.id,bus,value) }
        else { workspace?.onVolume?(model.id,value) }
    }
    private func endGain(_ value: Double) {
        if let bus = sendTarget { workspace?.onSendGainEnd?(model.id,bus,value) }
        else { workspace?.onVolumeGestureEnd?(model.id,value) }
    }
    func controlTextDidBeginEditing(_ obj: Notification) { editingGain = true }
    func controlTextDidEndEditing(_ obj: Notification) { editingGain = false; commitGain() }
    @objc private func commitGain() {
        guard !editingGain else { return } // End-editing owns a single commit, including Return.
        guard let value = MixerScale.parseDb(gainField.stringValue), fader.isEnabled else { updateFaderMode(); return }
        if abs(value - fader.valueDb) > 0.00001 { fader.commit(value) }
        updateFaderMode()
    }
    @objc private func routeChanged() {
        guard let id = (route.selectedItem?.representedObject as? NSNumber)?.uint64Value else { return }
        workspace?.onOutput?(model.id,id)
    }
    @objc private func panChanged() { workspace?.onPan?(model.id,pan.doubleValue) }
    func updateDestinations() {
        let items = workspace?.strips.filter { $0.kind == .bus && $0.id != model.id } ?? []
        route.removeAllItems(); route.addItem(withTitle: model.kind == .master ? model.outputName : "Master")
        route.lastItem?.representedObject = NSNumber(value: UInt64(0))
        if model.kind != .master {
            for bus in items { route.addItem(withTitle: bus.title); route.lastItem?.representedObject = NSNumber(value: bus.id) }
        }
        route.select(route.itemArray.first { ($0.representedObject as? NSNumber)?.uint64Value == model.outputID })
        route.isEnabled = model.kind != .master && workspace?.editingEnabled != false
        addSend.isEnabled = model.kind == .track && !items.isEmpty && model.sends.count < 8 && workspace?.editingEnabled != false
        addSend.isHidden = model.kind != .track
        addInsert.isEnabled = model.inserts.count < 4 && workspace?.editingEnabled != false
    }
    private func rebuildInserts() {
        insertButtons.forEach { $0.removeFromSuperview() }; insertButtons.removeAll()
        for (index, insert) in model.inserts.enumerated() {
            let button = MixerActionButton(insert.name)
            button.alignment = .left
            button.contentTintColor = !insert.available ? .systemOrange : insert.bypassed ? .tertiaryLabelColor : .labelColor
            button.title = "\(!insert.available ? "!" : insert.bypassed ? "○" : "•") \(insert.name)"
            button.toolTip = "\(insert.name)\(insert.available ? "" : " · unavailable") · \(insert.latencyFrames) samples latency. Click: parameters. Right-click: bypass / reorder / remove."
            button.invoke = { [weak self] in guard let self else { return }; workspace?.onInsert?(model.id,.edit(insert.id)) }
            button.contextMenu = { [weak self] in
                guard let self else { return NSMenu() }
                let menu = NSMenu(); menu.autoenablesItems = false
                menu.addItem(MixerMenuItem("Parameters…", enabled: insert.available) { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.edit(insert.id)) })
                menu.addItem(MixerMenuItem(insert.bypassed ? "Enable" : "Bypass") { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.bypass(insert.id,!insert.bypassed)) })
                menu.addItem(MixerMenuItem("Move up", enabled: index > 0) { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.move(insert.id,index-1)) })
                menu.addItem(MixerMenuItem("Move down", enabled: index+1 < model.inserts.count) { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.move(insert.id,index+1)) })
                menu.addItem(.separator())
                menu.addItem(MixerMenuItem("Remove insert") { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.remove(insert.id)) })
                return menu
            }
            addSubview(button); insertButtons.append(button)
        }
    }
    private func rebuildSends() {
        sendButtons.forEach { $0.removeFromSuperview() }; sendButtons.removeAll()
        for send in model.sends {
            let button = MixerActionButton("\(send.destination)  \(MixerScale.label(send.gainDb))")
            button.alignment = .left
            button.toolTip = "\(send.destination) · \(send.preFader ? "PRE" : "POST") · \(MixerScale.label(send.gainDb)) dB. Click: exact level. Right-click: tap / remove."
            button.invoke = { [weak self] in guard let self else {return}; workspace?.onSend?(model.id,.edit(send.busID)) }
            button.contextMenu = { [weak self] in
                guard let self else { return NSMenu() }
                let menu = NSMenu(); menu.autoenablesItems = false
                menu.addItem(MixerMenuItem("Use faders for \(send.destination)") { [weak self] in self?.workspace?.setSendTarget(send.busID) })
                menu.addItem(MixerMenuItem(send.preFader ? "Switch to POST" : "Switch to PRE") { [weak self] in guard let self else {return}; workspace?.onSend?(model.id,.tap(send.busID,!send.preFader)) })
                menu.addItem(MixerMenuItem("Remove send") { [weak self] in guard let self else {return}; workspace?.onSend?(model.id,.remove(send.busID)) })
                return menu
            }
            addSubview(button); sendButtons.append(button)
        }
    }
    private func showSendMenu() {
        let menu = NSMenu(); menu.autoenablesItems = false
        for bus in workspace?.strips.filter({ $0.kind == .bus }) ?? [] {
            menu.addItem(MixerMenuItem(bus.title, enabled: !model.sends.contains { $0.busID == bus.id }) { [weak self] in guard let self else { return }; workspace?.onSend?(model.id,.add(bus.id)) })
        }
        menu.popUp(positioning: nil, at: NSPoint(x: 0,y: addSend.bounds.maxY), in: addSend)
    }
    private func channelMenu() -> NSMenu {
        let menu = NSMenu(); menu.autoenablesItems = false
        menu.addItem(MixerMenuItem("Select \(model.title)") { [weak self] in self?.select() })
        if model.kind != .master {
            menu.addItem(MixerMenuItem("Rename…",enabled:workspace?.editingEnabled != false) { [weak self] in
                guard let self else { return }
                let input=NSTextField(string:model.title); input.frame=NSRect(x:0,y:0,width:260,height:26)
                let alert=NSAlert(); alert.messageText="Rename channel"; alert.accessoryView=input
                alert.addButton(withTitle:"Rename"); alert.addButton(withTitle:"Cancel")
                alert.window.initialFirstResponder=input
                if alert.runModal() == .alertFirstButtonReturn { workspace?.onRename?(model.id,input.stringValue) }
            })
        }
        menu.addItem(MixerMenuItem("Clear all solo",enabled:workspace?.editingEnabled != false) { [weak self] in self?.workspace?.onClearSolo?() })
        let insertMenu = NSMenu(); insertMenu.autoenablesItems = false
        for insert in model.inserts {
            insertMenu.addItem(MixerMenuItem(insert.name, enabled: insert.available) { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.edit(insert.id)) })
        }
        insertMenu.addItem(MixerMenuItem("Add insert…", enabled:model.inserts.count < 4) { [weak self] in guard let self else {return}; workspace?.onInsert?(model.id,.add) })
        let insertsItem = NSMenuItem(title:"Inserts",action:nil,keyEquivalent:""); insertsItem.submenu = insertMenu; menu.addItem(insertsItem)
        let sendMenu = NSMenu(); sendMenu.autoenablesItems = false
        for send in model.sends {
            sendMenu.addItem(MixerMenuItem("\(send.destination) · \(MixerScale.label(send.gainDb)) dB") { [weak self] in guard let self else {return}; workspace?.onSend?(model.id,.edit(send.busID)) })
        }
        if model.kind == .track {
            sendMenu.addItem(MixerMenuItem("Add send…", enabled: model.sends.count < 8) { [weak self] in self?.showSendMenu() })
            let sendsItem = NSMenuItem(title:"Sends",action:nil,keyEquivalent:""); sendsItem.submenu = sendMenu; menu.addItem(sendsItem)
        }
        menu.addItem(MixerMenuItem("Reset level to 0 dB", enabled: fader.isEnabled) { [weak self] in self?.fader.commit(0) })
        if model.kind == .bus {
            menu.addItem(.separator())
            menu.addItem(MixerMenuItem("Delete bus…") { [weak self] in guard let self else { return }; workspace?.onDeleteBus?(model.id) })
        }
        return menu
    }
    override func layout() {
        super.layout()
        let w = bounds.width, h = bounds.height, inset: CGFloat = 7
        let detailed = h >= 600, tall = h >= 780
        let showInserts = detailed && (rackMode == .full || rackMode == .inserts)
        let showSends = detailed && model.kind == .track && (rackMode == .full || rackMode == .sends)
        icon.isHidden = !detailed
        icon.frame = NSRect(x: w/2-14,y: 14,width: 28,height: 28)
        title.frame = NSRect(x: 3,y: detailed ? 48 : 6,width: w-6,height: 24)
        var y: CGFloat = detailed ? 82 : 35

        insertLabel.isHidden = !showInserts; addInsert.isHidden = !showInserts
        let visibleInsertCount = showInserts ? (tall ? 4 : 2) : 0
        for (i, button) in insertButtons.enumerated() { button.isHidden = i >= visibleInsertCount }
        if showInserts {
            insertLabel.frame = NSRect(x: inset,y:y,width:w-14,height:12); y += 17
            for (i, button) in insertButtons.enumerated() where i < visibleInsertCount { button.frame = NSRect(x:inset,y:y+CGFloat(i)*25,width:w-14,height:23) }
            y += CGFloat(visibleInsertCount)*25
            addInsert.frame = NSRect(x:inset,y:y,width:w-14,height:23); y += 34
        }

        sendsLabel.isHidden = !showSends; addSend.isHidden = !showSends
        let visibleSends = showSends ? (tall ? 3 : 1) : 0
        for (i, button) in sendButtons.enumerated() { button.isHidden = i >= visibleSends }
        if showSends {
            sendsLabel.frame = NSRect(x:inset,y:y,width:w-14,height:12); y += 17
            for (i, button) in sendButtons.enumerated() where i < visibleSends { button.frame = NSRect(x:inset,y:y+CGFloat(i)*25,width:w-14,height:23) }
            y += CGFloat(visibleSends)*25
            addSend.title = model.sends.count > visibleSends ? "+ Send · \(model.sends.count)" : "+ Send"
            addSend.frame = NSRect(x:inset,y:y,width:w-14,height:23); y += 33
        }

        // Compact workspace keeps processing accessible without forcing tall racks.
        if !detailed && h >= 350 && (rackMode == .full || rackMode == .inserts) {
            addInsert.isHidden = false; addInsert.frame = NSRect(x:inset,y:y,width:w-14,height:22); y += 28
        }
        route.frame = NSRect(x:inset,y:y,width:w-14,height:24); y += 30
        let showPan = h >= 300 && model.kind != .master
        balanceLabel.isHidden = !showPan; pan.isHidden = !showPan
        balanceLabel.frame = NSRect(x:inset,y:y,width:w-14,height:12)
        pan.frame = NSRect(x:inset,y:y+15,width:w-14,height:18)
        if showPan { y += 42 }
        else if model.kind == .master && h >= 400 { y += 42 }
        let meterBottom = h - 112, meterHeight = max(24,meterBottom-y)
        fader.frame = NSRect(x:w*0.35-14,y:y,width:28,height:meterHeight)
        meter.frame = NSRect(x:w*0.67,y:y,width:max(14,w*0.18),height:meterHeight)
        gainField.frame = NSRect(x:inset,y:meterBottom+7,width:w-14,height:22)
        let bw = min(28,(w-18)/3)
        mute.frame = NSRect(x:inset,y:h-76,width:bw,height:25)
        solo.frame = NSRect(x:inset+bw+2,y:h-76,width:bw,height:25)
        arm.frame = NSRect(x:inset+2*(bw+2),y:h-76,width:bw,height:25)
        autoLabel.frame = NSRect(x:inset,y:h-48,width:w-14,height:13)
        loudness.isHidden = model.kind != .master || h < 400
        loudness.frame = NSRect(x:inset,y:y-39,width:w-14,height:30)
        footer.frame = NSRect(x:1,y:h-30,width:w-2,height:29)
    }
    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let tint = model.color ?? DAWDesignTokens.Color.accent
        tint.withAlphaComponent(model.isSelected ? 0.25 : 0.12).setFill()
        NSRect(x:1,y:1,width:bounds.width-2,height:bounds.height >= 600 ? 73 : 31).fill()
        // The meter and fader have different scales. Explicitly label each.
        let attrs: [NSAttributedString.Key:Any] = [.font:NSFont.monospacedDigitSystemFont(ofSize:8,weight:.regular),.foregroundColor:DAWDesignTokens.Color.secondaryText]
        for db in [-60.0,-18,-6,0,12] where fader.frame.height > 100 {
            let position = CGFloat(MixerScale.position(db))
            let y: CGFloat = fader.frame.maxY - 8 - (fader.frame.height - 16) * position
            (String(format:"%g",db) as NSString).draw(at:NSPoint(x:4,y:y-4),withAttributes:attrs)
        }
    }
}
