import Foundation

/// An adapter to the project's existing BeatFrameMap, NOT an independent tempo.
/// All musical editing uses quarter-note beats; project storage remains frames.
struct PRTimeMap {
    let clipStart: UInt64
    let clipLength: UInt64
    let toAbsoluteBeat: (UInt64) -> Double
    let toAbsoluteFrame: (Double) -> UInt64
    let originBeat: Double
    let durationBeats: Double

    init(clipStart: UInt64, clipLength: UInt64,
         toBeat: @escaping (UInt64) -> Double,
         toFrame: @escaping (Double) -> UInt64) throws {
        guard clipLength > 0, clipStart <= PRLimits.timelineFrames,
              clipLength <= PRLimits.timelineFrames - clipStart else { throw PREditError.invalidClip }
        let origin = toBeat(clipStart)
        let duration = toBeat(clipStart + clipLength) - origin
        guard origin.isFinite, origin >= 0, duration.isFinite, duration > 0 else { throw PREditError.invalidClip }
        self.clipStart = clipStart; self.clipLength = clipLength
        toAbsoluteBeat = toBeat; toAbsoluteFrame = toFrame
        originBeat = origin; durationBeats = duration
    }

    func beat(at frame: UInt64) -> Double {
        toAbsoluteBeat(clipStart + min(frame, clipLength)) - originBeat
    }
    func frame(at beat: Double) -> UInt64 {
        guard beat.isFinite else { return 0 }
        let absolute = toAbsoluteFrame(originBeat + min(durationBeats, max(0, beat)))
        guard absolute > clipStart else { return 0 }
        return min(clipLength, absolute - clipStart)
    }
    func start(_ note: PianoRollNote) -> Double { beat(at: note.startFrames) }
    func end(_ note: PianoRollNote) -> Double {
        beat(at: note.startFrames + min(note.lengthFrames, clipLength - min(clipLength, note.startFrames)))
    }
    func length(_ note: PianoRollNote) -> Double { max(0, end(note) - start(note)) }

    func note(start: Double, end: Double, pitch: UInt8, channel: UInt8, velocity: UInt8) throws -> PianoRollNote {
        guard start.isFinite, end.isFinite, start >= -1e-8,
              end <= durationBeats + 1e-8, end > start else { throw PREditError.outsideClip }
        let first = frame(at: start), last = frame(at: end)
        guard last > first else { throw PREditError.invalidNote }
        let result = PianoRollNote(startFrames: first, lengthFrames: last - first,
                                   pitch: pitch, channel: channel, velocity: velocity)
        try PREdits.validate([result], clipLength: clipLength)
        return result
    }
}

enum PRGridDivision: Int, CaseIterable {
    case adaptive, whole, half, quarter, eighth, sixteenth, thirtySecond, sixtyFourth, eighthTriplet, sixteenthTriplet
    var title: String { ["Адаптивная", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/8 T", "1/16 T"][rawValue] }
    func step(pointsPerBeat: Double) -> Double {
        switch self {
        case .adaptive:
            return [1024.0, 512, 256, 128, 64, 32, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 0.0625, 0.03125]
                .last(where: { $0 * max(0.000_001, pointsPerBeat) >= 14 }) ?? 1024
        case .whole: return 4
        case .half: return 2
        case .quarter: return 1
        case .eighth: return 0.5
        case .sixteenth: return 0.25
        case .thirtySecond: return 0.125
        case .sixtyFourth: return 0.0625
        case .eighthTriplet: return 1 / 3
        case .sixteenthTriplet: return 1 / 6
        }
    }
}

struct PRGrid {
    var enabled = true
    var division: PRGridDivision = .sixteenth
    /// 0 = straight, 1/3 = a 2:1 swing; UI bounds this to 0...0.75.
    var swing: Double = 0
    func step(_ pointsPerBeat: Double) -> Double { division.step(pointsPerBeat: pointsPerBeat) }

    func snap(_ relativeBeat: Double, origin: Double, pointsPerBeat: Double, bypass: Bool = false) -> Double {
        guard enabled, !bypass, relativeBeat.isFinite, origin.isFinite else { return relativeBeat }
        let unit = step(pointsPerBeat), value = relativeBeat + origin
        let pair = floor(value / (2 * unit))
        let offset = min(0.75, max(0, swing))
        var closest = value, distance = Double.infinity
        for k in -1...1 {
            let first = (pair + Double(k)) * 2 * unit
            for candidate in [first, first + unit * (1 + offset), first + 2 * unit] {
                let error = abs(candidate - value)
                if error < distance { distance = error; closest = candidate }
            }
        }
        return closest - origin
    }
}

struct PRPitchRows {
    let pitches: [Int]
    private let rows: [Int: Int]
    init(used: Set<Int>? = nil, scale: PRScale? = nil) {
        var values = Array((0...127).reversed())
        if let used, !used.isEmpty { values = values.filter { used.contains($0) } }
        if let scale { values = values.filter { scale.contains($0) } }
        if values.isEmpty { values = [60] }
        pitches = values
        rows = Dictionary(uniqueKeysWithValues: values.enumerated().map { ($0.element, $0.offset) })
    }
    func row(for pitch: UInt8) -> Int? { rows[Int(pitch)] }
    func pitch(at row: Int) -> Int { pitches[min(pitches.count - 1, max(0, row))] }
}
