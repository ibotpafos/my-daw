import AppKit

extension ArrangementEditingController {
    @discardableResult
    func perform(expected: UInt64? = nil,
                 restoring: [(UInt64, ArrangementClipKey.Kind, ArrangementClipBounds)] = [],
                 _ command: (DraftApp, UInt64) -> Int32) -> Bool {
        guard let app, editable, let revision = currentRevision() else {
            message("Останови запись и заверши текущий жест перед редактированием."); return false
        }
        guard documentID == app.midiDocumentID, projectionRevision == revision,
              expected == nil || expected == revision else {
            cancelGesture(); app.refresh(); message("Изменение отменено: проект уже обновился."); return false
        }
        app.finishEditing(); app.stopAudio()
        let result = command(app, revision)
        if result != 0 {
            var bytes = [CChar](repeating: 0, count: 512)
            daw_error(app.session, &bytes, bytes.count)
            let reason = String(decoding: bytes.prefix { $0 != 0 }.map { UInt8(bitPattern: $0) }, as: UTF8.self)
            message("Клип не изменён: \(reason)")
            return false
        }
        restoringSelection = restoring; restoreKeyboardFocus = true
        app.refresh(); app.pollTransport()
        message("Изменение применено · ⌘Z — отменить")
        return true
    }

    func commit(_ g: Gesture, event: NSEvent) {
        guard let app, g.document == app.midiDocumentID, currentRevision() == g.revision else {
            message("Жест отменён: проект изменился."); return
        }
        switch g.action {
        case .pan: return
        case .zoom:
            if g.dragged {
                fit(start: min(g.originFrame, frame(at: event.locationInWindow, in: g.lane)),
                    end: max(g.originFrame, frame(at: event.locationInWindow, in: g.lane)))
            } else { zoom(by: g.copy ? 0.5 : 2, at: event.locationInWindow) }
        case .marquee:
            if !g.dragged {
                if !g.additive { selection.removeAll(); syncSelection() }
                seekPlayhead(min(g.originFrame, playableEnd())); return
            }
            let end = overlay.convert(event.locationInWindow, from: nil)
            let box = NSRect(x: min(g.originDocument.x, end.x), y: min(g.originDocument.y, end.y),
                             width: abs(end.x - g.originDocument.x), height: max(1, abs(end.y - g.originDocument.y)))
            let selected = items.filter { item in
                guard let lane = lanes.first(where: { $0.track == item.key.track }) else { return false }
                return lane.view.convert(rect(item.bounds, in: lane), to: overlay).intersects(box)
            }.map(\.key)
            selection = g.additive ? selection.union(selected) : Set(selected); syncSelection()
        case .draw:
            var bounds = g.proposed.first
            if bounds == nil {
                let start = snapped(g.originFrame, flags: event.modifierFlags)
                let grid = app.gridSnap(atFrame: Int64(start)).quantum
                let quantum = grid > 0 ? grid : UInt64((48_000 * 60 / app.tempoMap.bpm(atFrame: start)).rounded())
                bounds = .init(start: start, length: min(Self.limit - start, quantum * 4))
            }
            guard let bounds, bounds.isValid(limit: Self.limit) else { return }
            focusTrack = g.lane.track
            perform(expected: g.revision, restoring: [(g.lane.track, .midi, bounds)]) { app, revision in
                var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
                clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.start = bounds.start; clip.length = bounds.length
                return daw_add_midi_clip(app.session, g.lane.track, &clip, nil, 0, revision)
            }
        case .move:
            guard g.dragged, let moved = g.proposed.first, let original = g.items.first,
                  let source = lanes.firstIndex(where: { $0.track == g.lane.track }),
                  let target = lanes.firstIndex(where: { $0.track == g.target }) else { return }
            let delta = Int64(moved.start) - Int64(original.bounds.start)
            editSelection(g.items, action: g.copy ? UInt32(DAW_CLIP_SELECTION_COPY) : UInt32(DAW_CLIP_SELECTION_MOVE),
                          delta: delta, trackOffset: target - source, expected: g.revision)
        case .trimLeft, .trimRight, .fadeLeft, .fadeRight:
            guard g.dragged, let primary = g.primary, let b = g.proposed.first, b != primary.bounds else { return }
            perform(expected: g.revision, restoring: [(primary.key.track, primary.key.kind, b)]) { app, revision in
                if primary.key.kind == .midi {
                    return daw_trim_midi_clip(app.session, primary.key.track, UInt32(primary.key.index), b.start, b.length, revision)
                }
                return daw_edit_clip_full(app.session, primary.key.track, UInt32(primary.key.index), b.start, b.offset,
                                          b.length, b.fadeIn, b.fadeOut, revision)
            }
        }
    }

    /// Map every source row by the same offset, including unselected rows.
    /// Kept identical between the ghost preview and the commit adapter.
    func groupDestinations(_ group: [Item], trackOffset: Int) -> [Lane]? {
        var result: [Lane] = []
        for item in group {
            guard let source = lanes.firstIndex(where: { $0.track == item.key.track }) else { return nil }
            let target = source + trackOffset
            guard lanes.indices.contains(target) else { return nil }
            result.append(lanes[target])
        }
        return result
    }

    @discardableResult
    func editSelection(_ group: [Item], action: UInt32, delta: Int64 = 0, trackOffset: Int = 0,
                       expected: UInt64?) -> Bool {
        guard !group.isEmpty, group.count <= 256 else {
            message("Выбери от 1 до 256 клипов для одной операции."); return false
        }
        let deleting = action == UInt32(DAW_CLIP_SELECTION_DELETE)
        if action == UInt32(DAW_CLIP_SELECTION_MOVE), delta == 0, trackOffset == 0 { return false }
        guard let offset = Int32(exactly: trackOffset),
              let destinations = groupDestinations(group, trackOffset: trackOffset) else {
            message("Вся группа должна помещаться в существующие дорожки. Ничего не изменено."); return false
        }
        var restoring: [(UInt64, ArrangementClipKey.Kind, ArrangementClipBounds)] = []
        if !deleting {
            guard ArrangementEditMath.moveDelta(delta, clips: group.map(\.bounds), limit: Self.limit) == delta else {
                message("Вся группа должна помещаться в таймлайн. Ничего не изменено."); return false
            }
            restoring = zip(group, destinations).map { item, lane in
                var bounds = item.bounds; bounds.start = UInt64(Int64(bounds.start) + delta)
                return (lane.track, item.key.kind, bounds)
            }
        }
        let refs: [daw_clip_selection_ref] = group.map { item in
            var ref = daw_clip_selection_ref()
            ref.struct_size = UInt32(MemoryLayout<daw_clip_selection_ref>.size)
            ref.version = UInt32(DAW_CLIP_SELECTION_REF_VERSION)
            ref.track_id = item.key.track; ref.clip_index = UInt32(item.key.index)
            ref.kind = item.key.kind == .midi ? 2 : 1
            return ref
        }
        let oldFocus = focusTrack
        if !deleting, let index = group.firstIndex(where: { $0.key.track == focusTrack }) {
            focusTrack = destinations[index].track
        }
        let success = perform(expected: expected, restoring: restoring) { app, revision in
            refs.withUnsafeBufferPointer {
                daw_edit_clip_selection(app.session, $0.baseAddress, UInt32($0.count), action, delta, offset, revision)
            }
        }
        if !success { focusTrack = oldFocus }
        return success
    }

    func transfer(_ item: Item, target: UInt64, start: UInt64, copy: Bool, expected: UInt64) {
        guard start <= Self.limit, item.bounds.start <= Self.limit,
              let from = lanes.firstIndex(where: { $0.track == item.key.track }),
              let to = lanes.firstIndex(where: { $0.track == target }) else { return }
        editSelection([item], action: copy ? UInt32(DAW_CLIP_SELECTION_COPY) : UInt32(DAW_CLIP_SELECTION_MOVE),
                      delta: Int64(start) - Int64(item.bounds.start), trackOffset: to - from, expected: expected)
    }
    func singleSelection() -> Item? {
        guard selection.count == 1 else {
            message(selection.isEmpty ? "Сначала выбери клип." : "Для этой операции выбери один клип; группа не изменена."); return nil
        }
        return items.first { selection.contains($0.key) }
    }
    func split(_ item: Item, at frame: UInt64) {
        guard frame > item.bounds.start, frame < item.bounds.end else { return }
        perform(expected: projectionRevision) { app, revision in
            item.key.kind == .audio ? daw_split_clip(app.session, item.key.track, UInt32(item.key.index), frame, revision) :
                daw_split_midi_clip(app.session, item.key.track, UInt32(item.key.index), frame, revision)
        }
    }
    func toggleMute(_ item: Item) {
        guard item.key.kind == .audio else { message("Mute MIDI-клипа ещё не поддерживается моделью проекта."); return }
        perform(expected: projectionRevision, restoring: [(item.key.track, item.key.kind, item.bounds)]) { app, revision in
            var clip = daw_clip(); clip.struct_size = UInt32(MemoryLayout<daw_clip>.size)
            guard daw_get_clip(app.session, item.key.track, UInt32(item.key.index), &clip) == 0 else { return 1 }
            return daw_set_clip_muted(app.session, item.key.track, UInt32(item.key.index), clip.muted == 0 ? 1 : 0, revision)
        }
    }
    func deleteSelection() {
        let selected = items.filter { selection.contains($0.key) }
        guard !selected.isEmpty else { return }
        editSelection(selected, action: UInt32(DAW_CLIP_SELECTION_DELETE), expected: projectionRevision)
    }
    func duplicateSelection() {
        let selected = items.filter { selection.contains($0.key) }
        guard let start = selected.map(\.bounds.start).min(), let end = selected.map(\.bounds.end).max() else { return }
        guard end > start, end - start <= UInt64(Int64.max) else { return }
        editSelection(selected, action: UInt32(DAW_CLIP_SELECTION_COPY), delta: Int64(end - start), expected: projectionRevision)
    }
    func playableEnd() -> UInt64 {
        guard let app else { return 0 }
        var transport = daw_transport(); transport.struct_size = UInt32(MemoryLayout<daw_transport>.size)
        return daw_get_transport(app.session, &transport) == 0 ? transport.duration : 0
    }
    func zoom(by factor: CGFloat, at point: NSPoint) {
        guard factor.isFinite, factor > 0, let app, let scroll = app.timelineScroll, let document = app.timelineDocument else { return }
        let local = document.convert(point, from: nil), old = scroll.contentView.bounds
        let fraction = local.x / max(1, document.bounds.width), viewportX = local.x - old.minX
        app.setTimelineZoom(app.timelineZoom * factor)
        app.restoreArrangementViewport(NSPoint(x: fraction * document.bounds.width - viewportX, y: old.minY))
    }
    func fit(start: UInt64, end: UInt64) {
        guard end > start, let app, let scroll = app.timelineScroll else { return }
        let total = Double(max(1, app.timelineRuler.projectFrames))
        app.setTimelineZoom(CGFloat(total / Double(end - start) * 0.85))
        let x = CGFloat(Double(start) / total) * app.timelineDocument.bounds.width - scroll.contentSize.width * 0.075
        app.restoreArrangementViewport(NSPoint(x: x, y: scroll.contentView.bounds.minY))
    }
    func handleKey(_ event: NSEvent) -> Bool {
        guard let app else { return false }
        if event.keyCode == 53, gesture != nil { cancelGesture(); return true }
        // Never route from an editor, mixer, text field, sheet or another window.
        let responder = app.window.firstResponder as? NSView
        let focused = responder.map { view in lanes.contains { view === $0.view || view.isDescendant(of: $0.view) } } ?? false
        let emptyLaneFocus = app.window.firstResponder === app.window && focusTrack != nil
        guard focused || emptyLaneFocus else { return false }
        let flags = event.modifierFlags.intersection([.command, .option, .control, .shift])
        if flags.isEmpty, let tool = ArrangementTool.key(event.keyCode) { if !event.isARepeat { selectTool(tool) }; return true }
        if gesture != nil {
            // Cmd-K/S/Z and menu equivalents remain reachable, but discard the
            // uncommitted preview before the responder chain executes them.
            if flags.contains(.command) { cancelGesture(); return false }
            return true
        }
        if flags == [.command, .shift], event.keyCode == 0 {
            selection = Set(items.map(\.key)); syncSelection(); return true
        }
        if flags == [.command] {
            switch event.keyCode {
            case 0: selection = Set(items.filter { $0.key.track == focusTrack }.map(\.key)); syncSelection(); return true
            case 8: copySelection(cut: false); return true
            case 7: copySelection(cut: true); return true
            case 9: paste(); return true
            case 2: duplicateSelection(); return true
            default: return false // Preserve Cmd-S/Z/K, track deletion and menu routing.
            }
        }
        if flags == [.option], event.keyCode == 123 || event.keyCode == 124 {
            nudge(event.keyCode == 124 ? 1 : -1); return true
        }
        guard flags.isEmpty || flags == [.shift] else { return false }
        switch event.keyCode {
        case 51, 117: deleteSelection(); return true
        case 1: if let item = singleSelection() { split(item, at: app.playheadFrame) }; return true
        case 2: duplicateSelection(); return true
        case 8: copySelection(cut: false); return true
        case 9: paste(); return true
        case 46: if let item = singleSelection() { toggleMute(item) }; return true
        case 6:
            if flags == [.shift] { app.setTimelineZoom(1) }
            else {
                let selected = items.filter { selection.contains($0.key) }
                if let start = selected.map(\.bounds.start).min(), let end = selected.map(\.bounds.end).max() { fit(start: start, end: end) }
            }
            return true
        case 48:
            let boundaries = Set(items.flatMap { [$0.bounds.start, $0.bounds.end] }).sorted()
            let next = flags == [.shift] ? boundaries.last(where: { $0 < app.playheadFrame }) : boundaries.first(where: { $0 > app.playheadFrame })
            if let next { seekPlayhead(min(next, playableEnd())); revealPlayhead() }; return true
        case 123, 124:
            let quantum = max(1, app.gridSnap(atFrame: Int64(app.playheadFrame)).quantum)
            let target = event.keyCode == 124 ? min(playableEnd(), app.playheadFrame + quantum) : app.playheadFrame - min(app.playheadFrame, quantum)
            seekPlayhead(target); revealPlayhead(); return true
        case 125, 126:
            if let index = lanes.firstIndex(where: { $0.track == focusTrack }) {
                let next = min(lanes.count - 1, max(0, index + (event.keyCode == 125 ? 1 : -1)))
                focus(lanes[next]); lanes[next].view.scrollToVisible(lanes[next].view.bounds)
            }
            return true
        case 115: seekPlayhead(0); revealPlayhead(); return true
        case 119: seekPlayhead(playableEnd()); revealPlayhead(); return true
        default: return false
        }
    }
    func nudge(_ direction: Int64) {
        guard let app, let first = items.first(where: { selection.contains($0.key) }) else { return }
        let group = items.filter { selection.contains($0.key) }
        let quantum = max(1, app.gridSnap(atFrame: Int64(first.bounds.start)).quantum)
        let delta = ArrangementEditMath.moveDelta(direction * Int64(quantum), clips: group.map(\.bounds), limit: Self.limit)
        guard delta != 0 else { return }
        editSelection(group, action: UInt32(DAW_CLIP_SELECTION_MOVE), delta: delta, expected: projectionRevision)
    }
    func seekPlayhead(_ frame: UInt64) {
        guard let app else { return }
        guard !app.isRecording, !app.midiTakeArmed else {
            message("Во время записи можно двигать вид, но не позицию транспорта."); return
        }
        app.seekAudio(frame)
    }
    func revealPlayhead() {
        guard let app, let scroll = app.timelineScroll else { return }
        let x = CGFloat(Double(app.playheadFrame) / Double(max(1, app.timelineRuler.projectFrames))) * app.timelineDocument.bounds.width
        app.restoreArrangementViewport(NSPoint(x: x - scroll.contentSize.width * 0.25, y: scroll.contentView.bounds.minY))
    }
}
