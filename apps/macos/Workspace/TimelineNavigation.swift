import AppKit

extension DraftApp {
    func wireTimelineNavigation() {
        let editor = ArrangementEditingController()
        window.arrangementEditing = editor
        editor.attach(to: self)
        let range = timelineRuler.cycleRange
        range.onCommit = { [weak self] in self?.commitTimelineRange($0) ?? false }
        range.onToggle = { [weak self] in self?.toggleLoop() }
        range.onClear = { [weak self] in self?.clearRange() }
        range.snapFrame = { [weak self] frame in
            guard let self else { return frame }
            let grid = self.gridSnap(atFrame: Int64(min(frame, BeatFrameMap.timelineLimitFrame)))
            guard grid.quantum > 0, grid.quantum <= BeatFrameMap.timelineLimitFrame, frame >= grid.anchor else { return frame }
            let steps = ((frame - grid.anchor) + grid.quantum / 2) / grid.quantum
            return min(BeatFrameMap.timelineLimitFrame, grid.anchor + steps * grid.quantum)
        }
    }
    func syncTimelineRange() {
        let range = timelineRuler.cycleRange
        var transport = daw_transport(); transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        guard let session, daw_get_transport(session, &transport) == 0 else { range.editingEnabled = false; return }
        range.projectFrames = timelineRuler.projectFrames
        range.playableFrames = transport.duration
        range.selection = rangeStart.flatMap { start in
            rangeEnd.flatMap { TimelineFrameRange(start: start, end: $0, limit: transport.duration) }
        }
        for view in midiArrangementViews {
            view.cycleSelection = range.selection; view.cycleEnabled = loopEnabled
        }
        range.loopEnabled = loopEnabled
        range.editingEnabled = !isRecording && !midiTakeArmed
        timelineRuler.window?.invalidateCursorRects(for: range)
    }
    @discardableResult
    func commitTimelineRange(_ range: TimelineFrameRange) -> Bool {
        guard !isRecording, !midiTakeArmed, let session else { return false }
        // The ABI validates against the *current* project duration and stops
        // playback on a loop edit. No revision or undo entry is consumed.
        guard daw_set_loop(session, 1, range.start, range.end) == 0 else {
            // A stale drag is recoverable. Keep the previous range and surface
            // the actual bridge reason without blocking editing in a modal loop.
            var bytes = [CChar](repeating: 0, count: 512)
            daw_error(session, &bytes, bytes.count)
            let reason = String(decoding: bytes.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }, as: UTF8.self)
            DAWLog.bridge.error("Изменение цикла отклонено: \(reason, privacy: .public)")
            setProjectMessage("Цикл не изменён: \(reason)")
            updateStorageStatus(); syncTimelineRange()
            return false
        }
        rangeStart = range.start; rangeEnd = range.end; loopEnabled = true
        setProjectMessage("Диапазон цикла обновлён")
        updateStorageStatus(); updateTimelineTools(); pollTransport()
        return true
    }
    @objc func setRangeStart() {
        guard !isRecording, !midiTakeArmed, let frame = currentTransportFrame() else { return }
        disableLoop(); guard !loopEnabled else { return }
        rangeStart = frame
        if let end = rangeEnd, end <= frame { rangeEnd = nil }
        updateTimelineTools(); pollTransport()
    }
    @objc func setRangeEnd() {
        guard !isRecording, !midiTakeArmed, let frame = currentTransportFrame() else { return }
        let start = rangeStart ?? 0
        guard frame > start else { storageMessage("Конец диапазона должен быть позже начала."); return }
        disableLoop(); guard !loopEnabled else { return }
        rangeStart = start; rangeEnd = frame
        updateTimelineTools(); pollTransport()
    }
    @objc func clearRange() {
        guard !isRecording, !midiTakeArmed else { return }
        disableLoop(); guard !loopEnabled else { return }
        rangeStart = nil; rangeEnd = nil
        updateTimelineTools(); pollTransport()
    }
    func disableLoop() {
        guard !isRecording, !midiTakeArmed, loopEnabled else { return }
        if check(daw_set_loop(session, 0, 0, 0)) { loopEnabled = false }
    }
    @objc func toggleLoop() {
        guard !isRecording, !midiTakeArmed else { return }
        if loopEnabled { disableLoop(); updateTimelineTools(); pollTransport(); return }
        guard let start = rangeStart, let end = rangeEnd, end > start else {
            storageMessage("Задай диапазон перетаскиванием в полосе цикла."); return
        }
        if check(daw_set_loop(session, 1, start, end)) { loopEnabled = true; updateTimelineTools(); pollTransport() }
    }

    func setTimelineZoom(_ zoom: CGFloat) {
        window.contentView?.layoutSubtreeIfNeeded()
        let scroll = timelineScroll
        let oldBounds = scroll?.contentView.bounds ?? .zero
        let anchor = TimelineZoomAnchor(documentWidth: Double(timelineDocument?.bounds.width ?? 1),
            viewportX: Double(oldBounds.minX), viewportWidth: Double(oldBounds.width),
            playheadFraction: Double(playheadFrame) / Double(max(1, timelineRuler.projectFrames)))
        timelineZoom = zoom.isFinite ? min(256, max(1, zoom)) : 1
        fitTimelineViewport()
        window.contentView?.layoutSubtreeIfNeeded()
        if let scroll, let document = timelineDocument {
            let x = anchor.origin(documentWidth: Double(document.bounds.width), viewportWidth: Double(scroll.contentSize.width))
            restoreArrangementViewport(NSPoint(x: x, y: oldBounds.minY))
        }
        UserDefaults.standard.set(Double(timelineZoom), forKey: "timelineZoom")
        timelineRuler.needsDisplay = true
    }
}
