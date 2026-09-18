import Foundation

@main @MainActor
struct CommandAliasTests {
    private static var checks = 0
    static func main() throws {
        var model = DAWCommandAliases()
        expect(model.commands.isEmpty, "fresh aliases are empty")
        try model.set(["  рендер  ", "Bounce", "РЕНДЕР", "café", "CAFE", "mix   down"], for: "export")
        expect(model.aliases(for: "export") == ["рендер", "Bounce", "café", "mix down"], "trim, collapse whitespace and Unicode dedup")
        let original = DAWCommandSearchItem(id: "export", title: "Экспорт WAV", path: "Файл", shortcut: "⌘E")
        let decorated = model.applying(to: original)
        expect(decorated.id == original.id && decorated.title == original.title && decorated.shortcut == original.shortcut, "aliases never replace action identity/title/shortcut")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "BOUNCE") == [decorated], "English alias resolves Russian title")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "РЕНДЕР") == [decorated], "Russian case folding")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "cafe") == [decorated], "diacritic folding")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "mx dwn") == [decorated], "existing fuzzy search handles alias")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "файл bounce") == [decorated], "alias plus menu path")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "экспорт WAV") == [decorated], "original title remains searchable")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "⌘E") == [decorated], "original shortcut remains searchable")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "bounce missing").isEmpty, "all tokens still mandatory")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "bounce рендер").isEmpty, "different aliases are not combined into a fictitious phrase")
        let exact = DAWCommandSearchItem(id: "exact", title: "Bounce", path: "", shortcut: "")
        let prefix = DAWCommandSearchItem(id: "prefix", title: "Bounce another", path: "", shortcut: "")
        expect(DAWCommandPaletteSearch.results(in: [prefix, decorated, exact], query: "bounce").map(\.id) == ["exact", "export", "prefix"], "exact title > exact synonym > title prefix")
        try model.set(["Bounce"], for: "second")
        let second = model.applying(to: DAWCommandSearchItem(id: "second", title: "Другой экспорт", path: "", shortcut: ""))
        expect(DAWCommandPaletteSearch.results(in: [decorated, second], query: "bounce").count == 2, "shared aliases show both original commands, never overwrite")
        expect(model.applying(to: original).aliases.count == 4, "other command edit does not remove aliases")
        expect(DAWCommandPaletteSearch.results(in: [decorated], query: "").count == 1, "aliases do not add duplicate rows")
        var usage = DAWCommandUsageHistory()
        usage.recordInvocation(id: "second")
        expect(usage.orderedCommands([decorated, second], scope: .recent) == [second], "aliases do not bypass recent/frequent scope")
        expect(usage.orderedCommands([], scope: .all).isEmpty, "unavailable command never comes from preferences")
        expect(try DAWCommandAliases(encoded: model.encoded()) == model, "bounded JSON roundtrip")
        expect(try model.encoded() == model.encoded(), "deterministic serialization")
        expect(DAWCommandAliases.parse(" рендер ; bounce ; ; ") == ["рендер", "bounce"], "semicolon editor syntax")
        expect(DAWCommandAliases.parse(" ; ").isEmpty, "empty editor removes aliases")
        expect(DAWCommandAliases.parse("mix, down") == ["mix, down"], "comma remains an ordinary alias character")
        let before = model
        for invalid in [[""], ["\n"], ["a\nb"], ["\0"], ["a;b"], ["\u{007f}"],
                        [String(repeating: "x", count: 49)], [String(repeating: "e\u{0301}", count: 100)],
                        Array(repeating: "x", count: 9)] {
            reject { try model.set(invalid, for: "export") }
            expect(model == before, "invalid edit is atomic")
        }
        for invalidID in ["", "\0bad", String(repeating: "x", count: 2049)] {
            reject { try model.set(["name"], for: invalidID) }
        }
        try model.set([String(repeating: "🎹", count: 48)], for: "boundary")
        expect(model.aliases(for: "boundary").count == 1, "48 emoji / 192 bytes boundary is valid")
        let oneCluster = "e" + String(repeating: "\u{0301}", count: 200)
        reject { try model.set([oneCluster], for: "boundary") }
        try model.set([], for: "export")
        expect(model.aliases(for: "export").isEmpty && model.aliases(for: "second") == ["Bounce"], "remove only requested command")

        var full = DAWCommandAliases()
        for i in 0..<256 { try full.set(["name\(i)"], for: "id\(i)") }
        reject { try full.set(["new"], for: "overflow") }
        expect(full.commands.count == 256, "capacity rejects without eviction")
        try full.set(["edited"], for: "id0")
        expect(full.aliases(for: "id0") == ["edited"], "editing at capacity works")
        try full.set([], for: "id0")
        try full.set(["new"], for: "overflow")
        expect(full.commands.count == 256, "removal frees capacity")

        var escaped = DAWCommandAliases()
        var rejectedBudget = false
        for i in 0..<256 {
            let prior = escaped
            do { try escaped.set(["name"], for: String(repeating: "\u{0001}", count: 2000) + "\(i)") }
            catch { rejectedBudget = true; expect(escaped == prior, "encoded budget rejects atomically"); break }
        }
        expect(rejectedBudget, "JSON escaping is part of the actual byte budget")
        expect(try escaped.encoded().count <= DAWCommandAliases.maximumEncodedBytes, "serialized size is bounded")
        expect(try DAWCommandAliases(encoded: escaped.encoded()) == escaped, "escaped bounded state survives restart")
        expect(DAWCommandAliases(encoded: Data(repeating: 0, count: 262145)).commands.isEmpty, "oversize input rejected before decoder")
        expect(DAWCommandAliases(encoded: Data("bad json".utf8)).commands.isEmpty, "corrupt settings safe")
        func encoded(_ version: Int, _ values: [String: Any]) throws -> Data {
            try JSONSerialization.data(withJSONObject: ["version": version, "commands": values])
        }
        expect(DAWCommandAliases(encoded: try encoded(2, ["good": ["yes"]])).commands.isEmpty, "unknown schema rejected")
        let clean = DAWCommandAliases(encoded: try encoded(1, ["good": ["yes", "YES"], "bad": ["line\nbreak"], "": ["x"]]))
        expect(clean.commands == ["good": ["yes"]], "invalid records discarded, valid records survive")
        var generated = DAWCommandAliases()
        for i in 0..<400 {
            let id = "g\(i % 32)"
            try generated.set(i % 3 == 0 ? [] : ["\(i) рендер", "render \(i)"], for: id)
            expect(try DAWCommandAliases(encoded: generated.encoded()) == generated, "generated edit/remove/restart invariant")
        }

        let suite = "mydaw.alias-test.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = DAWCommandAliasStore(defaults: defaults)
        let otherWindow = DAWCommandAliasStore(defaults: defaults)
        expect(defaults.data(forKey: DAWCommandAliasStore.defaultKey) == nil, "opening store writes no settings")
        let history = DAWCommandUsageStore(defaults: defaults)
        history.recordInvocation(id: "kept")
        let historyBytes = defaults.data(forKey: DAWCommandUsageStore.defaultKey)
        try store.set(["bounce"], for: "export")
        try otherWindow.set(["record"], for: "recording")
        store.reload()
        expect(store.aliases.commands == ["export": ["bounce"], "recording": ["record"]], "two windows preserve unrelated edits")
        expect(defaults.data(forKey: DAWCommandUsageStore.defaultKey) == historyBytes, "alias edit leaves usage history untouched")
        let bytes = defaults.data(forKey: DAWCommandAliasStore.defaultKey)
        reject { try store.set(["bad;value"], for: "export") }
        expect(defaults.data(forKey: DAWCommandAliasStore.defaultKey) == bytes, "failed save writes no preferences")
        history.clear()
        expect(defaults.data(forKey: DAWCommandAliasStore.defaultKey) == bytes, "clear usage history retains aliases")
        try store.set([], for: "export"); try store.set([], for: "recording")
        expect(defaults.data(forKey: DAWCommandAliasStore.defaultKey) == nil, "last alias removal deletes only alias key")
        print("command aliases: \(checks) checks passed")
    }
    private static func expect(_ condition: Bool, _ message: String) {
        checks += 1
        guard condition else { fatalError(message) }
    }
    private static func reject(_ work: () throws -> Void) {
        do { try work(); fatalError("expected rejection") } catch { checks += 1 }
    }
}
