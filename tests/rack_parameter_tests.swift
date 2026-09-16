import Foundation

@main
struct RackParameterTests {
    static func main() {
        var count = 0
        func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
            count += 1
            if !condition() { fatalError(message) }
        }
        for (lower, upper, logarithmic) in [(-120.0,24.0,false), (0,1,false), (20,20000,true),
                                            (0.0001,100000,true), (-1,1,true), (5,5,false)] {
            let source = RackParameter(id: 0, name: "Test", minimum: lower, maximum: upper,
                                       value: lower, writable: true, logarithmic: logarithmic)!
            for position in [0.0, 0.001, 0.1, 0.25, 0.5, 0.75, 0.999, 1.0] {
                let native = source.nativeValue(at: position)!
                expect(native.isFinite && (lower...upper).contains(native), "Finite bounded native value")
                let roundtrip = RackParameter(id: 0, name: "Test", minimum: lower, maximum: upper,
                                             value: native, writable: true, logarithmic: logarithmic)!
                expect(abs(roundtrip.position - (lower == upper ? 0 : position)) < 1e-9, "Mapping roundtrip")
            }
            expect(source.nativeValue(at: .nan) == nil, "Reject NaN position")
            expect(source.nativeValue(at: 2) == nil, "Reject bad position")
        }
        let normalized = RackParameter(id: 8, name: "VST3", minimum: 0, maximum: 1, value: 0.4, writable: true, logarithmic: false)!
        expect(normalized.parse(" 0,375 ") == 0.375, "Decimal comma")
        expect(normalized.nativeValue(at: 0.375) == 0.375, "Normalized VST3 values must not be rounded to integers")
        for invalid in ["nan", "inf", "-inf", "1e309", "word", "0,2,3", "-1", "1.1"] {
            expect(normalized.parse(invalid) == nil, "Reject invalid or out-of-range text")
        }
        let fixed = RackParameter(id: 9, name: "Constant", minimum: 2, maximum: 2, value: 2, writable: true, logarithmic: false)!
        expect(!fixed.writable && !fixed.accepts(2), "Fixed range is read-only")
        let meter = RackParameter(id: 10, name: "Meter", minimum: 0, maximum: 1, value: 0, writable: false, logarithmic: false)!
        expect(!meter.accepts(0.5), "Read-only metadata preserved")
        expect(RackParameter(id: 1, name: "Bad", minimum: 2, maximum: 1, value: 1, writable: true, logarithmic: false) == nil, "Reject inverted range")
        expect(RackParameter(id: 1, name: "Bad", minimum: 0, maximum: .infinity, value: 1, writable: true, logarithmic: false) == nil, "Reject infinite range")
        expect(RackParameter(id: 1, name: "Bad", minimum: 0, maximum: Double.greatestFiniteMagnitude, value: 1, writable: true, logarithmic: false) == nil, "Reject values beyond native float ABI")
        print("PASS: \(count) rack parameter range, logarithmic mapping and numeric validation assertions")
    }
}
