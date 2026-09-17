import Foundation

/// Presentation-only fractions. NSClipView remains the owner of actual scrolling.
struct ArrangementOverviewGeometry: Equatable {
    let documentWidth: Double
    let viewportWidth: Double
    let start: Double
    let extent: Double
    var maximumStart: Double { 1 - extent }

    init(documentWidth: Double, viewportX: Double, viewportWidth: Double) {
        let width = documentWidth.isFinite && documentWidth > 0 ? documentWidth : 1
        let visible = viewportWidth.isFinite ? max(0, min(width, viewportWidth)) : width
        self.documentWidth = width
        self.viewportWidth = visible
        extent = visible / width
        start = Self.clamp(viewportX / width, upper: 1 - extent)
    }

    func origin(forStart fraction: Double) -> Double {
        Self.clamp(fraction, upper: maximumStart) * documentWidth
    }

    func centeredStart(at fraction: Double) -> Double {
        Self.clamp(fraction - extent / 2, upper: maximumStart)
    }

    static func fraction(at x: Double, width: Double) -> Double {
        guard width.isFinite, width > 0 else { return 0 }
        return clamp(x / width, upper: 1)
    }

    /// Bounds are evaluated without start + length overflow, even for bad input.
    static func segment(start: UInt64, length: UInt64, total: UInt64) -> ClosedRange<Double>? {
        guard total > 0, start < total, length > 0 else { return nil }
        let end = start + min(length, total - start)
        return Double(start) / Double(total)...Double(end) / Double(total)
    }

    private static func clamp(_ value: Double, upper: Double) -> Double {
        guard value.isFinite else { return 0 }
        return min(upper, max(0, value))
    }
}
