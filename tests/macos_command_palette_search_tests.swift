import Foundation

@main
struct CommandPaletteSearchTests {
    static func main() {
        let commands = [
            DAWCommandSearchItem(id: "1", title: "Экспорт WAV…", path: "Файл › Экспорт", shortcut: "⌘E"),
            DAWCommandSearchItem(id: "2", title: "Экспорт DAWproject…", path: "Файл › Экспорт", shortcut: "⇧⌘E"),
            DAWCommandSearchItem(id: "3", title: "Добавить MIDI дорожку", path: "Дорожка", shortcut: ""),
            DAWCommandSearchItem(id: "4", title: "Сканировать Audio Units", path: "Плагины", shortcut: ""),
            DAWCommandSearchItem(id: "5", title: "Масштаб 100%", path: "Вид", shortcut: "⌘0")
        ]

        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "экспорт").map(\.id) == ["1", "2"],
            "prefix search must keep menu order for equally relevant export commands"
        )
        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "midi").first?.id == "3",
            "case/locale-insensitive title search must find MIDI"
        )
        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "audio unit").first?.id == "4",
            "multi-token search must require and rank both words"
        )
        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "скан au").first?.id == "4",
            "fuzzy subsequence fallback should find abbreviated command intent"
        )
        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "⌘0").first?.id == "5",
            "shortcut text must be searchable"
        )
        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "несуществующая").isEmpty,
            "unmatched query must return no commands"
        )
        expect(
            DAWCommandPaletteSearch.results(in: commands, query: "", limit: 2).map(\.id) == ["1", "2"],
            "empty search must preserve original menu order and respect the limit"
        )

        print("command palette search tests: OK")
    }

    private static func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        guard condition() else {
            fputs("FAIL: \(message)\n", stderr)
            exit(1)
        }
    }
}
