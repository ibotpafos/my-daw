import Foundation

/// Stable identity for a note while the editor is open. The domain currently
/// stores notes positionally, so this ID is UI-only and is regenerated when a
/// clip is reloaded from the bridge.
struct PianoRollNoteID: Hashable, Comparable {
    let rawValue: Int
    static func < (lhs: Self, rhs: Self) -> Bool { lhs.rawValue < rhs.rawValue }
}

struct PianoRollEditableNote: Equatable, Hashable {
    var id: PianoRollNoteID
    var startFrames: UInt64
    var lengthFrames: UInt64
    var pitch: UInt8
    var channel: UInt8
    var velocity: UInt8

    var endFrames: UInt64 { startFrames + lengthFrames }

    init(id: PianoRollNoteID, note: PianoRollNote) {
        self.id = id
        startFrames = note.startFrames
        lengthFrames = note.lengthFrames
        pitch = note.pitch
        channel = note.channel
        velocity = note.velocity
    }

    var bridgeNote: PianoRollNote {
        PianoRollNote(startFrames: startFrames, lengthFrames: lengthFrames,
                      pitch: pitch, channel: channel, velocity: velocity)
    }
}

enum PianoRollTool: Int, CaseIterable {
    case select, draw, erase, split
    var title: String {
        switch self { case .select: return "Select"; case .draw: return "Draw"; case .erase: return "Erase"; case .split: return "Split" }
    }
    var shortcut: String {
        switch self { case .select: return "V"; case .draw: return "B"; case .erase: return "E"; case .split: return "S" }
    }
}

enum PianoRollGrid: String, CaseIterable {
    case off = "Off", whole = "1/1", half = "1/2", quarter = "1/4", eighth = "1/8", sixteenth = "1/16", thirtySecond = "1/32"
    var beats: Double {
        switch self { case .off: return 0; case .whole: return 4; case .half: return 2; case .quarter: return 1; case .eighth: return 0.5; case .sixteenth: return 0.25; case .thirtySecond: return 0.125 }
    }
}

enum PianoRollScale: String, CaseIterable {
    case chromatic = "Chromatic", major = "Major", minor = "Minor", dorian = "Dorian", mixolydian = "Mixolydian", pentatonicMinor = "Minor Pentatonic"
    var pitchClasses: Set<Int> {
        switch self {
        case .chromatic: return Set(0..<12)
        case .major: return [0,2,4,5,7,9,11]
        case .minor: return [0,2,3,5,7,8,10]
        case .dorian: return [0,2,3,5,7,9,10]
        case .mixolydian: return [0,2,4,5,7,9,10]
        case .pentatonicMinor: return [0,3,5,7,10]
        }
    }
}

struct PianoRollScaleFilter: Equatable {
    var root: Int = 0
    var scale: PianoRollScale = .chromatic
    var enabled = false
    func contains(_ pitch: Int) -> Bool {
        !enabled || scale.pitchClasses.contains((pitch - root + 120) % 12)
    }
}

struct PianoRollQuantize: Equatable {
    var grid: PianoRollGrid = .sixteenth
    var strength: Double = 1
    var swing: Double = 0
    var triplet = false
}

struct PianoRollHumanize: Equatable {
    var timingFrames: UInt64 = 0
    var velocity: UInt8 = 0
}

struct PianoRollViewportModel: Equatable {
    var pixelsPerBeat: CGFloat = 96
    var rowHeight: CGFloat = 16
    var originBeat: Double = 0
    var topPitch: Int = 84
    var lowestPitch: Int = 0
    var highestPitch: Int = 127
}

struct PianoRollSelectionSummary: Equatable {
    var count = 0
    var earliestFrame: UInt64?
    var latestFrame: UInt64?
    var lowestPitch: UInt8?
    var highestPitch: UInt8?
    var averageVelocity: Double?
}

extension Array where Element == PianoRollEditableNote {
    func summary(selection: Set<PianoRollNoteID>) -> PianoRollSelectionSummary {
        let picked = filter { selection.contains($0.id) }
        guard !picked.isEmpty else { return PianoRollSelectionSummary() }
        return PianoRollSelectionSummary(
            count: picked.count,
            earliestFrame: picked.map(\.startFrames).min(),
            latestFrame: picked.map(\.endFrames).max(),
            lowestPitch: picked.map(\.pitch).min(),
            highestPitch: picked.map(\.pitch).max(),
            averageVelocity: picked.map { Double($0.velocity) }.reduce(0,+) / Double(picked.count))
    }
}
