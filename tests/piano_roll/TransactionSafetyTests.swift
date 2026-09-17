import Foundation

@main struct TransactionSafetyTests {
    @MainActor static func main() throws {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ label: String) {
            guard condition() else {
                FileHandle.standardError.write(Data("FAIL: \(label)\n".utf8)); exit(1)
            }
            checks += 1
            FileHandle.standardOutput.write(Data("PASS \(label)\n".utf8))
        }
        func rejects(_ label: String, _ body: () throws -> Void) {
            do { try body(); check(false, label) } catch { check(true, label) }
        }
        let map = try PRTimeMap(clipStart: 0, clipLength: 384_000,
            toBeat: { Double($0) / 24_000 }, toFrame: { UInt64(($0 * 24_000).rounded()) })
        let notes = [60, 64, 67].map { PianoRollNote(startFrames: 24_000, lengthFrames: 24_000,
            pitch: UInt8($0), channel: 0, velocity: 90) }
        let binding = PRClipContext(documentID: UUID(), trackID: 7, clipIndex: 0, revision: 5)
        let state = PRProState()
        var writes = 0
        state.onCommit = { _ in writes += 1 }
        state.receive(notes: notes, map: map, editable: true)
        state.selectAll()
        let oldSelection = state.selection
        state.ratchet(count: 2, gate: 1)
        check(writes == 1 && state.awaitingCommit && !state.editable, "missing echo freezes editor after one write")
        check(state.entities.map(\.note) == notes, "unconfirmed write never replaces authoritative store")
        state.insertChord(root: 60, kind: .major, inversion: 0)
        state.strum(spreadBeats: 0.5, descending: false)
        state.velocityRamp(from: 10, to: 90)
        state.nudge(beats: 1)
        check(writes == 1, "all mutation paths respect pending-host barrier")
        state.receive(notes: notes, map: map, editable: true)
        check(state.editable && !state.awaitingCommit, "authoritative refresh releases barrier")
        check(state.selection == oldSelection, "rejected ratchet restores original selection IDs")

        state.onCommit = { _ in
            writes += 1
            state.receive(notes: notes, map: map, editable: true)
        }
        let before = writes
        state.deleteSelected()
        check(writes == before + 1 && state.entities.map(\.note) == notes, "rejected deletion restores original notes")
        check(state.selection == oldSelection, "rejected deletion retains whole selection")
        check(state.status.contains("отклонено"), "reject does not advertise success")

        let noHost = PRProState()
        noHost.receive(notes: notes, map: map, editable: true)
        noHost.selectAll(); noHost.nudge(beats: 1)
        check(noHost.entities.map(\.note) == notes && !noHost.awaitingCommit, "missing callback refuses without optimistic data")

        let blocked: [(String, (PRProState) -> Void)] = [
            ("ratchet", { $0.ratchet(count: 2, gate: 1) }),
            ("strum", { $0.strum(spreadBeats: 0.5, descending: false) }),
            ("ramp", { $0.velocityRamp(from: 10, to: 90) }),
            ("chord", { $0.insertChord(root: 72, kind: .minor7, inversion: 1) }),
            ("delete", { $0.deleteSelected() }), ("move", { $0.nudge(beats: 1) })]
        for (label, edit) in blocked {
            let s = PRProState(); var calls = 0
            s.onCommit = { _ in calls += 1 }
            s.receive(notes: notes, map: map, editable: false); s.selectAll()
            let selected = s.selection
            edit(s)
            check(calls == 0 && s.entities.map(\.note) == notes && s.selection == selected, "disabled \(label) is atomic refusal")
            s.receive(notes: notes, map: map, editable: true); s.selectAll()
            s.previewRatchet(count: 2, gate: 1)
            let candidate = s.entities
            edit(s)
            check(calls == 0 && s.entities == candidate && s.isTransformPreview, "\(label) cannot bypass active preview")
        }

        let tokens = PRProState()
        tokens.receive(notes: notes, map: map, editable: true); tokens.selectAll()
        check(tokens.beginGesture(), "first pointer gesture starts")
        let oldToken = tokens.gesture!.id
        tokens.cancelGesture(transaction: oldToken)
        tokens.previewRatchet(count: 3, gate: 1)
        let newToken = tokens.gesture!.id, candidate = tokens.entities
        tokens.finishGesture(transaction: oldToken)
        tokens.cancelGesture(transaction: oldToken)
        tokens.previewGesture([], transaction: oldToken)
        tokens.invalidateGesture(PREditError.invalidNote, transaction: oldToken)
        check(tokens.gesture?.id == newToken && tokens.entities == candidate, "late events cannot finish cancel or replace newer transaction")
        tokens.cancelGesture(transaction: newToken)
        tokens.beginGesture()
        let validToken = tokens.gesture!.id
        tokens.previewGesture([], transaction: validToken)
        tokens.invalidateGesture(PREditError.invalidNote, transaction: validToken)
        tokens.finishGesture(transaction: validToken)
        check(tokens.entities.map(\.note) == notes && !tokens.awaitingCommit, "invalid latest pointer cannot commit older candidate")

        let bound = PRProState()
        var captured: PRCommitRequest?
        bound.receive(notes: notes, map: map, editable: true, context: binding); bound.selectAll()
        let boundIDs = bound.selection
        bound.onCommitRequest = { request in
            captured = request
            var next = request.context; next.revision += 1
            bound.receive(notes: request.notes, map: map, editable: true, context: next)
        }
        bound.nudge(beats: 0.25, semitones: 1)
        check(captured?.context == binding && captured?.original == notes, "typed request captures original clip and revision")
        check(bound.selection == boundIDs && bound.context?.revision == 6, "accepted revision echo preserves selection")
        check(bound.status.contains("применено"), "typed echo confirms actual write")
        bound.beginGesture()
        let stableToken = bound.gesture!.id
        bound.receive(notes: bound.entities.map(\.note), map: map, editable: true, context: bound.context)
        check(bound.gesture?.id == stableToken, "same-revision refresh does not cancel pointer")
        var changedRevision = bound.context!; changedRevision.revision += 1
        bound.receive(notes: bound.entities.map(\.note), map: map, editable: true, context: changedRevision)
        check(!bound.isGesturing, "new revision cancels pointer even when notes unchanged")
        var other = changedRevision; other.documentID = UUID()
        bound.receive(notes: bound.entities.map(\.note), map: map, editable: true, context: other)
        check(bound.selection.isEmpty, "different document cannot inherit note selection")

        let reentrant = PRProState()
        reentrant.receive(notes: notes, map: map, editable: true, context: binding); reentrant.selectAll()
        var callbacks = 0, switched = false
        reentrant.onCommitRequest = { _ in callbacks += 1 }
        reentrant.onChange = {
            if reentrant.awaitingCommit && !switched {
                switched = true
                reentrant.receive(notes: notes, map: map, editable: true, context: other)
            }
        }
        reentrant.nudge(beats: 1)
        check(callbacks == 0 && reentrant.context == other, "reentrant clip switch prevents stale callback invocation")

        let entities = notes.enumerated().map { PRNoteEntity(id: UInt64($0.offset + 1), note: $0.element) }
        rejects("checked rejects duplicate IDs") { _ = try PREdits.checked([entities[0], entities[0]], map: map) }
        rejects("checked rejects zero ID") { _ = try PREdits.checked([PRNoteEntity(id: 0, note: notes[0])], map: map) }
        rejects("split rejects ID exhaustion") { _ = try PREdits.split(entities, selected: [1], at: 36_000, nextID: UInt64.max, map: map) }
        rejects("split rejects colliding IDs") { _ = try PREdits.split(entities, selected: [1], at: 36_000, nextID: 2, map: map) }
        let clipboard = PRClipboard(entities: entities, selected: [1, 2], map: map)
        rejects("paste rejects ID exhaustion") { _ = try clipboard.inserted(into: entities, at: 4, nextID: UInt64.max, map: map) }
        rejects("paste rejects colliding IDs") { _ = try clipboard.inserted(into: entities, at: 4, nextID: 2, map: map) }
        var exhausted = PRNoteStore()
        exhausted.replace(with: notes, preferring: [PRNoteEntity(id: UInt64.max, note: notes[0])])
        check(Set(exhausted.entities.map(\.id)).count == notes.count && exhausted.nextID > 0, "store safely resets exhausted ephemeral identities")

        let full = PRProState(); var overBudgetWrites = 0
        full.receive(notes: Array(repeating: notes[0], count: PRLimits.noteCount), map: map, editable: true)
        full.onCommit = { _ in overBudgetWrites += 1 }
        full.insertChord(root: 60, kind: .major7, inversion: 0)
        check(overBudgetWrites == 0 && full.entities.count == PRLimits.noteCount, "chord stamp validates combined project-sized candidate")
        print("RESULT \(checks) transaction-safety checks passed")
    }
}
