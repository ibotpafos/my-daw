import Foundation

@MainActor
final class PRProState {
    enum GestureSource { case pointer, transform }

    struct Gesture {
        var original: [PRNoteEntity]
        var originalSelection: Set<UInt64>
        var generation: UInt64
        var source: GestureSource
    }

    private var store = PRNoteStore()
    private var pendingCommit: [PRNoteEntity]?
    private(set) var gesture: Gesture?
    private(set) var preview: [PRNoteEntity]?
    private(set) var generation: UInt64 = 0
    private(set) var editable = false
    private(set) var index = PRNoteIndex([])

    var timeMap: PRTimeMap?
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

    var onCommit: (([PianoRollNote]) -> Void)?
    var onChange: (() -> Void)?

    var entities: [PRNoteEntity] { preview ?? store.entities }
    var selectedEntities: [PRNoteEntity] { entities.filter { selection.contains($0.id) } }
    var nextID: UInt64 { max(store.nextID, (entities.map(\.id).max() ?? 0) + 1) }
    var isGesturing: Bool { gesture != nil }
    var isTransformPreview: Bool { gesture?.source == .transform }
    var pitchRows: PRPitchRows {
        PRPitchRows(used: fold ? Set(store.entities.map { Int($0.note.pitch) }) : nil)
    }

    func receive(notes: [PianoRollNote], map: PRTimeMap?, editable: Bool) {
        if gesture != nil {
            gesture = nil
            preview = nil
            status = "Проект изменился во время жеста — жест отменён."
        }
        let preferred = pendingCommit ?? store.entities
        store.replace(with: notes, preferring: preferred)
        pendingCommit = nil
        timeMap = map
        self.editable = editable && map != nil
        selection.formIntersection(Set(store.entities.map(\.id)))
        index = PRNoteIndex(store.entities)
        generation &+= 1
        changed()
    }

    func changed() { onChange?() }

    func setStatus(_ value: String) {
        status = value
        changed()
    }

    func fail(_ error: Error) {
        status = (error as? PREditError)?.description ?? String(describing: error)
        changed()
    }

    func selectAll() {
        selection = Set(entities.map(\.id))
        status = "Выбрано нот: \(selection.count)"
        changed()
    }

    func clearSelection() {
        selection.removeAll()
        changed()
    }

    @discardableResult
    func beginGesture(source: GestureSource = .pointer) -> Bool {
        if let active = gesture {
            status = active.source == .transform
                ? "Сначала примените или отмените предпросмотр преобразования."
                : "Сначала завершите текущий жест редактирования."
            changed()
            return false
        }
        guard editable, timeMap != nil else {
            fail(PREditError.unavailable)
            return false
        }
        gesture = Gesture(original: store.entities,
                          originalSelection: selection,
                          generation: generation,
                          source: source)
        preview = nil
        return true
    }

    func previewGesture(_ candidate: [PRNoteEntity]) {
        guard gesture != nil, let map = timeMap else { return }
        do {
            preview = try PREdits.checked(candidate, map: map)
            changed()
        } catch {
            // Invalid latest pointer position must not leave an older valid preview
            // waiting to be committed on mouse-up.
            preview = nil
            fail(error)
        }
    }

    func cancelGesture() {
        if let gesture { selection = gesture.originalSelection }
        gesture = nil
        preview = nil
        status = "Жест отменён"
        changed()
    }

    func finishGesture() {
        guard let gesture else { return }
        let candidate = preview
        self.gesture = nil
        preview = nil
        guard generation == gesture.generation else {
            selection = gesture.originalSelection
            fail(PREditError.staleEdit)
            return
        }
        guard let candidate else {
            selection = gesture.originalSelection
            changed()
            return
        }
        commit(candidate, original: gesture.original)
    }

    func perform(_ transform: ([PRNoteEntity], PRTimeMap) throws -> [PRNoteEntity],
                 selection replacement: Set<UInt64>? = nil) {
        guard editable, gesture == nil, let map = timeMap else {
            if isTransformPreview {
                status = "Сначала примените или отмените предпросмотр преобразования."
                changed()
            } else {
                fail(PREditError.unavailable)
            }
            return
        }
        do {
            let candidate = try PREdits.checked(transform(store.entities, map), map: map)
            if let replacement { selection = replacement }
            commit(candidate, original: store.entities)
        } catch { fail(error) }
    }

    private func commit(_ candidate: [PRNoteEntity], original: [PRNoteEntity]) {
        guard candidate.map(\.note) != original.map(\.note) else {
            status = "Изменений нет"
            changed()
            return
        }
        pendingCommit = candidate
        store.replace(with: candidate.map(\.note), preferring: candidate)
        index = PRNoteIndex(store.entities)
        selection.formIntersection(Set(store.entities.map(\.id)))
        status = "Изменение применено · ⌘Z — отменить в проекте"
        changed()
        onCommit?(candidate.map(\.note))
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
        guard let map = timeMap else { fail(PREditError.unavailable); return }
        let ids = selection
        do {
            let result = try PRTransforms.ratchet(store.entities, selected: ids, count: count,
                                                  gate: gate, nextID: nextID, map: map)
            selection = result.selection
            commit(result.entities, original: store.entities)
        } catch { fail(error) }
    }

    func strum(spreadBeats: Double, descending: Bool) {
        guard let map = timeMap else { fail(PREditError.unavailable); return }
        let ids = selection
        do {
            let result = try PRTransforms.strum(store.entities, selected: ids,
                                                spreadBeats: spreadBeats,
                                                descending: descending, map: map)
            selection = result.selection
            commit(result.entities, original: store.entities)
        } catch { fail(error) }
    }

    func velocityRamp(from: Int, to: Int) {
        guard let map = timeMap else { fail(PREditError.unavailable); return }
        do {
            let result = try PRTransforms.velocityRamp(store.entities, selected: selection,
                                                       from: from, to: to, map: map)
            selection = result.selection
            commit(result.entities, original: store.entities)
        } catch { fail(error) }
    }

    func insertChord(root: Int, kind: PRChordKind, inversion: Int) {
        guard let map = timeMap else { fail(PREditError.unavailable); return }
        do {
            let inserted = try PRHarmony.chord(root: root, kind: kind, inversion: inversion,
                                               start: insertionBeat,
                                               length: defaultLengthBeats,
                                               channel: defaultChannel,
                                               velocity: defaultVelocity,
                                               nextID: nextID, map: map)
            selection = Set(inserted.map(\.id))
            commit(store.entities + inserted, original: store.entities)
        } catch { fail(error) }
    }

    func chordCandidates() -> [String] {
        PRHarmony.names(pitches: selectedEntities.map(\.note.pitch))
    }
}
