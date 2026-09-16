import Foundation

// MARK: - Темпо-карта проекта: Swift-зеркало доменного пересчёта beats↔frames
//
// Домен (engine/domain/session.cpp: State::beatsAtFrame/frameAtBeats) остаётся
// единственным источником истины; приложение только читает карту через C-ABI
// (daw_get_tempo_count/daw_get_tempo_point, daw_get_time_signature_count/
// daw_get_time_signature_point) и пишет её одной revision-aware командой
// daw_set_tempo. Формулы ниже — сверенное зеркало, а не молча скопированный код:
// на Apple arm64 `long double` — это 64-битный double, поэтому накопление
// сегментов в Double даёт побитово тот же результат, что и домен.
// Один бит = 60/bpm секунд при проектных 48 кГц, то есть 2 880 000 / bpm
// кадров; 120 BPM = 24 000 кадров на бит, 60 BPM = 48 000 кадров на бит.

/// Точка темпа из карты проекта (POD daw_tempo_point).
struct ProjectTempoPoint {
    var frame: UInt64
    var bpm: Double
}

/// Точка размера из карты проекта (POD daw_time_signature_point).
struct ProjectSignaturePoint {
    var frame: UInt64
    var numerator: UInt8
    var denominator: UInt8
}

/// Начало такта: кадр, позиция в битах и порядковый номер для линейки.
struct ProjectBarStart {
    var frame: UInt64
    var beats: Double
    var number: Int
}

struct BeatFrameMap {
    /// 48 000 кадров/с × 60 с — кадр/бит = framesPerMinute / bpm.
    static let framesPerMinute = 2_880_000.0
    static let timelineLimitFrame: UInt64 = 1 << 40
    static let ticksPerBeat = 480
    var tempo: [ProjectTempoPoint] = [ProjectTempoPoint(frame: 0, bpm: 120)]
    var signatures: [ProjectSignaturePoint] = [ProjectSignaturePoint(frame: 0, numerator: 4, denominator: 8)]

    /// Темп на позиции: последняя точка не позже кадра (карта действует last-holds).
    func tempoPoint(atFrame frame: UInt64) -> ProjectTempoPoint {
        var chosen = tempo[0]
        for candidate in tempo where candidate.frame <= frame { chosen = candidate }
        return chosen
    }
    func bpm(atFrame frame: UInt64) -> Double { tempoPoint(atFrame: frame).bpm }
    func framesPerBeat(atFrame frame: UInt64) -> Double { Self.framesPerMinute / bpm(atFrame: frame) }
    func signature(atFrame frame: UInt64) -> ProjectSignaturePoint {
        var chosen = signatures[0]
        for candidate in signatures where candidate.frame <= frame { chosen = candidate }
        return chosen
    }

    /// Зеркало State::beatsAtFrame: сумма целых сегментов, одно округление в конце.
    func beats(atFrame frame: UInt64) -> Double {
        let lane = tempo.sorted { $0.frame < $1.frame }
        var beats = 0.0
        var index = 0
        while index + 1 < lane.count, lane[index + 1].frame <= frame {
            beats += Double(lane[index + 1].frame - lane[index].frame) * lane[index].bpm / Self.framesPerMinute
            index += 1
        }
        beats += (frame > lane[index].frame ? Double(frame - lane[index].frame) : 0) * lane[index].bpm / Self.framesPerMinute
        return beats
    }

    /// Зеркало State::frameAtBeats: обратный ход по тем же сегментам.
    func frame(atBeats beats: Double) -> UInt64 {
        let lane = tempo.sorted { $0.frame < $1.frame }
        guard beats.isFinite, beats >= 0 else { return lane[0].frame }
        var remaining = beats
        var index = 0
        while true {
            let perBeat = Self.framesPerMinute / lane[index].bpm
            if index + 1 < lane.count {
                let segment = Double(lane[index + 1].frame - lane[index].frame) / perBeat
                if remaining == segment { return lane[index + 1].frame }
                if remaining > segment { remaining -= segment; index += 1; continue }
            }
            let value = Double(lane[index].frame) + remaining * perBeat
            guard value >= 0 else { return 0 }
            guard value < Double(Self.timelineLimitFrame) else { return Self.timelineLimitFrame }
            return UInt64(value.rounded())
        }
    }

    /// Такты: длина такта = numerator × 4 / denominator четвертных бит (бит домена — всегда
    /// четвертная): 4/8 → 2 бита, 4/4 → 4, 3/4 → 3. Размер читается из карты на кадре
    /// текущей границы, а сама точка смены размера всегда становится началом такта —
    /// при смене размера сетка тактов двигается.
    func barStarts(upToFrame limit: UInt64) -> [ProjectBarStart] {
        var marks: [ProjectBarStart] = []
        let changes = signatures.sorted { $0.frame < $1.frame }
        var beats = 0.0
        var frame: UInt64 = 0
        var number = 1
        while marks.count < 4096 {
            if frame > limit { break }
            marks.append(ProjectBarStart(frame: frame, beats: beats, number: number))
            let signature = self.signature(atFrame: frame)
            var nextBeats = beats + Double(max(1, Int(signature.numerator))) * 4 / Double(max(1, Int(signature.denominator)))
            var nextFrame = self.frame(atBeats: nextBeats)
            if let change = changes.first(where: { $0.frame > frame && $0.frame <= nextFrame }) {
                nextBeats = self.beats(atFrame: change.frame)
                nextFrame = change.frame
            }
            guard nextFrame > frame else { break }
            beats = nextBeats
            frame = nextFrame
            number += 1
        }
        return marks
    }

    /// 'такт.бит.тики' по позиции; тики — доли бита (480 на бит).
    func barBeatTick(atFrame frame: UInt64, bars: [ProjectBarStart]) -> String {
        let beats = max(0, self.beats(atFrame: frame))
        var anchor = ProjectBarStart(frame: 0, beats: 0, number: 1)
        for mark in bars where mark.beats <= beats + 1e-9 { anchor = mark }
        let offset = max(0, beats - anchor.beats)
        let whole = floor(offset)
        let tick = min(Self.ticksPerBeat - 1, Int((offset - whole) * Double(Self.ticksPerBeat)))
        return String(format: "%d.%d.%03d", anchor.number, Int(whole) + 1, max(0, tick))
    }
}

