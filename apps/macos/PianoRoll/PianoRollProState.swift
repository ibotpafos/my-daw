import Foundation

@MainActor
final class PRProState {
    enum GestureSource { case pointer, transform }

    struct Gesture {
        let id: UUID
        let original: [PRNoteEntity]
        let originalSelection: Set<UInt64>
        let generation: UInt64
        let source: GestureSource
    }

    private struct PendingCommit {
        let id: UUID
        let candidate: [PRNoteEntity]
        let original: [PRNoteEntity]
        let originalSelection: Set<UInt64>
        let selection: Set<UInt64>
        let context: PRClipContext?
        let generation: UInt64
        let clipStart: UInt64
        let clipLength: UInt64
    }

    private var store = PRNoteStore()
    private var pendingCommit: PendingCommit?
    private var hostEditable = false
    private(set) var gesture: Gesture?
    private(set) var preview: [PRNoteEntity]?
    private(set) var generation: UInt64 = 0
    private(set) var index = PRNoteIndex([])
    private(set) var context: PRClipContext?
    private(set) var timeMap: PRTimeMap?

    /// A missing host echo freezes writes. The last confirmed store is retained.
    var editable: Bool { hostEditable && pendingCommit == nil }
    var awaitingCommit: Bool { pendingCommit != nil }
    var canPerformEdit: Bool { editable && gesture == nil && timeMap != nil }
    var selection = Set<UInt64>()
    var tool: PRTool = .select
    var grid = PRGrid()
    var scale = PRScale()
    var lockScale = false
    var fold = false
    var followPlayhead = false
    var showVelocity = true
    var pixelsPerBeat = 96.0
    var rowHeight = 18.0
    var defaultLengthBeats = 0.25
    var defaultVelocity: UInt8 = 100
    var defaultChannel: UInt8 = 0
    var playheadFrame: UInt64 = 0
    var insertionBeat = 0.0
    var quantizeStrength = 1.0
    var humanizeTimingBeats = 0.015
    var humanizeVelocity = 6
    var status = "V — выбор · B — нота · Q — квантовать · ⌘Z — undo проекта"

    /// Production uses the revision-bound request; the note-only callback is
    /// retained for embedders/tests, which must echo an authoritative snapshot.
    var onCommitRequest: ((PRCommitRequest) -> Void)?
    var onCommit: (([PianoRollNote]) -> Void)?
    var onChange: (() -> Void)?

    var entities: [PRNoteEntity] { preview ?? store.entities }
    var selectedEntities: [PRNoteEntity] { entities.filter { selection.contains($0.id) } }
    var nextID: UInt64 {
        let maximum = entities.map(\.id).max() ?? 0
        return max(store.nextID, maximum == UInt64.max ? UInt64.max : maximum + 1)
    }
    var isGesturing: Bool { gesture != nil }
    var isTransformPreview: Bool { gesture?.source == .transform }
    var pitchRows: PRPitchRows {
        PRPitchRows(used: fold ? Set(store.entities.map { Int($0.note.pitch) }) : nil)
    }

    func receive(notes: [PianoRollNote], map: PRTimeMap?, editable: Bool,
                 context incomingContext: PRClipContext? = nil) {
        let pending = pendingCommit
        let priorGesture = gesture
        let sameClip: Bool
        switch (context, incomingContext) {
        case let (old?, new?): sameClip = old.sameClip(as: new)
        case (nil, nil): sameClip = true
        default: sameClip = false
        }
        // Repeated UI refreshes of one revision are not external edits. This
        // lets the transport refresh capture state without cancelling drags.
        if incomingContext != nil, incomingContext == context, pending == nil,
           notes == store.entities.map(\.note), hostEditable == (editable && map != nil),
           map?.clipStart == timeMap?.clipStart, map?.clipLength == timeMap?.clipLength {
            return
        }
        gesture = nil
        preview = nil
        pendingCommit = nil
        generation &+= 1
        context = incomingContext
        timeMap = map
        hostEditable = editable && map != nil
        do { try PREdits.validate(notes, clipLength: map?.clipLength ?? PRLimits.timelineFrames) }
        catch {
            hostEditable = false
            if !sameClip { store = PRNoteStore(); selection.removeAll() }
            index = PRNoteIndex(store.entities)
            fail(error)
            return
        }

        if !sameClip {
            store = PRNoteStore(notes: notes)
            selection.removeAll()
            status = "Выбран другой MIDI-клип"
        } else if let pending {
            let revisionMatches: Bool
            if let old = pending.context, let new = incomingContext {
                revisionMatches = old.sameClip(as: new) && old.revision < UInt64.max && new.revision == old.revision + 1
            } else { revisionMatches = pending.context == nil && incomingContext == nil }
            let accepted = notes == pending.candidate.map(\.note) && revisionMatches &&
                map?.clipStart == pending.clipStart && map?.clipLength == pending.clipLength
            store.replace(with: notes, preferring: accepted ? pending.candidate : pending.original)
            selection = accepted ? pending.selection : pending.originalSelection
            status = accepted
                ? "Изменение применено · ⌘Z — отменить в проекте"
                : "Изменение отклонено движком · показана актуальная версия проекта"
        } else {
            store.replace(with: notes)
            if let priorGesture {
                selection = priorGesture.originalSelection
                status = "Проект изменился во время жеста — жест отменён."
            }
        }
        selection.formIntersection(Set(store.entities.map(\.id)))
        index = PRNoteIndex(store.entities)
        changed()
    }

    func changed() { onChange?() }
    func setStatus(_ value: String) { status = value; changed() }
    func fail(_ error: Error) {
        status = (error as? PREditError)?.description ?? String(describing: error)
        changed()
    }
    func selectAll() {
        guard !isTransformPreview, !awaitingCommit else { return }
        selection = Set(entities.map(\.id))
        status = "Выбрано нот: \(selection.count)"
        changed()
    }
    func clearSelection() {
        guard !isTransformPreview, !awaitingCommit else { return }
        selection.removeAll(); changed()
    }

    @discardableResult
    func beginGesture(source: GestureSource = .pointer) -> Bool {
        guard canPerformEdit else {
            setStatus(isTransformPreview ? "Сначала примените или отмените предпросмотр преобразования." : PREditError.unavailable.description)
            return false
        }
        gesture = Gesture(id: UUID(), original: store.entities, originalSelection: selection,
                          generation: generation, source: source)
        preview = nil
        return true
    }

    /// Event sources retain this UUID. A late mouse-up cannot finish another
    /// surface's newer transaction, even after a document reload.
    func ownsGesture(_ id: UUID?) -> Bool { id != nil && gesture?.id == id }

    func previewGesture(_ candidate: [PRNoteEntity], transaction: UUID? = nil) {
        guard let gesture, transaction == nil || gesture.id == transaction, let map = timeMap else { return }
        do { preview = try PREdits.checked(candidate, map: map); changed() }
        catch {
            preview = nil
            fail(error)
        }
    }

    func invalidateGesture(_ error: Error, transaction: UUID? = nil) {
        guard let gesture, transaction == nil || gesture.id == transaction else { return }
        preview = nil
        fail(error)
    }

    func cancelGesture(transaction: UUID? = nil) {
        guard let gesture, transaction == nil || gesture.id == transaction else { return }
        selection = gesture.originalSelection
        self.gesture = nil; preview = nil
        status = "Жест отменён"
        changed()
    }

    func finishGesture(transaction: UUID? = nil) {
        guard let gesture, transaction == nil || gesture.id == transaction else { return }
        let candidate = preview
        let candidateSelection = selection
        self.gesture = nil; preview = nil
        selection = gesture.originalSelection
        guard generation == gesture.generation else { fail(PREditError.staleEdit); return }
        guard let candidate else { changed(); return }
        commit(candidate, original: gesture.original, selection: candidateSelection)
    }

    func perform(_ transform: ([PRNoteEntity], PRTimeMap) throws -> [PRNoteEntity],
                 selection replacement: Set<UInt64>? = nil) {
        guard canPerformEdit, let map = timeMap else {
            setStatus(isTransformPreview ? "Сначала примените или отмените предпросмотр преобразования." : PREditError.unavailable.description)
            return
        }
        do { commit(try transform(store.entities, map), original: store.entities, selection: replacement) }
        catch { fail(error) }
    }

    private func commit(_ candidate: [PRNoteEntity], original: [PRNoteEntity],
                        selection replacement: Set<UInt64>? = nil) {
        guard canPerformEdit, let map = timeMap else { fail(PREditError.unavailable); return }
        do { _ = try PREdits.checked(candidate, map: map) }
        catch { fail(error); return }
        guard original == store.entities else { fail(PREditError.staleEdit); return }
        guard candidate.map(\.note) != original.map(\.note) else { setStatus("Изменений нет"); return }
        guard onCommitRequest != nil ? context != nil : onCommit != nil else {
            fail(PREditError.unavailable); return
        }
        let pending = PendingCommit(id: UUID(), candidate: candidate, original: original,
            originalSelection: selection, selection: replacement ?? selection, context: context,
            generation: generation, clipStart: map.clipStart, clipLength: map.clipLength)
        pendingCommit = pending
        status = "Применяем изменение…"
        changed()
        // onChange is user code: it may synchronously load another clip. Never
        // invoke its commit callback with the previous clip's candidate.
        guard pendingCommit?.id == pending.id, generation == pending.generation else { return }
        if let onCommitRequest, let context = pending.context {
            onCommitRequest(PRCommitRequest(context: context, clipStart: map.clipStart,
                clipLength: map.clipLength, original: original.map(\.note), notes: candidate.map(\.note)))
        } else { onCommit?(candidate.map(\.note)) }
        if pendingCommit?.id == pending.id {
            setStatus("Нет подтверждения движка · правки заблокированы до обновления клипа")
        }
    }

    func deleteSelected() {
        let ids = selection
        perform { items, _ in items.filter { !ids.contains($0.id) } }
    }

    func nudge(beats: Double = 0, semitones: Int = 0) {
        let ids = selection
        perform { items, map in try PREdits.move(items, selected: ids, beats: beats, semitones: semitones, map: map) }
    }

    func adjustVelocity(_ delta: Int) {
        let ids = selection
        perform { items, _ in PREdits.velocity(items, selected: ids, delta: delta) }
    }

    func setVelocity(_ value: Int) {
        let ids = selection
        perform { items, _ in PREdits.setVelocity(items, selected: ids, value: value) }
    }

    func setChannel(_ channel: Int) {
        guard (0...15).contains(channel) else { fail(PREditError.invalidNote); return }
        let ids = selection
        perform { items, _ in
            items.map { entity in
                guard ids.contains(entity.id) else { return entity }
                var changed = entity
                changed.note.channel = UInt8(channel)
                return changed
            }
        }
    }

    func quantize() {
        let ids = selection, currentGrid = grid, strength = quantizeStrength, ppb = pixelsPerBeat
        perform { items, map in
            try PREdits.quantize(items, selected: ids, grid: currentGrid,
                                 pointsPerBeat: ppb, strength: strength, map: map)
        }
    }

    func legato() {
        let ids = selection
        perform { items, map in try PREdits.legato(items, selected: ids, map: map) }
    }

    func humanize(seed: UInt64 = UInt64.random(in: 1...UInt64.max)) {
        let ids = selection, timing = humanizeTimingBeats, velocity = humanizeVelocity
        perform { items, map in
            try PREdits.humanize(items, selected: ids, timeBeats: timing,
                                 velocityAmount: velocity, seed: seed, map: map)
        }
    }

    func snapSelectionToScale() {
        let ids = selection, current = scale
        perform { items, _ in PREdits.snapPitch(items, selected: ids, scale: current) }
    }

    func reverseSelection() {
        let ids = selection
        perform { items, map in try PRHarmony.reverse(items, selected: ids, map: map) }
    }

    func ratchet(count: Int, gate: Double) {
        guard canPerformEdit, let map = timeMap else { fail(PREditError.unavailable); return }
        let ids = selection
        do {
            let result = try PRTransforms.ratchet(store.entities, selected: ids, count: count,
                                                  gate: gate, nextID: nextID, map: map)
            commit(result.entities, original: store.entities, selection: result.selection)
        } catch { fail(error) }
    }

    func strum(spreadBeats: Double, descending: Bool) {
        guard canPerformEdit, let map = timeMap else { fail(PREditError.unavailable); return }
        let ids = selection
        do {
            let result = try PRTransforms.strum(store.entities, selected: ids,
                                                spreadBeats: spreadBeats,
                                                descending: descending, map: map)
            commit(result.entities, original: store.entities, selection: result.selection)
        } catch { fail(error) }
    }

    func velocityRamp(from: Int, to: Int) {
        guard canPerformEdit, let map = timeMap else { fail(PREditError.unavailable); return }
        do {
            let result = try PRTransforms.velocityRamp(store.entities, selected: selection,
                                                       from: from, to: to, map: map)
            commit(result.entities, original: store.entities, selection: result.selection)
        } catch { fail(error) }
    }

    func insertChord(root: Int, kind: PRChordKind, inversion: Int) {
        guard canPerformEdit, let map = timeMap else { fail(PREditError.unavailable); return }
        do {
            let inserted = try PRHarmony.chord(root: root, kind: kind, inversion: inversion,
                                               start: insertionBeat,
                                               length: defaultLengthBeats,
                                               channel: defaultChannel,
                                               velocity: defaultVelocity,
                                               nextID: nextID, map: map)
            commit(store.entities + inserted, original: store.entities, selection: Set(inserted.map(\.id)))
        } catch { fail(error) }
    }

    func chordCandidates() -> [String] {
        PRHarmony.names(pitches: selectedEntities.map(\.note.pitch))
    }
}
