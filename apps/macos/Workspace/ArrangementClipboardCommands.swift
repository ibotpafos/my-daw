import AppKit

extension ArrangementEditingController {
    func copySelection(cut: Bool) {
        guard let app, editable, gesture == nil, let revision = currentRevision(),
              revision == projectionRevision, documentID == app.midiDocumentID else {
            message("Заверши запись и обнови выделение перед копированием."); return
        }
        let group = items.filter { selection.contains($0.key) }
        guard !group.isEmpty, group.count == selection.count, group.count <= 256 else {
            message("Выбери от 1 до 256 клипов. Предыдущий буфер сохранён."); return
        }
        let ordered = app.trackIDs.keys.sorted().compactMap { app.trackIDs[$0] }
        let rows = group.compactMap { item in ordered.firstIndex(of: item.key.track) }
        guard rows.count == group.count, let top = rows.min(),
              let topLane = lanes.first(where: { $0.track == ordered[top] }),
              let anchor = group.map(\.bounds.start).min() else { return }
        let entries = zip(group, rows).map { item, row -> Clipboard.Entry in
            var bounds = item.bounds; bounds.start -= anchor
            return Clipboard.Entry(rowOffset: row - top, kind: item.key.kind, bounds: bounds)
        }
        let board = Clipboard(document: app.midiDocumentID, entries: entries)
        let refs: [daw_clip_selection_ref] = group.map { item in
            var ref = daw_clip_selection_ref()
            ref.struct_size = UInt32(MemoryLayout<daw_clip_selection_ref>.size)
            ref.version = UInt32(DAW_CLIP_SELECTION_REF_VERSION)
            ref.track_id = item.key.track; ref.clip_index = UInt32(item.key.index)
            ref.kind = item.key.kind == .midi ? 2 : 1
            return ref
        }
        let capture: (DraftApp, UInt64) -> Int32 = { app, revision in
            refs.withUnsafeBufferPointer {
                daw_capture_selection_clipboard(app.session, $0.baseAddress, UInt32($0.count), cut ? 1 : 0, revision)
            }
        }
        if cut {
            guard perform(expected: revision, capture) else { return }
        } else if capture(app, revision) != 0 {
            message("Копирование отклонено. Предыдущий буфер сохранён."); return
        }
        clipboard = board
        // Immediate Paste/Undo-Cut/Paste should anchor at the original top row,
        // not at whichever lower clip happened to be selected last.
        if let lane = lanes.first(where: { $0.track == topLane.track }) { focus(lane) }
        message(cut ? "Вырезано: \(group.count). ⌘V — вставить, ⌘Z — вернуть оригинал." :
            "Скопировано: \(group.count). Вставка начинается с выбранной дорожки и сохраняет промежутки.")
    }

    /// Menu availability and dispatch share the same document/row/type check.
    /// Final overlap, source slots, note and Undo budgets belong to the C ABI.
    func clipboardDestinations(_ board: Clipboard, track: UInt64, start: UInt64)
        -> [(UInt64, ArrangementClipKey.Kind, ArrangementClipBounds)]? {
        guard let app, board.document == app.midiDocumentID,
              documentID == app.midiDocumentID, projectionRevision == currentRevision(),
              !board.entries.isEmpty, board.entries.count <= 256, start <= Self.limit else { return nil }
        let ordered = app.trackIDs.keys.sorted().compactMap { app.trackIDs[$0] }
        guard let top = ordered.firstIndex(of: track) else { return nil }
        var result: [(UInt64, ArrangementClipKey.Kind, ArrangementClipBounds)] = []
        for entry in board.entries {
            guard entry.rowOffset >= 0, entry.rowOffset < ordered.count - top,
                  let lane = lanes.first(where: { $0.track == ordered[top + entry.rowOffset] }),
                  lane.kind == nil || lane.kind == entry.kind,
                  entry.bounds.isValid(limit: Self.limit), entry.bounds.end <= Self.limit - start else { return nil }
            var bounds = entry.bounds; bounds.start += start
            result.append((lane.track, entry.kind, bounds))
        }
        return result
    }

    func paste() {
        guard let app, editable, gesture == nil, let board = clipboard,
              let track = focusTrack, board.document == app.midiDocumentID else {
            message("Вставка недоступна: проверь буфер, фокус и состояние записи."); return
        }
        let continuation = board.continuation
        let start = continuation?.track == track && continuation?.frame == app.playheadFrame ?
            app.playheadFrame : snapped(app.playheadFrame, flags: [])
        guard let restoring = clipboardDestinations(board, track: track, start: start) else {
            message("Группа не помещается или тип целевой дорожки не подходит. Буфер сохранён."); return
        }
        if perform(expected: projectionRevision, restoring: restoring, { app, revision in
            daw_paste_clipboard(app.session, track, start, revision)
        }) {
            let end = restoring.map { $0.2.end }.max() ?? start
            seekPlayhead(end)
            var next = board; next.continuation = (track, end); clipboard = next
        }
    }
}
