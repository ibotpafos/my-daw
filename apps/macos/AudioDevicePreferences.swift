import Foundation

/// Machine settings only. A saved missing UID is kept, never replaced by a
/// default device while decoding preferences or reopening a musical project.
struct AudioDevicePreferences: Codable, Equatable {
    static let key = "audio.devices.v1"
    var version = 1
    var inputUID = ""
    var outputUID = ""
    var inputChannel: UInt32 = 0
    var outputLeft: UInt32 = 0
    var outputRight: UInt32 = 1

    enum Failure: LocalizedError {
        case invalid
        var errorDescription: String? { "Некорректные настройки аудио. Проверьте устройства и номера каналов." }
    }
    func validate() throws {
        guard version == 1, inputUID.utf8.count <= 480, outputUID.utf8.count <= 480,
              !inputUID.contains("\0"), !outputUID.contains("\0"), inputChannel < 128,
              outputLeft < 128, outputRight < 128, outputLeft != outputRight else { throw Failure.invalid }
    }
    func encoded() throws -> Data {
        try validate()
        let data = try JSONEncoder().encode(self)
        guard data.count <= 8192 else { throw Failure.invalid }
        return data
    }
    static func read(from defaults: UserDefaults) throws -> Self {
        guard let raw = defaults.object(forKey: key) else { return Self() }
        guard let data = raw as? Data, data.count <= 8192 else { throw Failure.invalid }
        let config = try JSONDecoder().decode(Self.self, from: data)
        try config.validate()
        return config
    }
}
