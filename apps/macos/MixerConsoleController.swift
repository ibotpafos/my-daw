import AppKit

/// Thin adapter: the console uses the same C ABI, catalog and editors as the
/// existing project UI. It never invents a second routing or plug-in model.
@MainActor
extension DraftApp {
    func consoleAutomationLabel(_ kind:MixerStripKind,id:UInt64) -> String {
        let target = kind == .track ? automationTrackVolume : kind == .bus ? automationBusGain : automationMasterGain
        guard automationWrites(target:target,id:id) else { return "Read" }
        return automationMode == automationTouch ? "Touch · armed" : "Latch · armed"
    }
    private var consoleRoutingEditable: Bool {
        !isRecording && consoleGesture == nil && automationGesture == nil && pluginParameterGesture == nil
    }
    func configureMixerConsole() {
        mixerWorkspace.onVolumeGestureBegin = { [weak self] in self?.consoleBegin($0, send: nil) }
        mixerWorkspace.onVolume = { [weak self] _, value in self?.consoleWrite(value) }
        mixerWorkspace.onVolumeGestureEnd = { [weak self] _, _ in self?.consoleEnd() }
        mixerWorkspace.onSendGainBegin = { [weak self] id, bus in self?.consoleBegin(id, send: bus) }
        mixerWorkspace.onSendGain = { [weak self] _, _, value in self?.consoleWrite(value) }
        mixerWorkspace.onSendGainEnd = { [weak self] _, _, _ in self?.consoleEnd() }
        mixerWorkspace.onSendPanBegin = { [weak self] id,bus in self?.consoleBegin(id,send:bus,pan:true) }
        mixerWorkspace.onSendPan = { [weak self] _,_,value in self?.consoleWrite(value) }
        mixerWorkspace.onSendPanEnd = { [weak self] _,_,_ in self?.consoleEnd() }
        mixerWorkspace.onPan = { [weak self] id, value in
            guard let self, !isRecording else { return }
            mixerSetPan(id,value); refresh()
        }
        mixerWorkspace.onOutput = { [weak self] id, bus in self?.consoleRoute(id,to:bus) }
        mixerWorkspace.onInsert = { [weak self] id, action in self?.consoleInsert(id,action) }
        mixerWorkspace.onSend = { [weak self] id, action in self?.consoleSend(id,action) }
        mixerWorkspace.onCreateBus = { [weak self] in self?.addBus() }
        mixerWorkspace.onUndo = { [weak self] in self?.undo() }
        mixerWorkspace.onRedo = { [weak self] in self?.redo() }
        mixerWorkspace.historyState = { [weak self] in
            guard let self else { return (false,false) }
            var snapshot=daw_snapshot();snapshot.struct_size=UInt32(MemoryLayout<daw_snapshot>.size)
            guard daw_get_snapshot(session,&snapshot) == 0 else { return (false,false) }
            return (snapshot.can_undo != 0,snapshot.can_redo != 0)
        }
        mixerWorkspace.onClearSolo = { [weak self] in
            guard let self, !isRecording else { return }
            if check(daw_set_solo_exclusive(session,0,0,revision)) { refresh() }
        }
        mixerWorkspace.onRename = { [weak self] id,name in
            guard let self, !isRecording, let kind=mixerKinds[id],kind != .master else { return }
            let result = kind == .track ? daw_rename_track(session,id,name,revision) : daw_rename_bus(session,id,name,revision)
            if check(result) { refresh() }
        }
        mixerWorkspace.onResetPeaks = { [weak self] in self?.meterHolds.removeAll() }
        mixerWorkspace.onFocus = { [weak self] focused in
            guard let self else { return }
            arrangementInspectorSplit?.isHidden = focused
            arrangementConsoleSplit?.adjustSubviews()
            window.contentView?.layoutSubtreeIfNeeded()
        }
        installMixerConsoleMenu()
    }

    private func installMixerConsoleMenu() {
        guard let mainMenu=NSApp.mainMenu else{return}
        let root:NSMenuItem
        let menu:NSMenu
        if let existing=mainMenu.items.first(where:{$0.title=="Микшер"}),let existingMenu=existing.submenu {
            root=existing;menu=existingMenu;menu.removeAllItems()
        } else {
            root=NSMenuItem(title:"Микшер",action:nil,keyEquivalent:"")
            menu=NSMenu(title:"Микшер");root.submenu=menu;mainMenu.addItem(root)
        }
        func command(_ title:String,_ selector:Selector,_ key:String="",_ modifiers:NSEvent.ModifierFlags=[])->NSMenuItem {
            let item=NSMenuItem(title:title,action:selector,keyEquivalent:key);item.target=self;item.keyEquivalentModifierMask=modifiers;return item
        }
        menu.addItem(command("Матрица маршрутизации…",#selector(openMixerRoutingMatrix),"r",[.command,.option]))
        menu.addItem(.separator())
        menu.addItem(command("Канал полностью",#selector(mixerRackFull),"1",[.command,.option]))
        menu.addItem(command("Только Inserts",#selector(mixerRackInserts),"2",[.command,.option]))
        menu.addItem(command("Только Sends",#selector(mixerRackSends),"3",[.command,.option]))
        menu.addItem(command("Фейдеры",#selector(mixerRackFaders),"4",[.command,.option]))
        menu.addItem(.separator())
        menu.addItem(command("Показать все каналы",#selector(mixerShowAllChannels)))
    }

    @objc private func openMixerRoutingMatrix() {
        MixerRoutingPresenter.shared.present(
            strips:{ [weak self] in self?.mixerWorkspace.strips ?? [] },
            editingAllowed:{ [weak self] in self?.consoleRoutingEditable ?? false },
            onOutput:{ [weak self] id,bus in self?.consoleRoute(id,to:bus) },
            onSend:{ [weak self] id,action in self?.consoleSend(id,action) }
        )
    }
    @objc private func mixerRackFull(){mixerWorkspace.setRackMode(.full)}
    @objc private func mixerRackInserts(){mixerWorkspace.setRackMode(.inserts)}
    @objc private func mixerRackSends(){mixerWorkspace.setRackMode(.sends)}
    @objc private func mixerRackFaders(){mixerWorkspace.setRackMode(.faders)}
    @objc private func mixerShowAllChannels(){mixerWorkspace.consoleState.showAll();mixerWorkspace.needsLayout=true}

    func consoleBegin(_ id: UInt64, send: UInt64?, pan: Bool = false) {
        guard !isRecording, consoleGesture == nil, let kind = mixerKinds[id] else { return }
        if send == nil {
            let target: Int32 = kind == .track ? automationTrackVolume : kind == .bus ? automationBusGain : automationMasterGain
            if automationWrites(target:target,id:id) {
                mixerBeginVolume(id)
                if automationGesture != nil { consoleGesture = (true,revision) }
                return
            }
        }
        let target: Int32 = send != nil ? (pan ? Int32(DAW_MIXER_SEND_PAN) : Int32(DAW_MIXER_SEND_GAIN)) : kind == .track ? 1 : kind == .bus ? 3 : 4
        if check(daw_begin_mixer_gesture(session,target,id,send ?? 0,revision)) {
            consoleGesture = (false,revision)
        }
    }
    func consoleWrite(_ value: Double) {
        guard let gesture = consoleGesture, !isRecording else { return }
        if gesture.automation {
            if let target = automationGesture { _ = writeAutomation(target:target.target,id:target.id,value:value) }
        } else if !check(daw_write_mixer_gesture(session,value)) {
            daw_cancel_mixer_gesture(session); consoleGesture = nil
        }
    }
    func consoleEnd() {
        guard let gesture = consoleGesture else { refresh(); return }
        consoleGesture = nil
        if gesture.automation { endAutomationGesture() }
        else {
            if !check(daw_end_mixer_gesture(session,gesture.revision)) { daw_cancel_mixer_gesture(session) }
            refresh()
        }
    }
    private func consoleOwner(_ id: UInt64) -> Int32? {
        guard let kind = mixerKinds[id] else { return nil }
        return kind == .master ? Int32(DAW_INSERT_OWNER_MASTER) : kind == .bus ? Int32(DAW_INSERT_OWNER_BUS) : Int32(DAW_INSERT_OWNER_TRACK)
    }
    private func consoleRoute(_ id: UInt64, to bus: UInt64) {
        guard consoleRoutingEditable, let kind = mixerKinds[id], kind != .master else { return }
        // The existing routing commands validate all destinations and cycles.
        let result = kind == .track ? daw_set_track_output(session,id,bus,revision) : daw_set_bus_output(session,id,bus,revision)
        _ = check(result); refresh(); pollTransport();MixerRoutingPresenter.shared.reload()
    }
    private func consoleInsert(_ id: UInt64, _ action: MixerInsertAction) {
        guard !isRecording, let owner = consoleOwner(id) else { return }
        switch action {
        case .add:
            let title = mixerWorkspace.strips.first { $0.id == id }?.title ?? "Channel"
            addInsertOnChannel(owner:owner,ownerID:id,title:title)
        case .edit(let pluginID):
            var status = daw_insert_hosting_status(); status.struct_size = UInt32(MemoryLayout<daw_insert_hosting_status>.size)
            guard check(daw_get_insert_hosting_status(session,owner,id,pluginID,&status)) else { return }
            let remoteVST3 = status.selected_mode == UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS) && status.format == UInt32(DAW_INSERT_HOSTING_FORMAT_VST3)
            editInsertOnChannel(owner:owner,ownerID:id,id:pluginID,isolatedVST3:remoteVST3)
        case .bypass(let pluginID,let bypass):
            _ = check(daw_set_insert_bypass(session,owner,id,pluginID,bypass ? 1 : 0,revision)); refresh(); pollTransport()
        case .move(let pluginID,let index):
            guard index >= 0 else { return }
            _ = check(daw_move_insert(session,owner,id,pluginID,UInt32(index),revision)); refresh(); pollTransport()
        case .remove(let pluginID):
            _ = check(daw_remove_insert(session,owner,id,pluginID,revision)); refresh(); pollTransport()
        }
    }
    private func consoleSend(_ id: UInt64, _ action: MixerSendAction) {
        guard consoleRoutingEditable, mixerKinds[id] == .track else { return }
        let sends = mixerWorkspace.strips.first { $0.id == id }?.sends ?? []
        switch action {
        case .add(let bus):
            _ = check(daw_upsert_send(session,id,bus,-12,0,revision))
        case .remove(let bus):
            _ = check(daw_remove_send(session,id,bus,revision))
        case .mute(let bus,let muted):
            _ = check(daw_set_send_muted(session,id,bus,muted ? 1 : 0,revision))
        case .pan(let bus,let value,let independent):
            _ = check(daw_set_send_pan(session,id,bus,value,independent ? 1 : 0,revision))
        case .tap(let bus,let pre):
            guard let send = sends.first(where:{ $0.busID == bus }) else { return }
            _ = check(daw_upsert_send(session,id,bus,send.gainDb,pre ? 1 : 0,revision))
        case .edit(let bus):
            guard let send = sends.first(where:{ $0.busID == bus }) else { return }
            let field = NSTextField(string:MixerScale.label(send.gainDb))
            field.frame = NSRect(x:0,y:0,width:220,height:26)
            let alert = NSAlert(); alert.messageText = "Send → \(send.destination)"
            alert.informativeText = "Уровень в dB: −120…+24. Для непрерывного управления выбери Send → \(send.destination) над микшером."
            alert.accessoryView = field; alert.addButton(withTitle:"Применить"); alert.addButton(withTitle:"Отмена")
            alert.window.initialFirstResponder = field
            guard alert.runModal() == .alertFirstButtonReturn else { return }
            guard let value = MixerScale.parseDb(field.stringValue) else { storageMessage("Введи конечное значение от −120 до +24 dB."); return }
            _ = check(daw_upsert_send(session,id,bus,value,send.preFader ? 1 : 0,revision))
        }
        refresh(); pollTransport();MixerRoutingPresenter.shared.reload()
    }
}
