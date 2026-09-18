import AppKit

@MainActor
extension DraftApp {
    var recordingWorkspaceBusy: Bool { isRecording || midiTakeArmed || exportBusy || importBusy }
    func wireRecordingWorkspace() {
        recordingWorkspace.onTake = { [weak self] take in
            guard let self, let id = recordingWorkspace.trackID else { return }
            selectedTakes[id] = Int(take)
        }
        recordingWorkspace.onCommit = { [weak self] in self?.commitRecordingComp($0) }
        recordingWorkspace.onTogglePlayback = { [weak self] in self?.togglePlayStop() }
        recordingWorkspace.onAction = { [weak self] in self?.recordingWorkspaceAction($0) }
    }
    func refreshRecordingWorkspace() {
        guard workspace?.screen == .recording, session != nil else { return }
        let view = recordingWorkspace
        if view.documentID != midiDocumentID {
            for lane in view.lanes where lane.isGesturing { lane.cancelSwipe() }
        }
        guard !view.isGesturing else { return }
        var choices: [daw_track] = []
        for index in trackIDs.keys.sorted() {
            var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
            if daw_get_track(session, UInt32(index), &track) == 0, track.take_count > 0 { choices.append(track) }
        }
        let target = isRecording ? activeRecordingTarget : selectedMixerID
        let track = choices.first(where: { $0.id == target }) ?? choices.first(where: { $0.id == armedTrackID }) ?? choices.first
        view.tracks.removeAllItems(); view.trackIDs = choices.map(\.id)
        for choice in choices { view.tracks.addItem(withTitle: trackNames[choice.id] ?? "Аудиодорожка") }
        if let track, let index = view.trackIDs.firstIndex(of: track.id) { view.tracks.selectItem(at: index) }
        var takes: [RecordTakeSnapshot] = []
        if let track {
            for index in 0..<track.take_count {
                var take = daw_take(); take.struct_size = UInt32(MemoryLayout<daw_take>.size)
                var peaks = [Float](repeating: 0, count: 512)
                guard daw_get_take(session, track.id, index, &take) == 0,
                      daw_get_take_waveform(session, track.id, index, &peaks, UInt32(peaks.count)) == 0 else { continue }
                let name = withUnsafeBytes(of: take.name) { String(decoding: $0.prefix(while: { $0 != 0 }), as: UTF8.self) }
                takes.append(RecordTakeSnapshot(index: index, title: index == 0 ? "Основной" : name,
                                                start: take.start, frames: take.frames, peaks: peaks))
            }
        }
        let clips = track.flatMap { laneViews[$0.id]?.clips } ?? []
        let selection = UInt32(max(0, min(selectedTakes[track?.id ?? 0] ?? 0, max(0, takes.count - 1))))
        view.rebuild(trackID: track?.id, documentID: midiDocumentID, revision: revision, takes: takes, clips: clips, selected: selection)
        updateRecordingWorkspaceRuntime()
    }
    func updateRecordingWorkspaceRuntime() {
        guard workspace?.screen == .recording else { return }
        let view = recordingWorkspace
        let clips = view.trackID.flatMap { laneViews[$0]?.clips } ?? []
        let ordered = clips.sorted { $0.start < $1.start }
        let overlap = zip(ordered, ordered.dropFirst()).contains { $0.start + $0.length > $1.start }
        view.editable = !recordingWorkspaceBusy && !overlap && !view.snapshots.isEmpty
        view.apply.isEnabled = view.editable
        view.tracks.isEnabled = !recordingWorkspaceBusy && !view.trackIDs.isEmpty
        view.takes.isEnabled = view.editable
        view.start.isEnabled = view.editable; view.end.isEnabled = view.editable
        view.arm.isEnabled = !recordingWorkspaceBusy && view.trackID != nil
        view.arm.state = view.trackID != nil && armedTrackID == view.trackID ? .on : .off
        let destination = isRecording ? activeRecordingTarget : armedTrackID
        view.recordTarget.stringValue = destination.map { "Запись → " + (trackNames[$0] ?? "Дорожка") } ?? "Запись → новая дорожка"
        view.record.title = isRecording ? "■ Завершить запись" : "● Запись"
        view.record.isEnabled = !midiTakeArmed && !exportBusy && !importBusy
        view.monitor.isEnabled = true; view.monitor.state = recordMonitorOn ? .on : .off
        view.loop.isEnabled = !recordingWorkspaceBusy && view.trackID != nil && armedTrackID == view.trackID && (loopEnabled || view.requestFromFields() != nil)
        view.loop.state = loopEnabled && armedTrackID != nil ? .on : .off
        view.loop.toolTip = "Для записи в цикле включите Дубли в эту дорожку. Без arm запись создаёт новую дорожку без деления на дубли."
        view.preroll.isEnabled = !recordingWorkspaceBusy
        view.settings.isEnabled = !recordingWorkspaceBusy
        view.effects.isEnabled = !recordingWorkspaceBusy && view.trackID != nil
        view.importTake.isEnabled = !recordingWorkspaceBusy && view.trackID != nil
        view.fit.isEnabled = !view.isGesturing; view.zoom.isEnabled = !view.isGesturing && view.editable
        var preroll: UInt64 = 0
        if daw_get_record_preroll(session, &preroll) == 0 {
            let seconds = Double(preroll)/48000
            if let item = view.preroll.itemArray.first(where: { Double($0.tag) == seconds }) {
                view.preroll.select(item)
            } else {
                while view.preroll.numberOfItems > 5 { view.preroll.removeItem(at: 5) }
                view.preroll.addItem(withTitle: String(format: "Преролл: %.3f с", seconds))
                view.preroll.lastItem?.tag = -1; view.preroll.selectItem(at: view.preroll.numberOfItems - 1)
            }
        }
        if overlap { view.hint.stringValue = "Клипы перекрываются. Убери crossfade в Проекте перед сборкой нового comp." }
        view.clock.stringValue = transportLabel.stringValue
        for lane in view.lanes { lane.playhead = playheadFrame }
    }
    func recordingWorkspaceAction(_ sender: NSControl) {
        let view = recordingWorkspace
        guard !view.isGesturing else { return }
        defer { updateRecordingWorkspaceRuntime() }
        if sender === view.monitor { toggleRecordMonitor(view.monitor); return }
        if sender === view.record { if view.record.isEnabled { toggleRecording() }; return }
        guard !recordingWorkspaceBusy else { return }
        if sender === view.tracks {
            let index = view.tracks.indexOfSelectedItem
            guard view.trackIDs.indices.contains(index) else { return }
            selectedMixerID = view.trackIDs[index]; inspectorTrackID = selectedMixerID; inspectorClipIndex = nil
            refresh()
        } else if sender === view.arm, let id = view.trackID {
            performTrackAction(id, #selector(toggleArm(_:)))
        } else if sender === view.settings { showAudioDeviceSettings() }
        else if sender === view.effects, let id = view.trackID {
            selectedMixerID = id; updateMixerInspector(id); workspace?.present(.devices)
        } else if sender === view.importTake, let id = view.trackID {
            performTrackAction(id, #selector(importTake(_:)))
        } else if sender === view.preroll, let seconds = view.preroll.selectedItem?.tag, seconds >= 0 {
            let item = NSMenuItem(); item.representedObject = Double(seconds); pickRecordPreroll(item)
        } else if sender === view.apply {
            guard view.editable, let request = view.requestFromFields() else {
                view.hint.stringValue = "Задай начало и конец фразы внутри выбранного дубля."; return
            }
            commitRecordingComp(request)
        } else if sender === view.loop {
            guard view.loop.isEnabled else { return }
            if loopEnabled {
                if check(daw_set_loop(session, 0, 0, 0)) { loopEnabled = false }
            } else if let request = view.requestFromFields() {
                let range = request.range
                if check(daw_set_loop(session, 1, range.start, range.end)) {
                    rangeStart = range.start; rangeEnd = range.end; loopEnabled = true
                }
            }
            updateTimelineTools(); pollTransport()
        }
    }
    func commitRecordingComp(_ request: RecordCompRequest) {
        let view = recordingWorkspace
        guard workspace?.screen == .recording, !recordingWorkspaceBusy, view.editable,
              view.trackID == request.trackID, request.documentID == midiDocumentID, !view.isGesturing,
              consoleGesture == nil, automationGesture == nil, pluginParameterGesture == nil,
              !mixerWorkspace.linkedLevels.isEditing, window.makeFirstResponder(nil) else { return }
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        guard daw_get_snapshot(session, &snapshot) == 0, snapshot.revision == request.revision else {
            refresh(); view.hint.stringValue = "Проект изменился. Выдели фразу заново."; return
        }
        stopAudio()
        guard check(daw_comp_range(session, request.trackID, request.takeIndex,
                                   request.range.start, request.range.end, request.revision)) else { return }
        rangeStart = request.range.start; rangeEnd = request.range.end
        selectedTakes[request.trackID] = Int(request.takeIndex); selectedClips[request.trackID] = 0
        view.start.stringValue = String(format: "%.6f", Double(request.range.start)/48000)
        view.end.stringValue = String(format: "%.6f", Double(request.range.end)/48000)
        refresh(); pollTransport()
        view.hint.stringValue = "Фраза добавлена в comp. ⌘Z отменяет сборку одним действием."
    }
}
