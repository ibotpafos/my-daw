import Foundation

/// Search synonyms only: no selectors, closures, shortcuts or project data.
struct DAWCommandAliases: Equatable, Sendable {
    static let maximumCommands = 256
    static let maximumAliases = 8
    static let maximumCharacters = 48
    static let maximumEncodedBytes = 256 * 1024
    private struct Envelope: Codable {
        let version: Int
        let commands: [String: [String]]
    }
    enum ValidationError: Error, LocalizedError {
        case invalidID, tooManyAliases, invalidAlias, capacity, encodedSize
        var errorDescription: String? {
            switch self {
            case .invalidID: return "Для этой команды нельзя сохранить своё название."
            case .tooManyAliases: return "Можно задать до 8 названий для одной команды."
            case .invalidAlias: return "Название: до 48 символов (192 байт), без управляющих символов и точки с запятой."
            case .capacity: return "Достигнут лимит: 256 команд со своими названиями."
            case .encodedSize: return "Названия не сохранены: превышен лимит настроек 256 KiB."
            }
        }
    }
    private(set) var commands: [String: [String]] = [:]

    init(encoded data: Data? = nil) {
        guard let data, data.count <= Self.maximumEncodedBytes,
              let value = try? JSONDecoder().decode(Envelope.self, from: data),
              value.version == 1 else { return }
        // Stable restoration, even for hand-edited preferences. Invalid records
        // cannot remove valid ones. Serialized input is bounded before decoding.
        for id in value.commands.keys.sorted() {
            try? set(value.commands[id] ?? [], for: id)
        }
    }

    func aliases(for id: String) -> [String] { commands[id] ?? [] }

    static func parse(_ text: String) -> [String] {
        text.components(separatedBy: ";")
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
    }

    /// Transactional: validation failure does not discard old aliases or evict
    /// another command. An empty list explicitly removes this command's aliases.
    mutating func set(_ values: [String], for id: String) throws {
        guard !id.isEmpty, id.utf8.count <= 2048, !id.contains("\0") else {
            throw ValidationError.invalidID
        }
        guard values.count <= Self.maximumAliases else { throw ValidationError.tooManyAliases }
        var seen = Set<String>()
        var cleaned: [String] = []
        for value in values {
            guard !value.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }),
                  !value.contains(";") else { throw ValidationError.invalidAlias }
            let alias = value.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
            guard !alias.isEmpty, alias.count <= Self.maximumCharacters, alias.utf8.count <= 192 else {
                throw ValidationError.invalidAlias
            }
            if seen.insert(DAWCommandPaletteSearch.normalize(alias)).inserted { cleaned.append(alias) }
        }
        var candidate = self
        candidate.commands[id] = cleaned.isEmpty ? nil : cleaned
        guard candidate.commands.count <= Self.maximumCommands else { throw ValidationError.capacity }
        guard try candidate.encoded().count <= Self.maximumEncodedBytes else { throw ValidationError.encodedSize }
        self = candidate
    }

    func encoded() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        return try encoder.encode(Envelope(version: 1, commands: commands))
    }

    func applying(to item: DAWCommandSearchItem) -> DAWCommandSearchItem {
        DAWCommandSearchItem(id: item.id, title: item.title, path: item.path,
                            shortcut: item.shortcut, aliases: aliases(for: item.id))
    }
}

/// Reload before edits so two document windows do not overwrite each other's
/// unrelated preferences. Search text and usage history use neither this key
/// nor this store. A failed edit writes nothing.
@MainActor
final class DAWCommandAliasStore {
    static let defaultKey = "commandPalette.aliases.v1"
    private let defaults: UserDefaults
    private let key: String
    private(set) var aliases: DAWCommandAliases

    init(defaults: UserDefaults = .standard, key: String = DAWCommandAliasStore.defaultKey) {
        self.defaults = defaults
        self.key = key
        aliases = DAWCommandAliases(encoded: defaults.data(forKey: key))
    }
    func reload() { aliases = DAWCommandAliases(encoded: defaults.data(forKey: key)) }
    func set(_ values: [String], for id: String) throws {
        var next = DAWCommandAliases(encoded: defaults.data(forKey: key))
        try next.set(values, for: id)
        let data = try next.encoded()
        if next.commands.isEmpty { defaults.removeObject(forKey: key) }
        else { defaults.set(data, forKey: key) }
        aliases = next
    }
}
