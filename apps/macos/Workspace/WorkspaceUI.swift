import AppKit

/// Composition and routing only. Audio, import, plugin hosting and Undo remain
/// in the existing session/C ABI command paths.
extension DraftApp {
    func makeWorkspaceHeader() -> NSView {
        let surface = WorkspaceSurface()
        workspaceMode.font = .systemFont(ofSize: 11, weight: .semibold)
        workspaceMode.controlSize = .regular
        workspaceMode.segmentStyle = .rounded
        workspaceMode.heightAnchor.constraint(equalToConstant: 28).isActive = true
        workspaceMode.widthAnchor.constraint(equalToConstant: 208).isActive = true
        projectTitleLabel.font = .systemFont(ofSize: 13, weight: .semibold)
        projectTitleLabel.textColor = DAWDesignTokens.Color.text
        projectStateLabel.font = .systemFont(ofSize: 10)
        projectStateLabel.textColor = DAWDesignTokens.Color.secondaryText
        for field in [projectTitleLabel, projectStateLabel] {
            field.lineBreakMode = .byTruncatingTail
            field.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        }
        let project = NSStackView(views: [projectTitleLabel, projectStateLabel])
        project.orientation = .vertical; project.alignment = .leading; project.spacing = 4
        project.widthAnchor.constraint(greaterThanOrEqualToConstant: 100).isActive = true
        project.widthAnchor.constraint(lessThanOrEqualToConstant: 180).isActive = true
        let transport = NSStackView(views: [iconButton(.rewind, #selector(rewindAudio)), stopButton, playButton, recordButton, loopButton])
        transport.spacing = 5
        for case let control as NSButton in transport.arrangedSubviews {
            WorkspaceControlStyle.icon(control, height: 34)
        }
        playButton.contentTintColor = DAWDesignTokens.Color.mint
        recordButton.contentTintColor = DAWDesignTokens.Color.coral
        clockLabel.font = .monospacedDigitSystemFont(ofSize: 22, weight: .regular)
        clockLabel.textColor = DAWDesignTokens.Color.text
        clockLabel.alignment = .center
        clockLabel.setAccessibilityLabel("Позиция воспроизведения в минутах, секундах и миллисекундах")
        positionLabel.font = .monospacedDigitSystemFont(ofSize: 10, weight: .medium)
        signatureLabel.font = .monospacedDigitSystemFont(ofSize: 10, weight: .medium)
        let tempoRow = NSStackView(views: [tempoField, tempoStepper, signatureLabel, positionLabel]); tempoRow.spacing = 6
        tempoStepper.controlSize = .mini
        let display = NSStackView(views: [clockLabel, tempoRow]); display.orientation = .vertical; display.spacing = 3
        display.widthAnchor.constraint(equalToConstant: 206).isActive = true
        display.edgeInsets = NSEdgeInsets(top: 5, left: 10, bottom: 5, right: 10)
        display.wantsLayer = true
        display.layer?.backgroundColor = DAWDesignTokens.Color.canvas.cgColor
        display.layer?.cornerRadius = 6
        display.layer?.borderWidth = 1
        display.layer?.borderColor = DAWDesignTokens.Color.border.cgColor
        let paneSpecs: [(WorkspaceLayout.Pane, String, String, Selector)] = [
            (.library, "sidebar.left", "Библиотека · ⇧⌘1", #selector(toggleWorkspaceLibrary)),
            (.inspector, "sidebar.right", "Инспектор · ⇧⌘2", #selector(toggleWorkspaceInspector)),
            (.dock, "rectangle.bottomthird.inset.filled", "Нижняя панель · ⇧⌘3", #selector(toggleWorkspaceDock))
        ]
        var toggles: [NSView] = []
        for (pane, symbol, title, action) in paneSpecs {
            let control = button("", action)
            control.image = NSImage(systemSymbolName: symbol, accessibilityDescription: title)
            control.imagePosition = .imageOnly; control.setButtonType(.toggle)
            control.toolTip = title; control.setAccessibilityLabel(title)
            control.widthAnchor.constraint(equalToConstant: 30).isActive = true
            WorkspaceControlStyle.icon(control)
            workspaceToggleButtons[pane] = control; toggles.append(control)
        }
        let paneButtons = NSStackView(views: toggles); paneButtons.spacing = 3
        let stack = NSStackView(views: [workspaceMode, project, flexibleSpace(), transport, display, flexibleSpace(), paneButtons])
        stack.spacing = 10; stack.translatesAutoresizingMaskIntoConstraints = false; surface.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: surface.leadingAnchor, constant: 12),
                                     stack.trailingAnchor.constraint(equalTo: surface.trailingAnchor, constant: -12),
                                     stack.centerYAnchor.constraint(equalTo: surface.centerYAnchor)])
        return surface
    }

    /// Rebuilding track projections must not jump back to the beginning of the
    /// arrangement. Let NSClipView clamp when deletion shortens the document.
    func restoreArrangementViewport(_ origin: NSPoint?) {
        guard let origin, let scroll = timelineScroll, scroll.documentView != nil else { return }
        window.contentView?.layoutSubtreeIfNeeded()
        let clip = scroll.contentView
        let requested = NSRect(origin: origin, size: clip.bounds.size)
        clip.scroll(to: clip.constrainBoundsRect(requested).origin)
        scroll.reflectScrolledClipView(clip)
        if let headers = trackHeaderScroll {
            var headerBounds = headers.contentView.bounds
            headerBounds.origin.y = clip.bounds.origin.y
            headers.contentView.scroll(to: headers.contentView.constrainBoundsRect(headerBounds).origin)
            headers.reflectScrolledClipView(headers.contentView)
        }
    }

    func makeWorkspaceStatusBar() -> NSView {
        let surface = WorkspaceSurface()
        for field in [status, transportLabel, gridLabel] {
            field.font = .monospacedDigitSystemFont(ofSize: 10, weight: .regular)
            field.lineBreakMode = .byTruncatingTail
            field.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        }
        status.widthAnchor.constraint(greaterThanOrEqualToConstant: 120).isActive = true
        transportLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 340).isActive = true
        let stack = NSStackView(views: [status, flexibleSpace(), transportLabel, gridLabel, metronomeButton, recordMonitorButton, autoMonitorButton])
        stack.spacing = 10; stack.translatesAutoresizingMaskIntoConstraints = false; surface.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: surface.leadingAnchor, constant: 10),
                                     stack.trailingAnchor.constraint(equalTo: surface.trailingAnchor, constant: -10),
                                     stack.centerYAnchor.constraint(equalTo: surface.centerYAnchor)])
        return surface
    }

    func wireWorkspace() {
        wireTimelineNavigation()
        workspace?.onChange = { [weak self] preference in
            guard let self else { return }
            self.workspaceDock?.select(preference.dockTab)
            self.window.contentView?.layoutSubtreeIfNeeded()
            self.fitTrackHeaderWidth(); self.fitTimelineViewport(); self.updateWorkspaceChrome()
        }
        workspaceDock?.onSelect = { [weak self] tab in self?.workspace?.selectDock(tab) }
        workspaceDock?.onClose = { [weak self] in self?.workspace?.toggle(.dock) }
        inspectorBrowser.onShowDevices = { [weak self] in self?.workspace?.selectDock(.devices) }
        channelRack.onAction = { [weak self] target, action in self?.performRackAction(target, action) }
        workspaceDock?.select(workspace?.preference.dockTab ?? .devices)
    }

    @objc func toggleWorkspaceLibrary() { workspace?.toggle(.library) }
    @objc func toggleWorkspaceInspector() { workspace?.toggle(.inspector) }
    @objc func toggleWorkspaceDock() { workspace?.toggle(.dock) }
    @objc func resetWorkspaceLayout() { workspace?.resetLayout(); setTimelineZoom(1) }
    func restoreWorkspaceLayout() {
        restoringWorkspaceLayout = true
        window.contentView?.layoutSubtreeIfNeeded(); workspace?.applyGeometry()
        fitTrackHeaderWidth(); fitTimelineViewport()
        workspaceLayoutReady = true; restoringWorkspaceLayout = false
        updateWorkspaceChrome()
    }
    func applyWorkspaceMode(_ mode: Int) {
        // A workspace preset changes presentation only, never starts recording,
        // rewrites project data or rebuilds the audio graph.
        workspace?.selectDock(mode >= 2 ? .mixer : .devices)
        if mode == 1 || mode == 3 { workspace?.show(.inspector) }
        if mode == 3 { selectedMixerID = 0; updateMixerInspector(0) }
    }
    func windowDidResize(_ notification: Notification) {
        guard workspaceLayoutReady else { return }
        workspace?.applyGeometry(); fitTimelineViewport()
    }
    func splitView(_ splitView: NSSplitView, constrainMinCoordinate proposed: CGFloat, ofSubviewAt index: Int) -> CGFloat { 195 }
    func splitView(_ splitView: NSSplitView, constrainMaxCoordinate proposed: CGFloat, ofSubviewAt index: Int) -> CGFloat { min(260, max(195, splitView.bounds.width - 320)) }
    func splitView(_ splitView: NSSplitView, canCollapseSubview subview: NSView) -> Bool { false }
    func splitViewDidResizeSubviews(_ notification: Notification) {
        guard let split = notification.object as? NSSplitView, split === trackTimelineSplit else { return }
        if workspaceLayoutReady, !restoringWorkspaceLayout, split.bounds.width > 0 {
            UserDefaults.standard.set(Double(split.subviews[0].frame.width / split.bounds.width), forKey: "workspace.trackHeaderRatio.v3")
        }
        fitTimelineViewport()
    }
    func fitTimelineViewport() {
        guard let timelineScroll, timelineScroll.contentSize.width > 0 else { return }
        let width = max(1, timelineScroll.contentSize.width) * timelineZoom
        if abs((timelineWidthConstraint?.constant ?? 0) - width) > 0.5 { timelineWidthConstraint?.constant = width }
        timelineDocument?.needsLayout = true
    }
    var shouldHandleWorkspaceClipDelete: Bool {
        guard let view = window.firstResponder as? NSView else { return true }
        return !(view is TimelineRangeView) && !(view is MidiArrangementView) && !view.isDescendant(of: libraryBrowser) && !view.isDescendant(of: inspectorBrowser.midiEditor)
    }

    func refreshWorkspaceSelection() {
        let clipSelection = (inspectorTrackID, inspectorClipIndex)
        let strips = mixerWorkspace.strips
        if selectedMixerID == nil || !strips.contains(where: { $0.id == selectedMixerID }) {
            selectedMixerID = strips.first(where: { $0.kind == .track })?.id ?? strips.first?.id
        }
        if let id = selectedMixerID {
            updateMixerInspector(id)
            if clipSelection.0 == id, let clip = clipSelection.1 { updateClipInspector(id, clip) }
        }
        else { inspectorTrackID = nil; inspectorBrowser.channel = nil; inspectorBrowser.clip = nil; inspectorBrowser.midi = nil; refreshDeviceRack() }
        inspectorBrowser.editingEnabled = !isRecording && !midiTakeArmed
        libraryBrowser.mutationEnabled = !isRecording && !midiTakeArmed
    }
    func refreshDeviceRack() {
        guard session != nil, let id = selectedMixerID,
              let strip = mixerWorkspace.strips.first(where: { $0.id == id }) else {
            channelRack.update(target: nil, title: "", output: "", accent: DAWDesignTokens.Color.accent, devices: [], editable: false); return
        }
        let owner = Int32(strip.kind == .track ? DAW_INSERT_OWNER_TRACK : (strip.kind == .bus ? DAW_INSERT_OWNER_BUS : DAW_INSERT_OWNER_MASTER))
        let target = RackTarget(owner: owner, ownerID: id)
        var count: UInt32 = 0, devices: [RackDevice] = []
        if daw_get_insert_count(session, owner, id, &count) == 0 {
            for index in 0..<count {
                var plugin = daw_plugin(); plugin.struct_size = UInt32(MemoryLayout<daw_plugin>.size)
                guard daw_get_insert(session, owner, id, index, &plugin) == 0 else { continue }
                let name = withUnsafeBytes(of: plugin.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                var hosting = daw_insert_hosting_status(); hosting.struct_size = UInt32(MemoryLayout<daw_insert_hosting_status>.size)
                let hasHosting = daw_get_insert_hosting_status(session, owner, id, plugin.id, &hosting) == 0
                let isolated = hasHosting && hosting.selected_mode == UInt32(DAW_INSERT_HOSTING_MODE_OUT_OF_PROCESS)
                let vst3 = plugin.type == 0 && plugin.subtype == 0 && plugin.manufacturer == 0
                devices.append(RackDevice(id: plugin.id, name: name, format: vst3 ? "VST3" : "AU", bypassed: plugin.bypassed != 0,
                                          available: plugin.available != 0, isolated: isolated, canEdit: hasHosting && (!isolated || vst3),
                                          latencyFrames: UInt64(plugin.latency_frames)))
            }
        }
        channelRack.update(target: target, title: strip.title, output: strip.outputName, accent: strip.color ?? DAWDesignTokens.Color.accent,
                           devices: devices, editable: !isRecording && !midiTakeArmed)
    }
    func performRackAction(_ target: RackTarget, _ action: RackAction) {
        guard !isRecording, !midiTakeArmed, target == channelRack.target else { return }
        if case .add = action {
            let sender = NSButton(); let key = ObjectIdentifier(sender)
            insertDisclosureTargets[key] = (target.owner, target.ownerID, inspectorBrowser.channel?.title ?? "Канал")
            defer { insertDisclosureTargets.removeValue(forKey: key) }
            addOwnerInsert(sender); return
        }
        if case let .edit(id) = action {
            guard let device = channelRack.devices.first(where: { $0.id == id }), device.canEdit else { return }
            let sender = NSButton(); let key = ObjectIdentifier(sender)
            insertEditorTargets[key] = (target.owner, target.ownerID, id, device.isolated && device.format == "VST3")
            defer { insertEditorTargets.removeValue(forKey: key) }
            editOwnerInsert(sender); return
        }
        finishEditing(); stopAudio()
        let result: Int32
        switch action {
        case let .bypass(id):
            guard let device = channelRack.devices.first(where: { $0.id == id }) else { return }
            result = daw_set_insert_bypass(session, target.owner, target.ownerID, id, device.bypassed ? 0 : 1, revision)
        case let .move(id, index):
            guard channelRack.devices.indices.contains(index) else { return }
            result = daw_move_insert(session, target.owner, target.ownerID, id, UInt32(index), revision)
        case let .remove(id): result = daw_remove_insert(session, target.owner, target.ownerID, id, revision)
        default: return
        }
        if check(result) { refresh(); pollTransport() }
    }
    func updateWorkspaceChrome() {
        timelineRuler.cycleRange.editingEnabled = !isRecording && !midiTakeArmed
        projectTitleLabel.stringValue = currentURL?.deletingPathExtension().lastPathComponent ?? "Новый черновик"
        projectStateLabel.stringValue = "\(dirty ? "Есть изменения" : "Сохранено") · \(trackIDs.count) дорожек · 48 kHz"
        projectStateLabel.toolTip = summary.stringValue
        let milliseconds = playheadFrame / 48
        clockLabel.stringValue = String(format: "%02llu:%02llu.%03llu", milliseconds / 60000, (milliseconds / 1000) % 60, milliseconds % 1000)
        let signature = tempoMap.signature(atFrame: playheadFrame)
        signatureLabel.stringValue = "\(signature.numerator)/\(signature.denominator)"
        status.toolTip = status.stringValue; transportLabel.toolTip = transportLabel.stringValue
        if let preference = workspace?.preference {
            workspaceToggleButtons[.library]?.state = preference.libraryVisible ? .on : .off
            workspaceToggleButtons[.inspector]?.state = preference.inspectorVisible ? .on : .off
            workspaceToggleButtons[.dock]?.state = preference.dockVisible ? .on : .off
        }
        inspectorBrowser.editingEnabled = !isRecording && !midiTakeArmed
        libraryBrowser.mutationEnabled = !isRecording && !midiTakeArmed
    }
}
