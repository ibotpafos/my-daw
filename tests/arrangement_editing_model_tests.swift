import Foundation

@main
struct ArrangementEditingModelTests {
    static func main() {
        var checks = 0
        func expect(_ value: @autoclosure () -> Bool, _ text: String) {
            checks += 1
            precondition(value(), text)
        }
        typealias M = ArrangementEditMath
        let limit: UInt64 = 48_000 * 600
        expect(ArrangementTool.allCases.count == 9, "Nine tools")
        for (code, raw) in [(18,1),(19,2),(20,3),(21,4),(23,5),(22,6),(26,7),(28,8),(25,9),
                            (83,1),(84,2),(85,3),(86,4),(87,5),(88,6),(89,7),(91,8),(92,9)] {
            expect(ArrangementTool.key(UInt16(code))?.rawValue == raw, "Physical/keypad tool mapping")
        }
        expect(ArrangementTool.key(0) == nil, "Letters are not tools")
        expect(M.frame(x: .nan, width: 100, total: limit) == 0, "NaN")
        expect(M.frame(x: 100, width: 0, total: limit) == 0, "Zero width")
        expect(M.frame(x: -10, width: 100, total: limit) == 0, "Before project")
        expect(M.frame(x: 200, width: 100, total: UInt64.max) == UInt64.max, "Conversion at UInt64 boundary")
        expect(M.frame(x: 1, width: 2, total: 100) == 50, "Half width")
        expect(M.snapped(108, anchor: 100, quantum: 12, limit: 1000) == 112, "Tempo-relative snap")
        expect(M.snapped(105, anchor: 100, quantum: 12, limit: 1000) == 100, "Snap down")
        expect(M.snapped(106, anchor: 100, quantum: 12, limit: 1000) == 112, "Half-up")
        expect(M.snapped(105, anchor: 100, quantum: 0, limit: 1000) == 105, "Grid off")
        expect(M.snapped(UInt64.max, anchor: 0, quantum: 1, limit: .max) == .max, "No snap overflow")
        let a = ArrangementClipBounds(start: 100, length: 100, offset: 50, sourceLength: 500, fadeIn: 20, fadeOut: 30)
        let b = ArrangementClipBounds(start: 400, length: 100, sourceLength: 500)
        expect(M.moveDelta(-1000, clips: [a,b], limit: 1000) == -100, "Group clamps together at zero")
        expect(M.moveDelta(1000, clips: [a,b], limit: 1000) == 500, "Group clamps latest end")
        expect(M.moveDelta(100, clips: [], limit: 1000) == 0, "Empty group")
        expect(M.trim(a, frame: 0, left: true, midi: false, limit: 1000)?.start == 50, "Left source bound")
        expect(M.trim(a, frame: 500, left: true, midi: false, limit: 1000)?.length == 1, "No zero length trim")
        expect(M.trim(a, frame: 1000, left: false, midi: false, limit: 1000)?.length == 450, "Audio source end")
        expect(M.trim(a, frame: 1000, left: false, midi: true, limit: 1000)?.length == 900, "MIDI expansion")
        expect(M.trim(a, frame: 110, left: false, midi: false, limit: 1000)?.fadeOut == 0, "Fades clamp when shrinking")
        expect(M.fade(a, frame: 1000, left: true).fadeIn == 70, "Fade cannot cross opposite fade")
        expect(M.fade(a, frame: 0, left: false).fadeOut == 80, "Fade-out bounded")
        expect(!M.intersects(a, start: 200, end: 300), "Half-open adjacency")
        expect(M.intersects(a, start: 199, end: 300), "One-frame overlap")
        let invalid = ArrangementClipBounds(start: 0, length: 10, offset: .max, sourceLength: .max)
        expect(M.trim(invalid, frame: 1, left: true, midi: false, limit: 1000) == nil, "Invalid offset cannot overflow")
        var seed: UInt64 = 0xFACE1234
        for _ in 0..<4000 {
            seed = seed &* 6364136223846793005 &+ 1
            let start = seed % (limit - 1000)
            let clip = ArrangementClipBounds(start: start, length: 1000, offset: 500, sourceLength: 2000)
            let frame = (seed >> 12) % limit
            for left in [false,true] {
                let trimmed = M.trim(clip, frame: frame, left: left, midi: false, limit: limit)!
                expect(trimmed.isValid(limit: limit), "Valid trim bounds")
                expect(trimmed.offset < trimmed.sourceLength && trimmed.length <= trimmed.sourceLength - trimmed.offset, "Source bounds")
                expect(left ? trimmed.end == clip.end : trimmed.start == clip.start, "Opposite edge remains fixed")
            }
            let snap = M.snapped(frame, anchor: 0, quantum: 6000, limit: limit)
            expect(snap <= limit, "Snap remains bounded")
            let delta = M.moveDelta(Int64(seed % (limit * 2)) - Int64(limit), clips: [clip], limit: limit)
            let moved = UInt64(Int64(clip.start) + delta)
            expect(moved <= limit - clip.length, "Moved clip remains bounded")
        }
        print("PASS: \(checks) arrangement editing model assertions")
    }
}
