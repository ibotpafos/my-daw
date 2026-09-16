import Foundation

@main
struct ProjectTimeTests {
    static func expect(_ condition: @autoclosure () -> Bool, _ name: String) {
        precondition(condition(), "ProjectTime regression: \(name)")
    }

    static func main() {
        let basic = BeatFrameMap()
        expect(basic.frame(atBeats: 1) == 24_000, "120 BPM quarter note")
        expect(basic.beats(atFrame: 48_000) == 2, "default frame conversion")
        expect(basic.frame(atBeats: -1) == 0, "negative beat clamp")
        expect(basic.frame(atBeats: .nan) == 0, "non-finite beat guard")
        expect(basic.frame(atBeats: Double.greatestFiniteMagnitude) == BeatFrameMap.timelineLimitFrame,
               "timeline ceiling")

        var tempo = BeatFrameMap()
        tempo.tempo = [ProjectTempoPoint(frame: 0, bpm: 120), ProjectTempoPoint(frame: 48_000, bpm: 60)]
        expect(tempo.bpm(atFrame: 47_999) == 120, "before tempo boundary")
        expect(tempo.bpm(atFrame: 48_000) == 60, "at tempo boundary")
        expect(tempo.frame(atBeats: 2) == 48_000, "exact tempo boundary")
        expect(tempo.frame(atBeats: 3) == 96_000, "slower segment")
        expect(tempo.beats(atFrame: 96_000) == 3, "piecewise inverse")
        for frame: UInt64 in [0, 1, 12_345, 23_999, 24_000, 47_999, 48_000, 48_001, 96_000, 1_000_000] {
            expect(tempo.frame(atBeats: tempo.beats(atFrame: frame)) == frame, "frame round-trip \(frame)")
        }

        expect(basic.barStarts(upToFrame: 96_000).map(\.frame) == [0, 48_000, 96_000],
               "existing 4/8 project default")
        var meter = BeatFrameMap()
        meter.signatures = [ProjectSignaturePoint(frame: 0, numerator: 4, denominator: 4)]
        let bars = meter.barStarts(upToFrame: 192_000)
        expect(bars.map(\.frame) == [0, 96_000, 192_000], "4/4 bar boundaries")
        expect(meter.barBeatTick(atFrame: 0, bars: bars) == "1.1.000", "first beat label")
        expect(meter.barBeatTick(atFrame: 12_000, bars: bars) == "1.1.240", "half beat ticks")
        expect(meter.barBeatTick(atFrame: 24_000, bars: bars) == "1.2.000", "second beat label")
        expect(meter.barBeatTick(atFrame: 96_000, bars: bars) == "2.1.000", "next bar label")
        meter.signatures.append(ProjectSignaturePoint(frame: 72_000, numerator: 3, denominator: 4))
        expect(meter.barStarts(upToFrame: 144_000).map(\.frame) == [0, 72_000, 144_000],
               "meter change starts a new bar")
        print("PASS: ProjectTime — tempo, rounding, meter boundaries, labels and input guards")
    }
}
