import Foundation

@main
struct LibraryFolderScannerTests {
    static func main() async throws {
        var assertions = 0
        func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
            assertions += 1
            precondition(condition(), message)
        }
        let fm = FileManager()
        let base = fm.temporaryDirectory.appendingPathComponent("mydaw-folder-scan-\(UUID().uuidString)")
        try fm.createDirectory(at: base, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: base) }
        let paths = ["Drums/Kick2.wav", "Drums/Kick10.WAV", "Bass/Kick2.wav", "Vocal.aif", "Pad.AIFF", "FX.aifc", "ignore.mp3", ".hidden.wav", ".hidden/secret.wav"]
        for path in paths {
            let url = base.appendingPathComponent(path)
            try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data().write(to: url)
        }
        try fm.createDirectory(at: base.appendingPathComponent("empty.wav"), withIntermediateDirectories: true)
        try fm.createSymbolicLink(at: base.appendingPathComponent("linked.wav"), withDestinationURL: base.appendingPathComponent("Vocal.aif"))
        try fm.createSymbolicLink(at: base.appendingPathComponent("loop"), withDestinationURL: base)
        let result = try LibraryFolderScanner.scan(base)
        expect(result.urls.count == 6, "Only regular visible WAV/AIFF family files")
        expect(Set(result.urls.map(\.path)).count == 6, "Distinct identical names retained; symlinks never followed")
        expect(!result.urls.contains { $0.lastPathComponent == "empty.wav" }, "Directory extension is not an audio file")
        expect(result.limitedBy == nil && result.unreadable == 0, "Complete healthy enumeration")
        let sorted = result.urls.map(\.lastPathComponent)
        expect(sorted.firstIndex(of: "Kick2.wav")! < sorted.firstIndex(of: "Kick10.WAV")!, "Foundation natural ordering")
        for _ in 0..<3 { expect(tryScan(base).map(\.path) == result.urls.map(\.path), "Stable order, full-path tie break") }
        let limit = try LibraryFolderScanner.scan(base, limits: .init(files: 2))
        expect(limit.urls.count == 2 && limit.limitedBy == .files, "Explicit file truncation; bounded output")
        let entries = try LibraryFolderScanner.scan(base, limits: .init(entries: 1))
        expect(entries.visited == 1 && entries.limitedBy == .entries, "Metadata traversal cap")
        let timed = try LibraryFolderScanner.scan(base, limits: .init(seconds: 0.000000001))
        expect(timed.limitedBy == .time, "Elapsed metadata budget is reported")
        for url in [URL(string: "https://example.invalid")!, base.appendingPathComponent("missing"), base.appendingPathComponent("Vocal.aif"), base.appendingPathComponent("loop")] {
            do { _ = try LibraryFolderScanner.scan(url); fatalError("Invalid root accepted") }
            catch { expect(true, "Reject nonlocal/missing/file/symlink root") }
        }
        for limits in [LibraryFolderScanLimits(files: 0), .init(entries: -1), .init(seconds: .nan), .init(seconds: -.infinity)] {
            do { _ = try LibraryFolderScanner.scan(base, limits: limits); fatalError("Invalid limits accepted") }
            catch { expect(true, "Reject invalid bounds") }
        }
        let cancel = Task.detached {
            try LibraryFolderScanner.scan(base) { _ in
                withUnsafeCurrentTask { $0?.cancel() }
            }
        }
        do { _ = try await cancel.value; fatalError("Cancelled scan committed") }
        catch is CancellationError { expect(true, "Cooperative cancellation exits without a partial success") }
        let empty = base.appendingPathComponent("Empty")
        try fm.createDirectory(at: empty, withIntermediateDirectories: true)
        expect(tryScan(empty).isEmpty, "Empty directory succeeds, unlike inaccessible root")
        print("PASS: \(assertions) filesystem scanner assertions")
    }
    static func tryScan(_ url: URL) -> [URL] { try! LibraryFolderScanner.scan(url).urls }
}
