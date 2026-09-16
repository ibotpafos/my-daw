import Foundation

@main struct TransformCoreTests {
    static func main() throws {
        var count = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            precondition(condition(), name); count += 1; print("PASS \(name)")
        }
        func rejects(_ name: String, _ action: () throws -> Void) {
            do { try action(); preconditionFailure(name) } catch is PREditError { check(true, name) }
            catch { preconditionFailure("unexpected error \(error)") }
        }
        let map = try PRTimeMap(clipStart: 0, clipLength: 384_000,
            toBeat: { Double($0) / 24_000 }, toFrame: { UInt64(($0 * 24_000).rounded()) })
        let items = [
            PRNoteEntity(id: 1, note: PianoRollNote(startFrames: 24_000, lengthFrames: 24_000, pitch: 60, channel: 2, velocity: 75)),
            PRNoteEntity(id: 2, note: PianoRollNote(startFrames: 24_000, lengthFrames: 24_000, pitch: 64, channel: 2, velocity: 80)),
            PRNoteEntity(id: 3, note: PianoRollNote(startFrames: 24_000, lengthFrames: 24_000, pitch: 67, channel: 2, velocity: 90)),
            PRNoteEntity(id: 4, note: PianoRollNote(startFrames: 48_000, lengthFrames: 12_000, pitch: 72, channel: 2, velocity: 110)),
            PRNoteEntity(id: 5, note: PianoRollNote(startFrames: 96_000, lengthFrames: 24_000, pitch: 48, channel: 3, velocity: 50))]
        let all = Set(items.map(\.id))
        let ramp = try PRTransforms.velocityRamp(items, selected: all, from: 20, to: 110, map: map)
        check(ramp.entities.map(\.note.velocity) == [20, 20, 20, 50, 110], "velocity ramp uses note-on positions, not array order")
        check(ramp.entities.map(\.id) == items.map(\.id), "ramp preserves IDs")
        let descending = try PRTransforms.velocityRamp(items, selected: [1, 4, 5], from: 127, to: 1, map: map)
        check(descending.entities[0].note.velocity == 127 && descending.entities[4].note.velocity == 1, "descending ramp endpoints")
        check(descending.entities[1] == items[1] && descending.entities[2] == items[2], "unselected velocities are bit-exact")
        rejects("reject velocity zero") { _ = try PRTransforms.velocityRamp(items, selected: all, from: 0, to: 127, map: map) }
        let chopped = try PRTransforms.ratchet(items, selected: [1], count: 4, gate: 1, nextID: 6, map: map)
        let pieces = chopped.entities.filter { chopped.selection.contains($0.id) }
        check(pieces.map(\.note.startFrames) == [24_000, 30_000, 36_000, 42_000], "four repeats start on musical subdivisions")
        check(pieces.map(\.note.lengthFrames) == [6_000, 6_000, 6_000, 6_000], "gate one tiles original note exactly")
        check(pieces.map(\.id) == [1, 6, 7, 8] && chopped.entities.count == 8, "ratchet preserves first ID, allocates subsequent IDs")
        check(pieces.allSatisfy { $0.note.pitch == 60 && $0.note.channel == 2 && $0.note.velocity == 75 }, "ratchet retains MIDI attributes")
        check(chopped.entities.suffix(4) == items.suffix(4), "ratchet never changes unselected notes")
        let gated = try PRTransforms.ratchet(items, selected: [1], count: 4, gate: 0.5, nextID: 6, map: map)
        check(gated.entities.filter { gated.selection.contains($0.id) }.allSatisfy { $0.note.lengthFrames == 3_000 }, "gate creates silent gaps without note-off velocity")
        let unchanged = try PRTransforms.ratchet(items, selected: all, count: 1, gate: 1, nextID: 6, map: map)
        check(unchanged.entities == items, "one full-gate part is bit-exact no-op")
        rejects("reject zero gate") { _ = try PRTransforms.ratchet(items, selected: all, count: 4, gate: 0, nextID: 6, map: map) }
        rejects("reject invalid repeat count") { _ = try PRTransforms.ratchet(items, selected: all, count: 33, gate: 1, nextID: 6, map: map) }
        rejects("reject colliding fresh IDs") { _ = try PRTransforms.ratchet(items, selected: [1], count: 2, gate: 1, nextID: 2, map: map) }
        rejects("reject fresh ID exhaustion") { _ = try PRTransforms.ratchet(items, selected: [1], count: 4, gate: 1, nextID: UInt64.max, map: map) }
        let tiny = [PRNoteEntity(id: 1, note: PianoRollNote(startFrames: 0, lengthFrames: 1))]
        rejects("sub-frame subdivision rejects entire operation") { _ = try PRTransforms.ratchet(tiny, selected: [1], count: 2, gate: 1, nextID: 2, map: map) }
        let overflow = [PRNoteEntity(id: 1, note: PianoRollNote(startFrames: UInt64.max, lengthFrames: UInt64.max))]
        rejects("malformed timing rejects without UInt64 overflow") { _ = try PRTransforms.ratchet(overflow, selected: [1], count: 4, gate: 1, nextID: 2, map: map) }
        rejects("duplicate note IDs reject") { _ = try PRTransforms.strum([items[0], items[0]], selected: [1], spreadBeats: 0.1, descending: false, map: map) }
        let up = try PRTransforms.strum(items, selected: all, spreadBeats: 0.5, descending: false, map: map)
        check(up.entities.prefix(3).map(\.note.startFrames) == [24_000, 30_000, 36_000], "up-strum spreads chord pitches")
        check(up.entities.suffix(2) == items.suffix(2), "strum leaves separate onsets alone")
        check(up.entities.map(\.note.lengthFrames) == items.map(\.note.lengthFrames), "strum preserves lengths at constant tempo")
        let down = try PRTransforms.strum(items, selected: all, spreadBeats: 0.5, descending: true, map: map)
        check(down.entities.prefix(3).map(\.note.startFrames) == [36_000, 30_000, 24_000], "down-strum reverses pitch order")
        var otherChannel = items; otherChannel[1].note.channel = 3
        let separate = try PRTransforms.strum(otherChannel, selected: all, spreadBeats: 0.5, descending: false, map: map)
        check(separate.entities[1] == otherChannel[1], "strum groups MIDI channels separately")
        var duplicates = items; duplicates.append(PRNoteEntity(id: 10, note: items[1].note))
        let doubled = try PRTransforms.strum(duplicates, selected: all.union([10]), spreadBeats: 0.5, descending: false, map: map)
        check(doubled.entities[1].note == doubled.entities.last!.note, "duplicate occurrences of same pitch move together")
        var nearEnd = items; for i in 0..<3 { nearEnd[i].note.startFrames = 360_000 }
        rejects("strum beyond clip rejects atomically") { _ = try PRTransforms.strum(nearEnd, selected: [1,2,3], spreadBeats: 1, descending: false, map: map) }
        rejects("nonfinite strum rejects") { _ = try PRTransforms.strum(items, selected: all, spreadBeats: .nan, descending: false, map: map) }
        let tempo = try PRTimeMap(clipStart: 12_000, clipLength: 120_000,
            toBeat: { $0 <= 48_000 ? Double($0)/24_000 : 2 + Double($0-48_000)/48_000 },
            toFrame: { $0 <= 2 ? UInt64(($0*24_000).rounded()) : 48_000+UInt64((($0-2)*48_000).rounded()) })
        let crossing = [PRNoteEntity(id: 1, note: try tempo.note(start: 0.5, end: 2.5, pitch: 60, channel: 0, velocity: 100))]
        let changing = try PRTransforms.ratchet(crossing, selected: [1], count: 4, gate: 1, nextID: 2, map: tempo)
        check(changing.entities.map(\.note.lengthFrames) == [12_000, 12_000, 24_000, 24_000], "tempo change: equal beats, different frame lengths")
        check(changing.entities.reduce(UInt64(0)) { $0 + $1.note.lengthFrames } == crossing[0].note.lengthFrames, "tempo-crossing ratchet has no frame loss")
        let many = (0..<PRLimits.noteCount).map { PRNoteEntity(id: UInt64($0+1), note: tiny[0].note) }
        rejects("project-sized input cannot exceed note budget") { _ = try PRTransforms.ratchet(many, selected: [1], count: 2, gate: 1, nextID: 65_537, map: map) }
        var random = PRRandom(seed: 1234)
        for iteration in 0..<1000 {
            let length = UInt64(100 + Int((random.signedUnit()+1)*5_000))
            let item = PRNoteEntity(id: 1, note: PianoRollNote(startFrames: UInt64(iteration), lengthFrames: length))
            let n = 2 + iteration % 31
            let result = try PRTransforms.ratchet([item], selected: [1], count: n, gate: 1, nextID: 2, map: map)
            precondition(result.entities.count == n && Set(result.entities.map(\.id)).count == n)
            precondition(result.entities.first?.note.startFrames == item.note.startFrames)
            precondition(result.entities.reduce(UInt64(0)) { $0 + $1.note.lengthFrames } == length)
            for pair in zip(result.entities, result.entities.dropFirst()) {
                precondition(pair.0.note.startFrames + pair.0.note.lengthFrames == pair.1.note.startFrames)
            }
        }
        check(true, "1000 seeded subdivisions preserve all frames and unique IDs")
        print("RESULT \(count) transform checks passed; 1000 randomized partitions")
    }
}
