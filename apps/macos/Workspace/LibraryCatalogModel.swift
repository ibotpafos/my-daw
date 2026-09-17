import Foundation

enum LibraryCategory: Int, CaseIterable {
    case audio, instruments, effects
    var title: String { switch self { case .audio: return "Аудио"; case .instruments: return "Инстр."; case .effects: return "Эффекты" } }
    var symbol: String { switch self { case .audio: return "waveform"; case .instruments: return "pianokeys"; case .effects: return "slider.horizontal.3" } }
    var formats: [LibraryFormat] { self == .audio ? [.wav, .aiff] : [.au, .vst3] }
}

enum LibraryFormat: String, CaseIterable {
    case unknown, wav, aiff, au, vst3
    var title: String { self == .unknown ? "Другой" : rawValue.uppercased() }
    static func audio(_ url: URL?) -> Self {
        switch url?.pathExtension.lowercased() {
        case "wav": return .wav
        case "aif", "aiff", "aifc": return .aiff
        default: return .unknown
        }
    }
}

enum LibraryCollection: Int { case all, favorites }

/// Stable resource identity, never a catalog row, display name or plug-in version.
/// Constructing a key performs no filesystem access and never grants file access.
enum LibraryResourceKey {
    static func audio(_ url: URL) -> String? {
        guard url.isFileURL else { return nil }
        return checked("audio:" + url.standardizedFileURL.path)
    }
    static func audioUnit(type: UInt32, subtype: UInt32, manufacturer: UInt32) -> String {
        String(format: "au:%08X:%08X:%08X", type, subtype, manufacturer)
    }
    static func vst3(path: String, classID: String) -> String? {
        guard path.hasPrefix("/"), classID.utf8.count == 32,
              classID.utf8.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) }) else { return nil }
        return checked("vst3:" + classID.uppercased() + ":" + URL(fileURLWithPath: path).standardizedFileURL.path)
    }
    static func checked(_ key: String) -> String? {
        guard !key.contains("\0"), key.utf8.count <= 8192 else { return nil }
        if key.hasPrefix("audio:/") { return key }
        if key.hasPrefix("au:") {
            let parts = key.split(separator: ":")
            guard parts.count == 4, parts.dropFirst().allSatisfy({ $0.count == 8 && UInt32($0, radix: 16) != nil }) else { return nil }
            return key
        }
        if key.hasPrefix("vst3:") {
            let parts = key.split(separator: ":", maxSplits: 2, omittingEmptySubsequences: false)
            guard parts.count == 3, parts[1].utf8.count == 32, parts[2].hasPrefix("/"),
                  parts[1].utf8.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) }) else { return nil }
            return key
        }
        return nil
    }
}

/// A small local preference, not a second project database. Only identifiers are
/// retained. Missing/unscanned resources are never resurrected as usable entries.
@MainActor
final class LibraryFavorites {
    static let defaultsKey = "workspace.libraryFavorites.v1"
    static let limit = 256
    private struct Archive: Codable { var version = 1; var keys: [String] }
    private let defaults: UserDefaults
    private(set) var keys: Set<String> = []

    init(defaults: UserDefaults) {
        self.defaults = defaults
        guard let data = defaults.data(forKey: Self.defaultsKey), data.count <= 1024 * 1024,
              let archive = try? JSONDecoder().decode(Archive.self, from: data), archive.version == 1 else { return }
        for key in archive.keys.prefix(4096) where keys.count < Self.limit {
            if let valid = LibraryResourceKey.checked(key) { keys.insert(valid) }
        }
    }
    func contains(_ key: String?) -> Bool { key.map { keys.contains($0) } ?? false }
    @discardableResult
    func set(_ key: String, favorite: Bool) -> Bool {
        guard LibraryResourceKey.checked(key) != nil else { return false }
        guard contains(key) != favorite else { return true }
        guard !favorite || keys.count < Self.limit else { return false }
        var next = keys
        if favorite { next.insert(key) } else { next.remove(key) }
        guard let data = try? JSONEncoder().encode(Archive(keys: next.sorted())), data.count <= 1024 * 1024 else { return false }
        defaults.set(data, forKey: Self.defaultsKey)
        keys = next
        return true
    }
}

enum LibrarySearch {
    /// All words match across the real name, format, vendor/detail and path.
    /// Foundation handles Unicode/case/diacritics; no custom fuzzy-search engine.
    static func matches(_ query: String, in fields: [String]) -> Bool {
        let options: String.CompareOptions = [.caseInsensitive, .diacriticInsensitive]
        let tokens = query.folding(options: options, locale: .current).split(whereSeparator: { $0.isWhitespace })
        let haystack = fields.joined(separator: " ").folding(options: options, locale: .current)
        return tokens.allSatisfy { haystack.contains($0) }
    }
}
