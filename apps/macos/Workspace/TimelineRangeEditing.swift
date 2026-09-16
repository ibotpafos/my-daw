import Foundation

/// Transient ruler gesture geometry. The committed range lives in DraftApp and
/// the existing transport, never in a second persisted project model.
struct TimelineFrameRange: Equatable {
    let start: UInt64
    let end: UInt64
    var length: UInt64 { end - start }

    init?(start: UInt64, end: UInt64, limit: UInt64) {
        guard start < end, end <= limit else { return nil }
        self.start = start
        self.end = end
    }
}

struct TimelineRangeGesture {
    enum Mode { case create, move, resizeStart, resizeEnd }
    let mode: Mode
    let original: TimelineFrameRange?
    let anchor: UInt64
    let limit: UInt64

    func range(at rawFrame: UInt64, snap: (UInt64) -> UInt64) -> TimelineFrameRange? {
        guard limit > 0 else { return nil }
        let frame = min(rawFrame, limit)
        let origin = min(anchor, limit)
        let snapped = min(snap(frame), limit)
        switch mode {
        case .create:
            let start = min(snap(origin), limit)
            return TimelineFrameRange(start: min(start, snapped), end: max(start, snapped), limit: limit)
        case .move:
            guard let original, original.end <= limit else { return nil }
            let maxStart = limit - original.length
            let start: UInt64
            if frame >= origin {
                start = original.start + min(frame - origin, maxStart - original.start)
            } else {
                start = original.start - min(origin - frame, original.start)
            }
            let result = min(snap(start), maxStart)
            return TimelineFrameRange(start: result, end: result + original.length, limit: limit)
        case .resizeStart:
            guard let original, original.end <= limit else { return nil }
            return TimelineFrameRange(start: min(snapped, original.end - 1), end: original.end, limit: limit)
        case .resizeEnd:
            guard let original, original.end <= limit else { return nil }
            return TimelineFrameRange(start: original.start, end: max(snapped, original.start + 1), limit: limit)
        }
    }
}

/// Zoom about the visible playhead; when it is offscreen, preserve the viewport
/// midpoint instead. AppKit remains responsible for final document clamping.
struct TimelineZoomAnchor {
    let fraction: Double
    let screenX: Double

    init(documentWidth: Double, viewportX: Double, viewportWidth: Double, playheadFraction: Double) {
        let width = documentWidth.isFinite ? max(1, documentWidth) : 1
        let origin = viewportX.isFinite ? max(0, viewportX) : 0
        let viewport = viewportWidth.isFinite ? max(0, viewportWidth) : 0
        let cursorFraction = playheadFraction.isFinite ? min(1, max(0, playheadFraction)) : 0
        let cursor = cursorFraction * width
        let visible = cursor >= origin && cursor <= origin + viewport
        let anchor = visible ? cursor : min(width, origin + viewport / 2)
        fraction = min(1, max(0, anchor / width))
        screenX = anchor - origin
    }

    func origin(documentWidth: Double, viewportWidth: Double) -> Double {
        guard documentWidth.isFinite, viewportWidth.isFinite else { return 0 }
        return min(max(0, documentWidth - viewportWidth), max(0, fraction * documentWidth - screenX))
    }
}
