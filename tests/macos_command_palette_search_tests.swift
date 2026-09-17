import Foundation

@main
@MainActor
struct CommandPaletteSearchTests {
    private static var checks = 0
    static func main() throws {
        let commands = [
            DAWCommandSearchItem(id: "1", title: "Экспорт WAV…", path: "Файл › Экспорт", shortcut: "⌘E"),
            DAWCommandSearchItem(id: "2", title: "Экспорт DAWproject…", path: "Файл › Экспорт", shortcut: "⇧⌘E"),
            DAWCommandSearchItem(id: "3", title: "Добавить MIDI дорожку", path: "Дорожка", shortcut: ""),
            DAWCommandSearchItem(id: "4", title: "Сканировать Audio Units", path: "Плагины", shortcut: ""),
            DAWCommandSearchItem(id: "5", title: "Масштаб 100%", path: "Вид", shortcut: "⌘0")
        ]
        func ids(_ query: String, limit: Int = 80) -> [String] {
            DAWCommandPaletteSearch.results(in: commands, query: query, limit: limit).map(\.id)
        }
        expect(ids("экспорт") == ["1", "2"], "concise prefix command ranks first")
        expect(ids("midi").first == "3", "case-insensitive title")
        expect(ids("audio unit").first == "4", "all query tokens must match")
        expect(ids("скан au").first == "4", "ordered abbreviation")
        expect(ids("⌘0") == ["5"], "shortcut text")
        expect(ids("несуществующая").isEmpty, "unmatched query")
        expect(ids("", limit: 2) == ["1", "2"], "empty query preserves supplied order")
        expect(ids("   \t\n", limit: 2) == ["1", "2"], "whitespace query")
        expect(ids("", limit: 0).isEmpty && ids("midi", limit: -1).isEmpty, "nonpositive limit")
        expect(ids(String(repeating: "a", count: 257)).isEmpty, "query length bound")
        expect(ids("midi missing").isEmpty, "no partial multi-token match")
        let equal = [DAWCommandSearchItem(id: "x", title: "Play", path: "A", shortcut: ""),
                     DAWCommandSearchItem(id: "y", title: "Play", path: "B", shortcut: "")]
        expect(DAWCommandPaletteSearch.results(in: equal, query: "play").map(\.id) == ["x", "y"], "stable ties")
        let accents = [DAWCommandSearchItem(id: "a", title: "Ёлка Café MIDI", path: "", shortcut: "")]
        expect(DAWCommandPaletteSearch.results(in: accents, query: "елка cafe midi").count == 1, "Unicode folding")
        let identity = DAWCommandIdentity.make(identifier: nil, parents: ["Файл"], title: "Экспорт", selector: "export:", tag: 0)
        expect(identity == DAWCommandIdentity.make(identifier: nil, parents: ["Файл"], title: "Экспорт", selector: "export:", tag: 0), "stable identity")
        expect(identity != DAWCommandIdentity.make(identifier: nil, parents: ["Проект"], title: "Экспорт", selector: "export:", tag: 0), "path separates identity")
        expect(identity != DAWCommandIdentity.make(identifier: nil, parents: ["Файл"], title: "Экспорт", selector: "export:", tag: 1), "tag separates identity")
        expect(identity != DAWCommandIdentity.make(identifier: nil, parents: ["Файл"], title: "Экспорт", selector: "other:", tag: 0), "action separates identity")
        expect(DAWCommandIdentity.make(identifier: "export", parents: ["File"], title: "Export", selector: "e:", tag: 0)
            == DAWCommandIdentity.make(identifier: "export", parents: ["Файл"], title: "Экспорт", selector: "e:", tag: 0), "explicit identifier survives localization")
        expect(DAWCommandIdentity.make(identifier: nil, parents: ["a", "b"], title: "c", selector: "e:", tag: 0)
            != DAWCommandIdentity.make(identifier: nil, parents: ["a›b"], title: "c", selector: "e:", tag: 0), "unambiguous path encoding")

        var history = DAWCommandUsageHistory()
        expect(history.orderedCommands(commands, scope: .all) == commands, "fresh all is natural menu order")
        expect(history.orderedCommands(commands, scope: .recent).isEmpty, "fresh recent is empty")
        expect(history.orderedCommands(commands, scope: .frequent).isEmpty, "fresh frequent is empty")
        history.recordInvocation(id: "1")
        history.recordInvocation(id: "2")
        history.recordInvocation(id: "1")
        history.recordInvocation(id: "3")
        expect(history.entries.map(\.id) == ["3", "1", "2"], "recency moves invoked command to front")
        expect(history.entries.map(\.count) == [1, 2, 1], "counts accumulate per ID")
        expect(history.orderedCommands(commands, scope: .frequent).map(\.id) == ["1", "3", "2"], "frequency breaks ties by recency")
        expect(history.orderedCommands(commands, scope: .all).map(\.id) == ["3", "1", "2", "4", "5"], "recent commands precede unused ones without duplicates")
        let reordered = Array(commands.reversed())
        expect(history.orderedCommands(reordered, scope: .recent).map(\.id) == ["3", "1", "2"], "menu reordering does not corrupt history")
        history.recordInvocation(id: "removed-command")
        expect(history.orderedCommands(commands, scope: .recent).map(\.id) == ["3", "1", "2"], "unavailable history IDs are not executable results")
        expect(history.orderedCommands([], scope: .frequent).isEmpty, "history does not invent commands")
        let ordered = history.orderedCommands(commands, scope: .all)
        expect(DAWCommandPaletteSearch.results(in: ordered, query: "экспорт wav").first?.id == "1", "query relevance wins over recency")
        expect(history.orderedCommands(commands + commands, scope: .all).count == commands.count, "duplicate input IDs are bounded")
        expect(DAWCommandUsageHistory(encoded: history.encoded()) == history, "JSON round trip")
        let before = history
        history.recordInvocation(id: "")
        history.recordInvocation(id: String(repeating: "x", count: 2049))
        history.recordInvocation(id: "\0bad")
        expect(history == before, "invalid IDs do not mutate history")
        for index in 0..<120 { history.recordInvocation(id: "bounded-\(index)") }
        expect(history.entries.count == 100, "history capacity")
        expect(history.entries.first?.id == "bounded-119" && history.entries.last?.id == "bounded-20", "evicts oldest")
        expect(DAWCommandUsageHistory(encoded: Data("not json".utf8)).entries.isEmpty, "corrupt preferences")
        expect(DAWCommandUsageHistory(encoded: Data(repeating: 0, count: 256 * 1024 + 1)).entries.isEmpty, "size bound before decoding")
        func envelope(version: Int = 1, entries: [[String: Any]]) throws -> Data {
            try JSONSerialization.data(withJSONObject: ["version": version, "entries": entries])
        }
        expect(DAWCommandUsageHistory(encoded: try envelope(version: 2, entries: [["id": "1", "count": 1]])).entries.isEmpty, "unknown history version")
        let sanitized = DAWCommandUsageHistory(encoded: try envelope(entries: [
            ["id": "", "count": 1], ["id": "0", "count": 0], ["id": "negative", "count": -1],
            ["id": "1", "count": Int.max], ["id": "1", "count": 5], ["id": "2", "count": 3]
        ]))
        expect(sanitized.entries.map(\.id) == ["1", "2"], "restore removes invalid and duplicate entries")
        expect(sanitized.entries.first?.count == DAWCommandUsageHistory.maximumCount, "restore clamps count")
        var saturated = sanitized
        saturated.recordInvocation(id: "1")
        expect(saturated.entries.first?.count == DAWCommandUsageHistory.maximumCount, "invocation cannot overflow count")
        let oversized = try envelope(entries: (0..<200).map { ["id": "\($0)", "count": 1] })
        expect(DAWCommandUsageHistory(encoded: oversized).entries.count == 100, "restore enforces capacity")

        var escaped = DAWCommandUsageHistory()
        for i in 0..<100 {
            escaped.recordInvocation(id: String(repeating: "\u{0001}", count: 2000) + "-\(i)")
        }
        expect((escaped.encoded()?.count ?? Int.max) <= DAWCommandUsageHistory.maximumEncodedBytes,
               "JSON-escaped IDs obey encoded size budget")
        expect(!escaped.entries.isEmpty && escaped.entries.count < 100,
               "encoded budget evicts oldest entries without losing everything")
        expect(DAWCommandUsageHistory(encoded: escaped.encoded()) == escaped,
               "worst-case escaped history survives a restart")

        let suite = "my-daw.palette-tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = DAWCommandUsageStore(defaults: defaults)
        expect(defaults.data(forKey: DAWCommandUsageStore.defaultKey) == nil, "opening store does not write preferences")
        store.recordInvocation(id: "3")
        let restored = DAWCommandUsageStore(defaults: defaults)
        expect(restored.history.entries.first?.id == "3", "UserDefaults persists across instances")
        store.clear()
        expect(store.history.entries.isEmpty, "clear resets current view")
        expect(defaults.data(forKey: DAWCommandUsageStore.defaultKey) == nil, "clear removes persisted history")
        defaults.set(Data("bad json".utf8), forKey: DAWCommandUsageStore.defaultKey)
        expect(DAWCommandUsageStore(defaults: defaults).history.entries.isEmpty, "corrupt preferences fail closed")
        print("command palette model: \(checks) checks passed")
    }

    private static func expect(_ condition: Bool, _ message: String) {
        checks += 1
        guard condition else {
            FileHandle.standardError.write(Data("FAIL: \(message)\n".utf8))
            exit(1)
        }
    }
}
