import Foundation

struct LibraryFolderScan: Sendable {
    struct Progress: Sendable { let visited: Int; let found: Int }
    enum Limit: String, Sendable { case files, entries, time }
    var urls: [URL] = []
    var visited = 0
    var unreadable = 0
    var limitedBy: Limit?
}

struct LibraryFolderScanLimits: Sendable {
    var files = 10_000
    var entries = 100_000
    var seconds: TimeInterval = 15
}

/// Metadata only, on a utility worker. Foundation owns directory traversal;
/// the audio decoder/session/plug-in host are never called from this worker.
enum LibraryFolderScanner {
    static func scan(_ root: URL, limits: LibraryFolderScanLimits = .init(),
                     progress: @Sendable (LibraryFolderScan.Progress) -> Void = { _ in }) throws -> LibraryFolderScan {
        try Task.checkCancellation()
        guard root.isFileURL, limits.files > 0, limits.entries > 0,
              limits.seconds.isFinite, limits.seconds > 0 else { throw CocoaError(.fileReadInvalidFileName) }
        let keys: Set<URLResourceKey> = [.isRegularFileKey, .isDirectoryKey, .isSymbolicLinkKey, .isPackageKey]
        let properties = try root.resourceValues(forKeys: keys)
        guard properties.isDirectory == true, properties.isSymbolicLink != true,
              properties.isPackage != true else { throw CocoaError(.fileReadUnsupportedScheme) }
        let manager = FileManager()
        var result = LibraryFolderScan()
        var rootError: Error?
        guard let enumerator = manager.enumerator(at: root, includingPropertiesForKeys: Array(keys),
            options: [.skipsHiddenFiles, .skipsPackageDescendants], errorHandler: { url, error in
                if url.standardizedFileURL == root.standardizedFileURL { rootError = error; return false }
                result.unreadable += 1
                return !Task.isCancelled
            }) else { throw CocoaError(.fileReadNoPermission) }
        let started = ProcessInfo.processInfo.systemUptime
        var lastProgress = started
        progress(.init(visited: 0, found: 0))
        while true {
            try Task.checkCancellation()
            if ProcessInfo.processInfo.systemUptime - started >= limits.seconds { result.limitedBy = .time; break }
            guard let url = enumerator.nextObject() as? URL else { break }
            if result.visited >= limits.entries { result.limitedBy = .entries; break }
            result.visited += 1
            do {
                let values = try url.resourceValues(forKeys: keys)
                if values.isSymbolicLink == true || values.isPackage == true {
                    enumerator.skipDescendants()
                } else if values.isRegularFile == true,
                          ["wav", "aif", "aiff", "aifc"].contains(url.pathExtension.lowercased()) {
                    if result.urls.count >= limits.files { result.limitedBy = .files; break }
                    result.urls.append(url)
                }
            } catch { result.unreadable += 1 }
            let now = ProcessInfo.processInfo.systemUptime
            if now - lastProgress >= 0.15 {
                progress(.init(visited: result.visited, found: result.urls.count)); lastProgress = now
            }
        }
        try Task.checkCancellation()
        if let rootError { throw rootError }
        result.urls.sort {
            let order = $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent)
            return order == .orderedSame ? $0.path < $1.path : order == .orderedAscending
        }
        progress(.init(visited: result.visited, found: result.urls.count))
        return result
    }
}
