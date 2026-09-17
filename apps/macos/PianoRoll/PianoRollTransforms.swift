import Foundation

/// MIDI-note transformations only. Preview never creates a second persisted
/// document, CC lane or audio source. Every operation is atomic and selection-only.
enum PRTransforms {
    private static func validate(_ entities: [PRNoteEntity], map: PRTimeMap) throws {
        try PREdits.validate(entities.map(\.note), clipLength: map.clipLength)
        guard Set(entities.map(\.id)).count == entities.count else { throw PREditError.invalidNote }
    }
    struct Result {
        var entities: [PRNoteEntity]
        var selection: Set<UInt64>
    }

    static func velocityRamp(_ entities: [PRNoteEntity], selected: Set<UInt64>,
                             from: Int, to: Int, map: PRTimeMap) throws -> Result {
        try validate(entities, map: map)
        guard (1...127).contains(from), (1...127).contains(to) else { throw PREditError.invalidNote }
        let picked = entities.filter { selected.contains($0.id) }
        guard let first = picked.map({ map.start($0.note) }).min(),
              let last = picked.map({ map.start($0.note) }).max() else {
            return Result(entities: entities, selection: selected)
        }
        let result = entities.map { entity in
            guard selected.contains(entity.id) else { return entity }
            let position = last > first ? (map.start(entity.note) - first) / (last - first) : 0
            var item = entity
            item.note.velocity = UInt8(min(127, max(1, Int((Double(from) + Double(to - from) * position).rounded()))))
            return item
        }
        return Result(entities: try PREdits.checked(result, map: map), selection: selected)
    }

    /// Equal musical subdivisions. The first part retains the original note ID;
    /// later parts get fresh IDs. At gate=1 the segments tile the original exactly.
    static func ratchet(_ entities: [PRNoteEntity], selected: Set<UInt64>, count: Int,
                        gate: Double, nextID: UInt64, map: PRTimeMap) throws -> Result {
        try validate(entities, map: map)
        guard (1...32).contains(count), gate.isFinite, (0.05...1).contains(gate) else { throw PREditError.invalidNote }
        let picked = entities.filter { selected.contains($0.id) }
        let extra = picked.count * (count - 1)
        guard extra <= PRLimits.noteCount - entities.count else { throw PREditError.tooManyNotes }
        if extra > 0 {
            guard nextID > (entities.map(\.id).max() ?? 0), nextID <= UInt64.max - UInt64(extra) else { throw PREditError.invalidNote }
        }
        if count == 1 && gate == 1 { return Result(entities: entities, selection: selected) }
        var output: [PRNoteEntity] = [], ids = Set<UInt64>(), freshID = nextID
        output.reserveCapacity(entities.count + extra)
        for entity in entities {
            guard selected.contains(entity.id) else { output.append(entity); continue }
            let note = entity.note, start = map.start(note), length = map.length(note)
            for part in 0..<count {
                let first = part == 0 ? note.startFrames : map.frame(at: start + length * Double(part) / Double(count))
                let boundary = part + 1 == count ? note.startFrames + note.lengthFrames
                    : map.frame(at: start + length * Double(part + 1) / Double(count))
                guard boundary > first else { throw PREditError.invalidNote }
                let gateEnd = gate == 1 ? boundary : map.frame(at: start + length * (Double(part) + gate) / Double(count))
                let end = min(boundary, max(first + 1, gateEnd))
                var value = note; value.startFrames = first; value.lengthFrames = end - first
                let id: UInt64
                if part == 0 { id = entity.id } else { id = freshID; freshID += 1 }
                output.append(PRNoteEntity(id: id, note: value)); ids.insert(id)
            }
        }
        return Result(entities: try PREdits.checked(output, map: map), selection: ids)
    }

    /// Spread simultaneous selected voices of each channel over spreadBeats.
    /// Repeated occurrences of one pitch stay together. Unselected voices remain
    /// bit-exact. Reject the whole edit when a voice cannot fit, never drop it.
    static func strum(_ entities: [PRNoteEntity], selected: Set<UInt64>, spreadBeats: Double,
                      descending: Bool, map: PRTimeMap) throws -> Result {
        try validate(entities, map: map)
        guard spreadBeats.isFinite, (0...4).contains(spreadBeats) else { throw PREditError.invalidNote }
        guard spreadBeats > 0 else { return Result(entities: entities, selection: selected) }
        struct Group: Hashable { var start: UInt64; var channel: UInt8 }
        var pitches: [Group: Set<UInt8>] = [:]
        for item in entities where selected.contains(item.id) {
            pitches[Group(start: item.note.startFrames, channel: item.note.channel), default: []].insert(item.note.pitch)
        }
        let ranks = pitches.mapValues { values in
            let ordered = values.sorted { descending ? $0 > $1 : $0 < $1 }
            return Dictionary(uniqueKeysWithValues: ordered.enumerated().map { ($0.element, $0.offset) })
        }
        let output = try entities.map { item -> PRNoteEntity in
            guard selected.contains(item.id) else { return item }
            let note = item.note, key = Group(start: item.note.startFrames, channel: item.note.channel)
            guard let group = ranks[key], group.count > 1, let rank = group[note.pitch], rank > 0 else { return item }
            let delay = spreadBeats * Double(rank) / Double(group.count - 1)
            return PRNoteEntity(id: item.id, note: try map.note(start: map.start(note) + delay,
                end: map.end(note) + delay, pitch: note.pitch, channel: note.channel, velocity: note.velocity))
        }
        return Result(entities: try PREdits.checked(output, map: map), selection: selected)
    }
}
