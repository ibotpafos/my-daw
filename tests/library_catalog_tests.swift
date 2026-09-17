import Foundation

@main
struct LibraryCatalogTests {
    @MainActor static func main() {
        var assertions = 0
        func expect(_ value: @autoclosure () -> Bool, _ message: String) {
            assertions += 1
            if !value() { fatalError(message) }
        }
        let suite = "library-model-\(UUID().uuidString)"
        guard let defaults = UserDefaults(suiteName: suite) else { fatalError("Isolated defaults") }
        defer { defaults.removePersistentDomain(forName: suite) }
        let a = URL(fileURLWithPath: "/samples/Kick.wav")
        let b = URL(fileURLWithPath: "/other/Kick.wav")
        let key = LibraryResourceKey.audio(a)!
        expect(key != LibraryResourceKey.audio(b), "Same name, different files")
        expect(key == LibraryResourceKey.audio(URL(fileURLWithPath: "/samples/sub/../Kick.wav")), "Standardized path identity")
        expect(LibraryResourceKey.audio(URL(string: "https://example.invalid/Kick.wav")!) == nil, "No remote file identity")
        expect(LibraryFormat.audio(URL(fileURLWithPath: "/FILE.AIFC")) == .aiff, "AIFF aliases case-insensitive")
        expect(LibraryFormat.audio(a) == .wav && LibraryFormat.audio(nil) == .unknown, "WAV and unknown")
        let au = LibraryResourceKey.audioUnit(type: 0x61756678, subtype: 1, manufacturer: 2)
        expect(LibraryResourceKey.checked(au) == au, "Canonical AU triplet")
        expect(au != LibraryResourceKey.audioUnit(type: 0x61756D75, subtype: 1, manufacturer: 2), "AU type is identity")
        let cid = "0123456789abcdef0123456789abcdef"
        let vst = LibraryResourceKey.vst3(path: "/Library/Audio/Plug-Ins/VST3/Test.vst3", classID: cid)
        expect(vst != nil && LibraryResourceKey.checked(vst!) != nil, "Canonical VST3 identity")
        expect(vst == LibraryResourceKey.vst3(path: "/Library/Audio/Plug-Ins/VST3/Test.vst3", classID: cid.uppercased()), "SDK FUID case normalization")
        expect(vst != LibraryResourceKey.vst3(path: "/different/Test.vst3", classID: cid), "Same class different module")
        expect(LibraryResourceKey.vst3(path: "relative.vst3", classID: cid) == nil, "No relative module path")
        for invalid in ["", "12", String(repeating: "Z", count: 32)] {
            expect(LibraryResourceKey.vst3(path: "/a.vst3", classID: invalid) == nil, "Invalid class IDs")
        }
        for invalid in ["", "index:1", "audio:relative", "au:1:2:3", "au:00000000:00000000:GGGGGGGG", "audio:/a\0b", "audio:/" + String(repeating: "a", count: 8192)] {
            expect(LibraryResourceKey.checked(invalid) == nil, "Invalid preference identifier")
        }
        let store = LibraryFavorites(defaults: defaults)
        expect(store.keys.isEmpty && !store.contains(nil), "Initial preference empty")
        expect(store.set(key, favorite: true) && store.contains(key), "Favorite added")
        let saved = defaults.data(forKey: LibraryFavorites.defaultsKey)
        expect(store.set(key, favorite: true) && defaults.data(forKey: LibraryFavorites.defaultsKey) == saved, "Idempotent favorite")
        expect(LibraryFavorites(defaults: defaults).contains(key), "New store restores the favorite")
        expect(store.set(key, favorite: false) && !LibraryFavorites(defaults: defaults).contains(key), "Removal persisted")
        for index in 0..<LibraryFavorites.limit {
            expect(store.set("audio:/fixtures/\(index).wav", favorite: true), "Bounded capacity")
        }
        expect(store.keys.count == LibraryFavorites.limit, "Exact capacity")
        expect(!store.set("audio:/overflow.wav", favorite: true), "Capacity cannot grow")
        expect(store.set("audio:/fixtures/0.wav", favorite: false), "Can remove at capacity")
        expect(store.set("audio:/replacement.wav", favorite: true), "Can add after removing")
        for payload in [Data("wrong".utf8), Data("{\"version\":2,\"keys\":[\"audio:/a\"]}".utf8), Data(repeating: 0, count: 1024 * 1024 + 1)] {
            defaults.set(payload, forKey: LibraryFavorites.defaultsKey)
            expect(LibraryFavorites(defaults: defaults).keys.isEmpty, "Corrupt, future or oversized archive ignored")
        }
        defaults.set("not data", forKey: LibraryFavorites.defaultsKey)
        expect(LibraryFavorites(defaults: defaults).keys.isEmpty, "Wrong preference type")
        defaults.set(Data("{\"version\":1,\"keys\":[\"index:4\",\"audio:/a\",\"audio:/a\"]}".utf8), forKey: LibraryFavorites.defaultsKey)
        expect(LibraryFavorites(defaults: defaults).keys == ["audio:/a"], "Invalid entries removed; duplicates collapsed")
        expect(LibrarySearch.matches("  DYNAMICS apple au ", in: ["Dynamics Processor", "Apple", "AU"]), "Words match across metadata")
        expect(LibrarySearch.matches("cafe", in: ["Café.wav"]), "Diacritic matching")
        expect(LibrarySearch.matches("БАС WAV", in: ["Плотный бас", "wav"]), "Cyrillic matching")
        expect(!LibrarySearch.matches("bass vst3", in: ["bass", "AU"]), "All words required")
        expect(LibrarySearch.matches("  \n ", in: []), "Empty search")
        print("PASS: \(assertions) library identity, favorites persistence, bounds and Unicode-search assertions")
    }
}
