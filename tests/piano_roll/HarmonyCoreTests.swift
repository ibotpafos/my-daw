import Foundation

@main struct HarmonyCoreTests {
    static func main() throws {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            precondition(condition(), name)
            checks += 1
            print("PASS \(name)")
        }
        func rejects(_ name: String, _ body: () throws -> Void) {
            do { try body(); preconditionFailure(name) }
            catch is PREditError { check(true, name) }
            catch { preconditionFailure("unexpected \(error)") }
        }
        let map = try PRTimeMap(clipStart: 0, clipLength: 384_000,
            toBeat: { Double($0) / 24_000 },
            toFrame: { UInt64(($0 * 24_000).rounded()) })

        for kind in PRChordKind.allCases {
            for inversion in kind.intervals.indices {
                let notes = try PRHarmony.chord(root: 60, kind: kind, inversion: inversion,
                    start: 1, length: 2, channel: 4, velocity: 96, nextID: 100, map: map)
                check(notes.count == kind.intervals.count, "count \(kind.title) inv \(inversion)")
                check(Set(notes.map(\.id)).count == notes.count, "unique IDs \(kind.title) inv \(inversion)")
                check(notes.allSatisfy { $0.note.startFrames == 24_000 && $0.note.lengthFrames == 48_000 },
                      "timing \(kind.title) inv \(inversion)")
                check(notes.allSatisfy { $0.note.channel == 4 && $0.note.velocity == 96 },
                      "attributes \(kind.title) inv \(inversion)")
                check(PRHarmony.names(pitches: notes.map(\.note.pitch)).contains { $0.hasPrefix("C " + kind.title) },
                      "name candidate \(kind.title) inv \(inversion)")
            }
        }

        rejects("root above MIDI range") {
            _ = try PRHarmony.chord(root: 126, kind: .major7, inversion: 0,
                start: 0, length: 1, channel: 0, velocity: 100, nextID: 1, map: map)
        }
        rejects("invalid inversion") {
            _ = try PRHarmony.chord(root: 60, kind: .major, inversion: 3,
                start: 0, length: 1, channel: 0, velocity: 100, nextID: 1, map: map)
        }
        rejects("zero chord length") {
            _ = try PRHarmony.chord(root: 60, kind: .major, inversion: 0,
                start: 0, length: 0, channel: 0, velocity: 100, nextID: 1, map: map)
        }
        rejects("chord outside clip") {
            _ = try PRHarmony.chord(root: 60, kind: .minor7, inversion: 0,
                start: map.durationBeats - 0.1, length: 1, channel: 0, velocity: 100, nextID: 1, map: map)
        }

        let a = PRNoteEntity(id: 1, note: PianoRollNote(startFrames: 24_000, lengthFrames: 12_000, pitch: 60, channel: 1, velocity: 88))
        let b = PRNoteEntity(id: 2, note: PianoRollNote(startFrames: 72_000, lengthFrames: 24_000, pitch: 67, channel: 3, velocity: 111))
        let c = PRNoteEntity(id: 3, note: PianoRollNote(startFrames: 120_000, lengthFrames: 12_000, pitch: 72, channel: 2, velocity: 70))
        let original = [a, b, c]
        let reversed = try PRHarmony.reverse(original, selected: [1, 2, 3], map: map)
        let twice = try PRHarmony.reverse(reversed, selected: [1, 2, 3], map: map)
        check(twice == original, "reverse twice is exact")
        check(reversed.map(\.id) == original.map(\.id), "reverse preserves IDs")
        check(zip(reversed, original).allSatisfy { $0.note.pitch == $1.note.pitch && $0.note.channel == $1.note.channel && $0.note.velocity == $1.note.velocity },
              "reverse preserves MIDI attributes")
        let partial = try PRHarmony.reverse(original, selected: [1], map: map)
        check(partial == original, "single note reverse is no-op")
        let none = try PRHarmony.reverse(original, selected: [], map: map)
        check(none == original, "empty reverse is no-op")
        check(PRHarmony.names(pitches: []).isEmpty, "empty chord has no label")
        check(PRHarmony.names(pitches: [60, 64, 67]).contains("C Major"), "C major identified")
        check(PRHarmony.names(pitches: [64, 67, 72]).contains(where: { $0.hasPrefix("C Major/") }), "inversion slash candidate")

        print("RESULT \(checks) harmony checks passed")
    }
}
