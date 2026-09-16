import Foundation

@main struct ProStateCoreTests {
    @MainActor static func main() throws {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            precondition(condition(), name)
            checks += 1
            print("PASS \(name)")
        }
        let map = try PRTimeMap(clipStart: 100_000, clipLength: 384_000,
            toBeat: { Double($0) / 24_000 },
            toFrame: { UInt64(($0 * 24_000).rounded()) })
        var persisted = [
            PianoRollNote(startFrames: 24_000, lengthFrames: 24_000, pitch: 60, channel: 0, velocity: 80),
            PianoRollNote(startFrames: 48_000, lengthFrames: 24_000, pitch: 64, channel: 0, velocity: 90),
            PianoRollNote(startFrames: 72_000, lengthFrames: 24_000, pitch: 67, channel: 0, velocity: 100)]
        let originalFixture = persisted
        let state = PRProState()
        var commits = 0
        state.onCommit = { notes in
            commits += 1
            persisted = notes
            state.receive(notes: notes, map: map, editable: true)
        }
        state.receive(notes: persisted, map: map, editable: true)
        check(state.entities.count == 3 && state.editable, "receive authoritative snapshot")
        state.selectAll()
        let originalIDs = state.selection
        state.nudge(beats: 0.25, semitones: 2)
        check(commits == 1, "nudge is one host commit")
        check(state.selection == originalIDs, "accepted echo preserves IDs")
        check(state.status.contains("применено"), "accepted bridge echo confirms commit status")
        check(state.entities.map(\.note.pitch) == [62, 66, 69], "nudge transposes group")
        check(state.entities.map(\.note.startFrames) == [30_000, 54_000, 78_000], "nudge moves in musical time")

        state.setVelocity(127)
        check(commits == 2 && state.entities.allSatisfy { $0.note.velocity == 127 }, "set velocity selection commit")
        state.setChannel(15)
        check(commits == 3 && state.entities.allSatisfy { $0.note.channel == 15 }, "set channel selection commit")
        state.setChannel(16)
        check(commits == 3, "invalid channel does not commit")

        state.selection = [state.entities[0].id]
        check(state.beginGesture(), "begin gesture")
        let moved = try PREdits.move(state.entities, selected: state.selection, beats: 1, semitones: 0, map: map)
        state.previewGesture(moved)
        check(state.preview != nil, "valid gesture preview visible")
        let invalid = state.entities.map { entity -> PRNoteEntity in
            guard state.selection.contains(entity.id) else { return entity }
            var changed = entity
            changed.note.startFrames = map.clipLength
            return changed
        }
        state.previewGesture(invalid)
        check(state.preview == nil, "invalid latest preview clears previous candidate")
        state.finishGesture()
        check(commits == 3, "invalid final pointer position cannot commit older preview")

        state.selection = [state.entities[0].id]
        check(state.beginGesture(), "begin stale gesture")
        state.previewGesture(try PREdits.move(state.entities, selected: state.selection, beats: 0.5, semitones: 0, map: map))
        var external = persisted
        external[0].pitch = 50
        state.receive(notes: external, map: map, editable: true)
        check(!state.isGesturing && state.entities[0].note.pitch == 50, "external snapshot cancels stale gesture")
        state.finishGesture()
        check(commits == 3, "cancelled stale gesture cannot commit")

        state.selectAll()
        state.ratchet(count: 2, gate: 1)
        check(commits == 4 && state.entities.count == 6 && state.selection.count == 6, "ratchet updates selection and commits once")

        // Strum only changes simultaneous voices. Use a real chord fixture so
        // this assertion does not mistake a correct no-op for a missing commit.
        let simultaneous = [
            PianoRollNote(startFrames: 120_000, lengthFrames: 24_000, pitch: 60, channel: 0, velocity: 70),
            PianoRollNote(startFrames: 120_000, lengthFrames: 24_000, pitch: 64, channel: 0, velocity: 90),
            PianoRollNote(startFrames: 120_000, lengthFrames: 24_000, pitch: 67, channel: 0, velocity: 110)]
        state.receive(notes: simultaneous, map: map, editable: true)
        state.selectAll()
        state.strum(spreadBeats: 0.25, descending: false)
        check(commits == 5, "strum commits once on simultaneous voices")
        check(Set(state.entities.map(\.note.startFrames)).count > 1, "strum visibly spreads chord onsets")
        state.velocityRamp(from: 20, to: 110)
        check(commits == 6, "velocity ramp commits once")

        state.clearSelection()
        state.insertionBeat = 6
        state.defaultLengthBeats = 1
        state.defaultChannel = 2
        state.defaultVelocity = 99
        state.insertChord(root: 60, kind: .minor7, inversion: 1)
        check(commits == 7 && state.selection.count == 4, "chord stamp commits once and selects voices")
        check(state.selectedEntities.allSatisfy { $0.note.channel == 2 && $0.note.velocity == 99 }, "chord stamp carries default MIDI attributes")
        check(state.chordCandidates().contains(where: { $0.hasPrefix("C Minor") }), "selected chord gets candidate label")
        let beforeReverse = state.entities
        state.reverseSelection()
        check(commits == 7, "reversing simultaneous chord is no-op")
        check(state.entities == beforeReverse, "no-op reverse remains bit exact")

        let commitBeforeDisabled = commits
        state.receive(notes: persisted, map: map, editable: false)
        state.selectAll()
        state.deleteSelected()
        check(commits == commitBeforeDisabled && state.entities.map(\.note) == persisted, "disabled editor refuses mutation")

        state.receive(notes: persisted, map: nil, editable: true)
        state.selectAll()
        state.quantize()
        check(commits == commitBeforeDisabled, "missing time map refuses mutation")

        // The UI commits optimistically but the bridge remains authoritative.
        // A synchronous reject echo must restore the bridge snapshot and must
        // not leave a false "applied" status behind.
        let rejected = PRProState()
        rejected.receive(notes: originalFixture, map: map, editable: true)
        rejected.selection = [rejected.entities[0].id]
        var rejectedCalls = 0
        rejected.onCommit = { _ in
            rejectedCalls += 1
            rejected.receive(notes: originalFixture, map: map, editable: true)
        }
        rejected.nudge(beats: 1, semitones: 5)
        check(rejectedCalls == 1, "rejected edit still calls host exactly once")
        check(rejected.entities.map(\.note) == originalFixture, "rejected echo restores authoritative notes")
        check(rejected.status.contains("отклонено"), "rejected echo reports rejection instead of success")

        print("RESULT \(checks) pro-state checks passed")
    }
}