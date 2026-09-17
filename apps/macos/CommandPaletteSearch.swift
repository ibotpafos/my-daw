import Foundation

/// Search-only representation of an AppKit command. Keeping ranking independent
/// from NSMenu makes it deterministic and cheap to test without launching the UI.
struct DAWCommandSearchItem: Equatable {
    let id: String
    let title: String
    let path: String
    let shortcut: String
}

struct DAWCommandPaletteSearch {
    static func results(
        in items: [DAWCommandSearchItem],
        query rawQuery: String,
        limit: Int = 80
    ) -> [DAWCommandSearchItem] {
        let query = normalize(rawQuery).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty else {
            return Array(items.prefix(max(0, limit)))
        }

        let tokens = query.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        let ranked = items.enumerated().compactMap { offset, item -> (Int, Int, DAWCommandSearchItem)? in
            guard let score = score(item, query: query, tokens: tokens) else { return nil }
            return (score, offset, item)
        }
        .sorted { lhs, rhs in
            if lhs.0 != rhs.0 { return lhs.0 > rhs.0 }
            return lhs.1 < rhs.1
        }

        return ranked.prefix(max(0, limit)).map(\.2)
    }

    private static func score(
        _ item: DAWCommandSearchItem,
        query: String,
        tokens: [String]
    ) -> Int? {
        let title = normalize(item.title)
        let path = normalize(item.path)
        let shortcut = normalize(item.shortcut)
        var total = 0

        if title == query { total += 1_400 }
        else if title.hasPrefix(query) { total += 1_000 }
        else if title.contains(query) { total += 700 }
        else if path.contains(query) { total += 420 }

        for token in tokens {
            if title == token {
                total += 900
            } else if title.hasPrefix(token) {
                total += 720
            } else if titleWords(title).contains(where: { $0.hasPrefix(token) }) {
                total += 620
            } else if title.contains(token) {
                total += 480
            } else if path.contains(token) {
                total += 300
            } else if shortcut.contains(token) {
                total += 250
            } else if let fuzzy = fuzzySubsequenceScore(needle: token, haystack: title) {
                total += fuzzy
            } else if let fuzzy = fuzzySubsequenceScore(needle: token, haystack: path) {
                total += fuzzy / 2
            } else {
                return nil
            }
        }

        // Prefer concise command names when relevance is otherwise equal.
        total -= min(80, max(0, title.count - query.count))
        return total
    }

    private static func titleWords(_ value: String) -> [Substring] {
        value.split { character in
            character.isWhitespace || character == "/" || character == "-" || character == "›" || character == ":"
        }
    }

    private static func normalize(_ value: String) -> String {
        value
            .folding(options: [.caseInsensitive, .diacriticInsensitive], locale: Locale.current)
            .lowercased()
    }

    /// Small typo-tolerant fallback: all query characters must appear in order.
    /// Compact consecutive matches rank above scattered ones.
    private static func fuzzySubsequenceScore(needle: String, haystack: String) -> Int? {
        let wanted = Array(needle)
        guard !wanted.isEmpty else { return 0 }
        let source = Array(haystack)
        var sourceIndex = 0
        var previousMatch: Int?
        var gaps = 0
        var consecutive = 0

        for character in wanted {
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
                gaps += max(0, gap)
                if gap == 0 { consecutive += 1 }
            }
            previousMatch = found
        }

        return max(30, 190 + consecutive * 14 - gaps * 5)
    }
}
