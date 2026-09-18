import Foundation

@main
struct RecordCompSelectionTests {
    static func main() {
        var checks = 0
        func expect(_ value: @autoclosure () -> Bool, _ message: String) {
            checks += 1; precondition(value(), message)
        }
        let documentID = UUID()
        let gesture = RecordCompSwipe(documentID: documentID, trackID: 42, takeIndex: 3, revision: 9,
                                      takeStart: 1000, takeFrames: 8000, anchor: 4000)!
        expect(gesture.request(at: 4000) == nil, "Click is not an edit")
        expect(gesture.request(at: 5000)?.range.start == 4000, "Forward swipe")
        expect(gesture.request(at: 2000)?.range.end == 4000, "Backward swipe")
        expect(gesture.request(at: 0)?.range.start == 1000, "Clamp to take start")
        expect(gesture.request(at: UInt64.max)?.range.end == 9000, "Clamp to take end without overflow")
        for frame in stride(from: UInt64(0), through: 12000, by: 17) {
            if let request = gesture.request(at: frame) {
                expect(request.documentID == documentID && request.trackID == 42 && request.takeIndex == 3 && request.revision == 9, "Snapshot identity")
                expect(request.range.start >= 1000 && request.range.end <= 9000, "Inside source")
                expect(request.range.length > 0, "Nonzero selection")
            }
        }
        for (start, frames, anchor) in [(UInt64.max, UInt64(1), UInt64.max), (0, 0, 0),
            (1000, 8000, 999), (1000, 8000, 9001), (RecordCompSwipe.maximumFrame, 1, RecordCompSwipe.maximumFrame)] {
            expect(RecordCompSwipe(documentID: documentID, trackID: 1, takeIndex: 0, revision: 1,
                takeStart: start, takeFrames: frames, anchor: anchor) == nil, "Invalid source/anchor rejected")
        }
        for text in ["", "-1", "nan", "inf", "600.1", "1e100", "abc"] {
            expect(RecordCompSwipe.frame(seconds: text) == nil, "Invalid numeric input rejected")
        }
        expect(RecordCompSwipe.frame(seconds: " 1,25 ") == 60000, "Locale comma input")
        expect(RecordCompSwipe.frame(seconds: "600") == RecordCompSwipe.maximumFrame, "Maximum supported time")
        for frame in stride(from: UInt64(0), through: 480000, by: 137) {
            let text = String(format: "%.6f", Double(frame) / 48000)
            expect(RecordCompSwipe.frame(seconds: text) == frame, "Displayed seconds round-trip sample accurately")
        }
        print("PASS: \(checks) comp selection assertions")
    }
}
