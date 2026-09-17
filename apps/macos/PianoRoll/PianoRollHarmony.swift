import Foundation

enum PRChordKind: Int, CaseIterable {
    case major, minor, diminished, augmented, sus2, sus4, major7, minor7, dominant7

    var title: String {
        ["Major", "Minor", "Dim", "Aug", "Sus2", "Sus4", "Maj7", "Min7", "7"][rawValue]
    }

    var intervals: [Int] {
        switch self {
        case .major: return [0, 4, 7]
        case .minor: return [0, 3, 7]
        case .diminished: return [0, 3, 6]
        case .augmented: return [0, 4, 8]
        case .sus2: return [0, 2, 7]
        case .sus4: return [0, 5, 7]
        case .major7: return [0, 4, 7, 11]
        case .minor7: return [0, 3, 7, 10]
        case .dominant7: return [0, 4, 7, 10]
        }
    }
}

enum PRHarmony {
    static func chord(root: Int, kind: PRChordKind, inversion: Int,
                      start: Double, length: Double, channel: UInt8, velocity: UInt8,
                      nextID: UInt64, map: PRTimeMap) throws -> [PRNoteEntity] {
        guard (0...127).contains(root),
              (0..<kind.intervals.count).contains(inversion),
              length.isFinite, length > 0,
              nextID <= UInt64.max - UInt64(kind.intervals.count) else {
            throw PREditError.invalidNote
        }
        let pitches = kind.intervals.enumerated().map { index, interval in
            root + interval + (index < inversion ? 12 : 0)
        }.sorted()
        guard pitches.allSatisfy({ (0...127).contains($0) }) else { throw PREditError.invalidNote }
        return try pitches.enumerated().map { index, pitch in
            PRNoteEntity(id: nextID + UInt64(index), note: try map.note(
                start: start, end: start + length, pitch: UInt8(pitch),
                channel: channel, velocity: velocity))
        }
    }

    /// Candidate labels only. The editor does not claim harmonic-function analysis.
    static func names(pitches: [UInt8]) -> [String] {
        guard let bass = pitches.min(), !pitches.isEmpty else { return [] }
        let classes = Set(pitches.map { Int($0) % 12 })
        let bassClass = Int(bass) % 12
        let roots = [bassClass] + (0..<12).filter { $0 != bassClass }
        var result: [String] = []
        for root in roots {
            for kind in PRChordKind.allCases {
                let wanted = Set(kind.intervals.map { ($0 + root) % 12 })
                guard wanted == classes else { continue }
                let slash = root == bassClass ? "" : "/" + PRPitch.names[bassClass]
                result.append(PRPitch.names[root] + " " + kind.title + slash)
            }
        }
        return result
    }

    /// Mirror selected notes around the selected phrase span in musical time.
    /// Pitch, channel and velocity remain unchanged and IDs are stable.
    static func reverse(_ entities: [PRNoteEntity], selected: Set<UInt64>, map: PRTimeMap) throws -> [PRNoteEntity] {
        let chosen = entities.filter { selected.contains($0.id) }
        guard let first = chosen.map({ map.start($0.note) }).min(),
              let last = chosen.map({ map.end($0.note) }).max() else { return entities }
        return try PREdits.checked(try entities.map { entity in
            guard selected.contains(entity.id) else { return entity }
            let start = first + last - map.end(entity.note)
            let end = first + last - map.start(entity.note)
            var result = entity
            result.note = try map.note(start: start, end: end,
                                       pitch: entity.note.pitch,
                                       channel: entity.note.channel,
                                       velocity: entity.note.velocity)
            return result
        }, map: map)
    }
}
