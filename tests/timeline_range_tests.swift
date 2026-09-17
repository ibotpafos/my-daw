import Foundation

@main
struct TimelineRangeTests {
    static func main() {
        var checks = 0
        func expect(_ condition: Bool, _ message: String) {
            checks += 1
            precondition(condition, message)
        }
        let identity: (UInt64) -> UInt64 = { $0 }
        expect(TimelineFrameRange(start: 1, end: 1, limit: 10) == nil, "Zero-width selection rejected")
        expect(TimelineFrameRange(start: 4, end: 2, limit: 10) == nil, "Reversed committed selection rejected")
        expect(TimelineFrameRange(start: 0, end: 11, limit: 10) == nil, "Selection outside duration rejected")
        for limit: UInt64 in [1, 2, 97, 48000 * 600, UInt64.max] {
            let original = TimelineFrameRange(start: limit / 4, end: max(1, limit / 2), limit: limit)!
            for mode in [TimelineRangeGesture.Mode.create, .move, .resizeStart, .resizeEnd] {
                let gesture = TimelineRangeGesture(mode: mode, original: original, anchor: original.start, limit: limit)
                for frame in [UInt64(0), limit / 4, limit / 2, limit - 1, limit, UInt64.max] {
                    for snap in [identity, { _ in UInt64.max }, { _ in UInt64(0) }] {
                        guard let result = gesture.range(at: frame, snap: snap) else {
                            expect(mode == .create, "Only collapsed creation may be empty")
                            continue
                        }
                        expect(result.start < result.end && result.end <= limit, "Gesture always inside duration")
                        if mode == .move { expect(result.length == original.length, "Move preserves duration even at boundaries") }
                        if mode == .resizeStart { expect(result.end == original.end, "Left handle preserves right edge") }
                        if mode == .resizeEnd { expect(result.start == original.start, "Right handle preserves left edge") }
                    }
                }
            }
        }
        let original = TimelineFrameRange(start: 200, end: 600, limit: 1000)!
        let forward = TimelineRangeGesture(mode: .create, original: nil, anchor: 210, limit: 1000)
        let reverse = TimelineRangeGesture(mode: .create, original: nil, anchor: 640, limit: 1000)
        let snap: (UInt64) -> UInt64 = { (($0 + 50) / 100) * 100 }
        expect(forward.range(at: 640, snap: snap) == original, "Snap endpoints of created range")
        expect(reverse.range(at: 210, snap: snap) == original, "Reverse drag has identical range")
        expect(forward.range(at: 210, snap: snap) == nil, "Click is not a new range")
        let move = TimelineRangeGesture(mode: .move, original: original, anchor: 300, limit: 1000)
        expect(move.range(at: 0, snap: identity)?.start == 0, "Move stops at project start")
        expect(move.range(at: 1000, snap: identity)?.start == 600, "Move stops at project end")
        expect(TimelineRangeGesture(mode: .move, original: original, anchor: 0, limit: 100).range(at: 80, snap: identity) == nil, "Shrunk project rejects stale gesture")
        expect(TimelineRangeGesture(mode: .create, original: nil, anchor: 0, limit: 0).range(at: 0, snap: identity) == nil, "Empty project cannot create a cycle")
        let visible = TimelineZoomAnchor(documentWidth: 1000, viewportX: 100, viewportWidth: 400, playheadFraction: 0.25)
        expect(visible.origin(documentWidth: 2000, viewportWidth: 400) == 350, "Visible playhead retains screen position")
        let hidden = TimelineZoomAnchor(documentWidth: 1000, viewportX: 100, viewportWidth: 400, playheadFraction: 0.9)
        expect(hidden.origin(documentWidth: 2000, viewportWidth: 400) == 400, "Offscreen playhead zooms about viewport center")
        expect(hidden.origin(documentWidth: 400, viewportWidth: 400) == 0, "Zoom fit resets horizontal scroll")
        for value in [Double.nan, .infinity, -.infinity, -1, 0, 100] {
            let anchor = TimelineZoomAnchor(documentWidth: value, viewportX: value, viewportWidth: value, playheadFraction: value)
            expect(anchor.fraction.isFinite && anchor.screenX.isFinite, "Invalid zoom values do not escape")
            expect(anchor.origin(documentWidth: value, viewportWidth: value).isFinite, "Invalid zoom output is finite")
        }
        print("PASS: \(checks) timeline range and anchored zoom assertions")
    }
}
