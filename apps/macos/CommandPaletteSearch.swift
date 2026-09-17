import Foundation

/// Values only: matching and usage ordering do not depend on AppKit or a session.
struct DAWCommandSearchItem: Equatable, Sendable {
    let id: String
    let title: String
    let path: String
    let shortcut: String
}

enum DAWCommandPaletteScope: Int, CaseIterable, Sendable {
    case all, recent, frequent
}

/// Menu position is deliberately absent: inserting another command must not
/// attach its history to an unrelated action. Explicit identifiers survive
/// title/localization changes; the fallback is specific to the full menu path.
enum DAWCommandIdentity {
    static func make(identifier: String?, parents: [String], title: String,
                     selector: String, tag: Int) -> String {
        let components: [String]
        if let identifier, !identifier.isEmpty {
            components = ["menu-id-v1", identifier]
        } else {
            components = ["menu-path-v1"] + parents + [title, selector, String(tag)]
        }
        return components.map { "\($0.utf8.count):\($0)" }.joined()
    }
}

/// Most-recent-first bounded history. No clock or project revision is involved.
/// A record means target/action was dispatched, not that a modal operation was
/// completed (the user can still cancel the Open/Export dialog).
struct DAWCommandUsageHistory: Equatable, Sendable {
    struct Entry: Codable, Equatable, Sendable {
        let id: String
        var count: Int
    }
    private struct Envelope: Codable {
        let version: Int
        let entries: [Entry]
    }
    static let capacity = 100
    static let maximumCount = 1_000_000
    static let maximumEncodedBytes = 256 * 1024
    private(set) var entries: [Entry] = []

    init(encoded data: Data? = nil) {
        guard let data, data.count <= Self.maximumEncodedBytes,
              let envelope = try? JSONDecoder().decode(Envelope.self, from: data),
              envelope.version == 1 else { return }
        var seen = Set<String>()
        for entry in envelope.entries {
            guard Self.validID(entry.id), entry.count > 0,
                  seen.insert(entry.id).inserted else { continue }
            entries.append(Entry(id: entry.id, count: min(entry.count, Self.maximumCount)))
            if entries.count == Self.capacity { break }
        }
        enforceEncodedBudget()
    }

    mutating func recordInvocation(id: String) {
        guard Self.validID(id) else { return }
        let previous = entries.first(where: { $0.id == id })?.count ?? 0
        entries.removeAll { $0.id == id }
        entries.insert(Entry(id: id, count: min(previous + 1, Self.maximumCount)), at: 0)
        if entries.count > Self.capacity { entries.removeLast(entries.count - Self.capacity) }
        enforceEncodedBudget()
    }

    /// JSON escaping can expand a bounded UTF-8 ID. Keep our own output within
    /// the decoder's byte budget too, or a valid history disappears on restart.
    private mutating func enforceEncodedBudget() {
        while !entries.isEmpty {
            guard let data = encoded() else { entries.removeAll(); return }
            guard data.count > Self.maximumEncodedBytes else { return }
            entries.removeLast()
        }
    }

    func encoded() -> Data? {
        try? JSONEncoder().encode(Envelope(version: 1, entries: entries))
    }

    /// Only commands available in the captured menu context can be returned.
    /// History never resurrects a removed, hidden, or disabled menu action.
    func orderedCommands(_ commands: [DAWCommandSearchItem], scope: DAWCommandPaletteScope)
        -> [DAWCommandSearchItem] {
        var byID: [String: DAWCommandSearchItem] = [:]
        var naturalOrder: [DAWCommandSearchItem] = []
        for command in commands where byID[command.id] == nil {
            byID[command.id] = command
            naturalOrder.append(command)
        }
        let usage: [Entry]
        if scope == .frequent {
            usage = entries.enumerated().sorted {
                $0.element.count == $1.element.count
                    ? $0.offset < $1.offset : $0.element.count > $1.element.count
            }.map(\.element)
        } else {
            usage = entries
        }
        let used = usage.compactMap { byID[$0.id] }
        guard scope == .all else { return used }
        let usedIDs = Set(used.map(\.id))
        return used + naturalOrder.filter { !usedIDs.contains($0.id) }
    }

    private static func validID(_ id: String) -> Bool {
        !id.isEmpty && id.utf8.count <= 2048 && !id.contains("\0")
    }
}

/// Local preferences only. Never writes a project or the command search text.
@MainActor
final class DAWCommandUsageStore {
    static let defaultKey = "commandPalette.usage.v1"
    private let defaults: UserDefaults
    private let key: String
    private(set) var history: DAWCommandUsageHistory

    init(defaults: UserDefaults = .standard, key: String = DAWCommandUsageStore.defaultKey) {
        self.defaults = defaults
        self.key = key
        history = DAWCommandUsageHistory(encoded: defaults.data(forKey: key))
    }

    func recordInvocation(id: String) {
        history.recordInvocation(id: id)
        if let data = history.encoded() { defaults.set(data, forKey: key) }
    }

    func clear() {
        history = DAWCommandUsageHistory()
        defaults.removeObject(forKey: key)
    }
}

struct DAWCommandPaletteSearch {
    static func results(in items: [DAWCommandSearchItem], query rawQuery: String,
                        limit: Int = 80) -> [DAWCommandSearchItem] {
        guard limit > 0, rawQuery.count <= 256 else { return [] }
        let query = normalize(rawQuery).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty else { return Array(items.prefix(limit)) }
        let tokens = query.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        return items.enumerated().compactMap { offset, item -> (Int, Int, DAWCommandSearchItem)? in
            guard let score = score(item, query: query, tokens: tokens) else { return nil }
            return (score, offset, item)
        }.sorted { lhs, rhs in
            lhs.0 == rhs.0 ? lhs.1 < rhs.1 : lhs.0 > rhs.0
        }.prefix(limit).map(\.2)
    }

    private static func score(_ item: DAWCommandSearchItem, query: String, tokens: [String]) -> Int? {
        let title = normalize(item.title)
        let path = normalize(item.path)
        let shortcut = normalize(item.shortcut)
        let words = title.split { $0.isWhitespace || "/-›:".contains($0) }
        var total = 0
        if title == query { total += 1_400 }
        else if title.hasPrefix(query) { total += 1_000 }
        else if title.contains(query) { total += 700 }
        else if path.contains(query) { total += 420 }
        for token in tokens {
            if title == token { total += 900 }
            else if title.hasPrefix(token) { total += 720 }
            else if words.contains(where: { $0.hasPrefix(token) }) { total += 620 }
            else if title.contains(token) { total += 480 }
            else if path.contains(token) { total += 300 }
            else if shortcut.contains(token) { total += 250 }
            else if let fuzzy = fuzzySubsequenceScore(needle: token, haystack: title) { total += fuzzy }
            else if let fuzzy = fuzzySubsequenceScore(needle: token, haystack: path) { total += fuzzy / 2 }
            else { return nil }
        }
        return total - min(80, max(0, title.count - query.count))
    }

    private static func normalize(_ value: String) -> String {
        let locale = Locale(identifier: "en_US_POSIX")
        return value.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: locale)
            .lowercased(with: locale)
    }

    /// Abbreviation matching, not spelling correction: letters must be in order.
    private static func fuzzySubsequenceScore(needle: String, haystack: String) -> Int? {
        let source = Array(haystack)
        var sourceIndex = 0
        var previousMatch: Int?
        var gaps = 0
        var consecutive = 0
        for character in needle {
            var found: Int?
            while sourceIndex < source.count {
                if source[sourceIndex] == character {
                    found = sourceIndex
                    sourceIndex += 1
                    break
                }
                sourceIndex += 1
            }
            guard let found else { return nil }
            if let previousMatch {
                let gap = found - previousMatch - 1
                gaps += gap
                if gap == 0 { consecutive += 1 }
            }
            previousMatch = found
        }
        return max(30, 190 + consecutive * 14 - gaps * 5)
    }
}
