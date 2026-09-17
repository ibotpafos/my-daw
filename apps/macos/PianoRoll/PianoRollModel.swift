import Foundation

// The bridge remains the only persisted model. These are clip-relative 48 kHz
// frames, not samples at the device rate and not a second MIDI document.
struct PianoRollNote: Equatable, Hashable, Codable, Sendable {
    var startFrames: UInt64 = 0
    var lengthFrames: UInt64 = 480
    var pitch: UInt8 = 60
    var channel: UInt8 = 0
    var velocity: UInt8 = 100
}

struct PianoRollClipModel: Equatable, Hashable, Sendable {
    var index: Int = 0
    var startFrames: UInt64 = 0
    var lengthFrames: UInt64 = 0
    var noteCount: UInt32 = 0
    var title: String {
        let end = startFrames.addingReportingOverflow(lengthFrames)
        return "Клип \(index + 1) · \(Self.seconds(startFrames))–\(Self.seconds(end.overflow ? UInt64.max : end.partialValue)) с · \(noteCount) нот"
    }
    private static func seconds(_ frames: UInt64) -> String {
        String(format: "%.1f", Double(frames) / 48_000)
    }
}

enum PRLimits {
    static let timelineFrames: UInt64 = 1 << 40
    static let noteFrames: UInt64 = 480_000
    static let noteCount = 65_536
    static let clipboardBytes = 16 * 1_024 * 1_024
}

enum PREditError: Error, Equatable, CustomStringConvertible {
    case invalidClip, invalidNote, noteTooLong, outsideClip, tooManyNotes
    case invalidClipboard, staleEdit, unavailable
    var description: String {
        switch self {
        case .invalidClip: return "Некорректные границы MIDI-клипа."
        case .invalidNote: return "Проверьте высоту, канал, velocity и длину ноты."
        case .noteTooLong: return "Движок пока ограничивает одну ноту 10 секундами. Изменение не применено."
        case .outsideClip: return "Ноты не помещаются в клип. Сначала увеличьте его длину."
        case .tooManyNotes: return "Превышен лимит 65 536 нот; изменение не применено."
        case .invalidClipboard: return "Буфер не содержит поддерживаемых нот My DAW."
        case .staleEdit: return "Проект изменился во время жеста. Жест отменён; повторите изменение."
        case .unavailable: return "Редактирование недоступно: выберите MIDI-клип и остановите запись."
        }
    }
}

struct PRNoteEntity: Equatable, Sendable {
    let id: UInt64                 // Ephemeral UI identity; never serialized to the bridge.
    var note: PianoRollNote
}

/// The binding is ephemeral. A new/opened document gets a fresh UUID even
/// when its saved revision, track IDs and MIDI notes happen to be identical.
struct PRClipContext: Equatable, Sendable {
    var documentID: UUID
    var trackID: UInt64
    var clipIndex: Int
    var revision: UInt64

    func sameClip(as other: PRClipContext) -> Bool {
        documentID == other.documentID && trackID == other.trackID && clipIndex == other.clipIndex
    }
}

struct PRCommitRequest: Sendable {
    var context: PRClipContext
    var clipStart: UInt64
    var clipLength: UInt64
    var original: [PianoRollNote]
    var notes: [PianoRollNote]
}

struct PRNoteStore {
    private(set) var entities: [PRNoteEntity] = []
    private(set) var nextID: UInt64 = 1

    init(notes: [PianoRollNote] = []) { replace(with: notes) }

    // Match value AND occurrence: identical overlapping notes are distinct.
    // Exhaustion is an explicit ID-space reset, never a trapping increment.
    mutating func replace(with notes: [PianoRollNote], preferring preferred: [PRNoteEntity]? = nil) {
        let prior = preferred ?? entities
        let greatest = prior.map(\.id).max() ?? 0
        let unique = Set(prior.map(\.id))
        if greatest >= UInt64.max - UInt64(notes.count) ||
            nextID >= UInt64.max - UInt64(notes.count) ||
            unique.count != prior.count || unique.contains(0) {
            entities = notes.enumerated().map { PRNoteEntity(id: UInt64($0.offset) + 1, note: $0.element) }
            nextID = UInt64(notes.count) + 1
            return
        }
        var buckets: [PianoRollNote: [UInt64]] = [:]
        for entity in prior.reversed() { buckets[entity.note, default: []].append(entity.id) }
        nextID = max(nextID, greatest + 1)
        entities = notes.map { note in
            if let id = buckets[note]?.popLast() { return PRNoteEntity(id: id, note: note) }
            defer { nextID += 1 }
            return PRNoteEntity(id: nextID, note: note)
        }
    }
}

enum PRPitch {
    static let names = ["C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"]
    static func name(_ pitch: UInt8) -> String {
        guard pitch <= 127 else { return "?" }
        return names[Int(pitch) % 12] + String(Int(pitch) / 12 - 1)
    }
    static func parse(_ input: String) -> UInt8? {
        let raw = input.trimmingCharacters(in: .whitespacesAndNewlines)
        if let number = Int(raw), (0...127).contains(number) { return UInt8(number) }
        let text = raw.replacingOccurrences(of: "♯", with: "#")
            .replacingOccurrences(of: "♭", with: "b").uppercased()
        let chars = Array(text)
        guard let first = chars.first,
              var semitone = [Character("C"): 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11][first]
        else { return nil }
        var offset = 1
        if chars.count > offset, chars[offset] == "#" { semitone += 1; offset += 1 }
        else if chars.count > offset, chars[offset] == "B" { semitone -= 1; offset += 1 }
        guard chars.count > offset, let octave = Int(String(chars[offset...])), (-2...10).contains(octave) else { return nil }
        let pitch = (octave + 1) * 12 + semitone
        return (0...127).contains(pitch) ? UInt8(pitch) : nil
    }
    static func isBlack(_ pitch: Int) -> Bool { [1, 3, 6, 8, 10].contains(pitch % 12) }
}

enum PRScaleKind: Int, CaseIterable, Sendable {
    case chromatic, major, minor, harmonicMinor, dorian, minorPentatonic
    var title: String {
        ["Хроматическая", "Мажор", "Минор", "Гармонический минор", "Дорийская", "Минорная пентатоника"][rawValue]
    }
    var intervals: Set<Int> {
        switch self {
        case .chromatic: return Set(0..<12)
        case .major: return [0, 2, 4, 5, 7, 9, 11]
        case .minor: return [0, 2, 3, 5, 7, 8, 10]
        case .harmonicMinor: return [0, 2, 3, 5, 7, 8, 11]
        case .dorian: return [0, 2, 3, 5, 7, 9, 10]
        case .minorPentatonic: return [0, 3, 5, 7, 10]
        }
    }
}

struct PRScale: Equatable, Sendable {
    var root = 0
    var kind: PRScaleKind = .chromatic
    func contains(_ pitch: Int) -> Bool { kind.intervals.contains(((pitch - root) % 12 + 12) % 12) }
    func nearest(_ pitch: Int) -> UInt8 {
        let bounded = min(127, max(0, pitch))
        for delta in 0...12 {
            if bounded - delta >= 0, contains(bounded - delta) { return UInt8(bounded - delta) }
            if bounded + delta <= 127, contains(bounded + delta) { return UInt8(bounded + delta) }
        }
        return UInt8(bounded)
    }
}

enum PRTool: Int, CaseIterable {
    case select, draw, erase, split, velocity, hand
    var title: String { ["Выбор", "Нота", "Ластик", "Разрез", "Velocity", "Рука"][rawValue] }
    var shortcut: String { ["V", "B", "E", "S", "Y", "H"][rawValue] }
    var symbol: String { ["cursorarrow", "pencil", "eraser", "scissors", "slider.vertical.3", "hand.draw"][rawValue] }
}
