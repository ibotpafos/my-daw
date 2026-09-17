import Foundation

@main struct MixExportPolicyTests {
    static func main() {
        var checks = 0
        func expect(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError(name) }
            checks += 1
        }
        var p = MixExportPolicy()
        expect(p.block == .empty, "Empty project blocks export")
        p.hasMIDI = true; p.duration = 48_000
        expect(p.canExport && p.midiOnly, "MIDI-only project is eligible")
        p.hasAudio = true
        expect(p.canExport && !p.midiOnly, "Mixed project is eligible")
        p.hasMIDI = false
        expect(p.canExport && !p.midiOnly, "Audio-only project is eligible")
        p.duration = 0
        expect(p.block == .empty, "No actual timeline duration")
        p.duration = 48_000; p.start = 1_000
        expect(p.canExport, "Insertion cursor is not an invalid half-range")
        p.end = 24_000
        expect(p.canExport, "Valid range")
        p.end = 1_000
        expect(p.block == .range, "Empty range")
        p.end = 999
        expect(p.block == .range, "Reversed range")
        p.end = 48_001
        expect(p.block == .range, "Range beyond duration")
        p.start = nil; p.end = 24_000
        expect(p.block == .range, "End without a start")
        p.end = nil
        let base = p
        p.recording = true; expect(p.block == .recording, "Do not stop recording to export")
        p = base; p.midiCapture = true; expect(p.block == .midiCapture, "MIDI recording blocks export")
        p = base; p.gesture = true; expect(p.block == .gesture, "Unfinished edits block export")
        p = base; p.busy = true; expect(p.block == .busy, "Export/import busy")
        p = base; p.dialogOpen = true; expect(p.block == .dialog, "Nested dialog blocks export")
        p = base; p.available = false; expect(p.block == .unavailable, "Failed ABI read is fail-closed")
        for mask in 0..<32 {
            p = base
            p.recording = mask & 1 != 0; p.midiCapture = mask & 2 != 0
            p.gesture = mask & 4 != 0; p.busy = mask & 8 != 0; p.dialogOpen = mask & 16 != 0
            precondition(p.canExport == (mask == 0), "All guards compose")
        }
        expect(true, "32 guard combinations")
        print("RESULT \(checks) mix-export policy checks; 32 guard combinations")
    }
}
