import Foundation

/// Window-local interaction policy. Frames and Undo remain owned by the session.
enum ArrangementTool: Int, CaseIterable {
    case pointer = 1, range, split, erase, draw, mute, hand, zoom, fade

    var title: String {
        switch self {
        case .pointer: "Указатель"
        case .range: "Выделение"
        case .split: "Ножницы"
        case .erase: "Ластик"
        case .draw: "MIDI-карандаш"
        case .mute: "Mute аудиоклипа"
        case .hand: "Рука"
        case .zoom: "Масштаб"
        case .fade: "Фейды аудиоклипа"
        }
    }
    var symbol: String {
        switch self {
        case .pointer: "cursorarrow"
        case .range: "rectangle.dashed"
        case .split: "scissors"
        case .erase: "eraser"
        case .draw: "pencil"
        case .mute: "speaker.slash"
        case .hand: "hand.draw"
        case .zoom: "magnifyingglass"
        case .fade: "line.diagonal"
        }
    }
    /// Physical digit keys also work with a Russian layout and with the keypad.
    static func key(_ code: UInt16) -> Self? {
        let keys: [UInt16: Int] = [18: 1, 19: 2, 20: 3, 21: 4, 23: 5, 22: 6,
                                  26: 7, 28: 8, 25: 9, 83: 1, 84: 2, 85: 3,
                                  86: 4, 87: 5, 88: 6, 89: 7, 91: 8, 92: 9]
        return keys[code].flatMap(Self.init(rawValue:))
    }
}

struct ArrangementClipKey: Hashable {
    enum Kind: Hashable { case audio, midi }
    let track: UInt64
    let index: Int
    let kind: Kind
}

struct ArrangementClipBounds: Equatable {
    var start: UInt64
    var length: UInt64
    var offset: UInt64 = 0
    var sourceLength: UInt64 = 0
    var fadeIn: UInt64 = 0
    var fadeOut: UInt64 = 0
    var looped = false

    func isValid(limit: UInt64) -> Bool {
        length > 0 && start <= limit && length <= limit - start &&
            fadeIn <= length && fadeOut <= length - fadeIn
    }
    var end: UInt64 { start.addingReportingOverflow(length).overflow ? UInt64.max : start + length }
}

enum ArrangementEditMath {
    static func frame(x: Double, width: Double, total: UInt64) -> UInt64 {
        guard x.isFinite, width.isFinite, width > 0, total > 0 else { return 0 }
        // Converting Double(UInt64.max) back to UInt64 traps: clamp before conversion.
        let fraction = min(1, max(0, x / width))
        if fraction >= 1 { return total }
        let value = (fraction * Double(total)).rounded()
        return value >= Double(total) ? total : UInt64(value)
    }
    static func snapped(_ frame: UInt64, anchor: UInt64, quantum: UInt64, limit: UInt64) -> UInt64 {
        let value = min(frame, limit)
        guard quantum > 0, anchor <= value, anchor <= limit else { return value }
        let relative = value - anchor
        let whole = relative / quantum
        let remainder = relative % quantum
        let roundUp = remainder >= quantum / 2 + quantum % 2
        let steps = whole + (roundUp ? 1 : 0)
        guard steps <= (limit - anchor) / quantum else { return limit }
        return anchor + steps * quantum
    }
    /// One delta for the entire group; clamping never collapses relative spacing.
    static func moveDelta(_ desired: Int64, clips: [ArrangementClipBounds], limit: UInt64) -> Int64 {
        guard limit <= UInt64(Int64.max), !clips.isEmpty,
              clips.allSatisfy({ $0.isValid(limit: limit) }) else { return 0 }
        let earliest = clips.map(\.start).min() ?? 0
        let latest = clips.map(\.end).max() ?? 0
        return min(Int64(limit - latest), max(-Int64(earliest), desired))
    }
    static func trim(_ clip: ArrangementClipBounds, frame: UInt64, left: Bool,
                     midi: Bool, limit: UInt64) -> ArrangementClipBounds? {
        guard clip.isValid(limit: limit) else { return nil }
        var result = clip
        if !midi {
            guard clip.offset < clip.sourceLength,
                  clip.looped || clip.length <= clip.sourceLength - clip.offset else { return nil }
        }
        if left {
            let lower = midi ? 0 : clip.start - min(clip.start, clip.offset)
            let start = min(clip.end - 1, max(lower, frame))
            result.start = start; result.length = clip.end - start
            if !midi {
                guard clip.offset <= clip.sourceLength else { return nil }
                if start >= clip.start {
                    let delta = start - clip.start
                    guard delta < clip.sourceLength - clip.offset else { return nil }
                    result.offset = clip.offset + delta
                } else { result.offset = clip.offset - (clip.start - start) }
            }
        } else {
            var upper = limit
            if !midi && !clip.looped {
                guard clip.offset <= clip.sourceLength else { return nil }
                upper = clip.start + min(limit - clip.start, clip.sourceLength - clip.offset)
            }
            guard upper > clip.start else { return nil }
            result.length = min(upper, max(clip.start + 1, frame)) - clip.start
        }
        result.fadeIn = min(result.fadeIn, result.length)
        result.fadeOut = min(result.fadeOut, result.length - result.fadeIn)
        return result
    }
    static func fade(_ clip: ArrangementClipBounds, frame: UInt64, left: Bool) -> ArrangementClipBounds {
        var result = clip
        guard clip.fadeIn <= clip.length, clip.fadeOut <= clip.length - clip.fadeIn else { return result }
        if left { result.fadeIn = min(clip.length - clip.fadeOut, frame > clip.start ? frame - clip.start : 0) }
        else { result.fadeOut = min(clip.length - clip.fadeIn, frame < clip.end ? clip.end - frame : 0) }
        return result
    }
    static func intersects(_ clip: ArrangementClipBounds, start: UInt64, end: UInt64) -> Bool {
        start < end && clip.start < end && clip.end > start
    }
}
