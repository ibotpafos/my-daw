import Foundation

@main
struct OverviewGeometryTests {
    static func main() {
        var checks = 0
        func expect(_ condition: Bool, _ text: String) {
            checks += 1
            precondition(condition, text)
        }
        for document in [1.0, 320, 1234, 20000] {
            for viewport in [0.0, 100, 320, 1700, 40000] {
                for x in [-50.0, 0, 75, 1000, 1e100] {
                    let g = ArrangementOverviewGeometry(documentWidth: document, viewportX: x, viewportWidth: viewport)
                    expect(g.start >= 0 && g.start <= g.maximumStart, "Origin is bounded")
                    expect(g.extent >= 0 && g.extent <= 1, "Viewport fraction bounded")
                    expect(g.origin(forStart: g.start) <= max(0, document - viewport) + 0.001, "No overscroll")
                    expect(g.centeredStart(at: 1) == g.maximumStart, "End centers/clamps")
                    expect(g.centeredStart(at: 0) == 0, "Start centers/clamps")
                }
            }
        }
        for invalid in [Double.nan, .infinity, -.infinity] {
            let g = ArrangementOverviewGeometry(documentWidth: invalid, viewportX: invalid, viewportWidth: invalid)
            expect(g.start == 0 && g.extent == 1, "Bad geometry is full viewport")
            expect(g.origin(forStart: invalid) == 0, "Invalid origin is safe")
            expect(ArrangementOverviewGeometry.fraction(at: invalid, width: 100) == 0, "Invalid pointer safe")
            expect(ArrangementOverviewGeometry.fraction(at: 10, width: invalid) == 0, "Invalid width safe")
        }
        let g = ArrangementOverviewGeometry(documentWidth: 4000, viewportX: 1000, viewportWidth: 1000)
        expect(g.start == 0.25 && g.extent == 0.25, "Exact quarter viewport")
        expect(g.origin(forStart: 0.6) == 2400, "Normalized scroll")
        expect(g.origin(forStart: 1) == 3000, "End clamp")
        expect(g.origin(forStart: -1) == 0, "Start clamp")
        expect(g.centeredStart(at: 0.5) == 0.375, "Center target")
        expect(ArrangementOverviewGeometry.fraction(at: -1, width: 100) == 0, "Pointer before start")
        expect(ArrangementOverviewGeometry.fraction(at: 999, width: 100) == 1, "Pointer beyond end")
        expect(ArrangementOverviewGeometry.segment(start: 4, length: 4, total: 16) == 0.25...0.5, "Exact span")
        expect(ArrangementOverviewGeometry.segment(start: 0, length: .max, total: 16) == 0...1, "Clamp overflowing length")
        expect(ArrangementOverviewGeometry.segment(start: .max - 10, length: .max, total: .max)?.upperBound == 1, "UInt64 max without overflow")
        expect(ArrangementOverviewGeometry.segment(start: .max, length: 1, total: .max) == nil, "Outside span")
        expect(ArrangementOverviewGeometry.segment(start: 0, length: 1, total: 0) == nil, "Zero project")
        expect(ArrangementOverviewGeometry.segment(start: 0, length: 0, total: 12) == nil, "Empty clip")
        print("PASS: \(checks) arrangement overview geometry assertions")
    }
}
