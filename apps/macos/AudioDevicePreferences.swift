import Foundation

/// Machine settings only. A saved missing UID is kept, never replaced by a
/// default device while decoding preferences or reopening a musical project.
/// v2 adds a second input channel and mono/stereo capture mode. v1 preferences
/// migrate to mono without changing their selected source.
struct AudioDevicePreferences: Codable, Equatable {
    static let key = "audio.devices.v2"
    static let legacyKey = "audio.devices.v1"
    var version = 2
    var inputUID = ""
    var outputUID = ""
    var inputChannel: UInt32 = 0
    var inputRight: UInt32 = 1
    var recordingChannels: UInt32 = 1
    var outputLeft: UInt32 = 0
    var outputRight: UInt32 = 1

    private struct LegacyV1: Codable {
        var version: Int
        var inputUID: String
        var outputUID: String
        var inputChannel: UInt32
        var outputLeft: UInt32
        var outputRight: UInt32
    }

    enum Failure: LocalizedError {
        case invalid
        var errorDescription: String? { "Некорректные настройки аудио. Проверьте устройства, режим записи и номера каналов." }
    }
    func validate() throws {
        guard version == 2, inputUID.utf8.count <= 480, outputUID.utf8.count <= 480,
              !inputUID.contains("\0"), !outputUID.contains("\0"),
              recordingChannels == 1 || recordingChannels == 2,
              inputChannel < 128, inputRight < 128,
              recordingChannels == 1 || inputChannel != inputRight,
              outputLeft < 128, outputRight < 128, outputLeft != outputRight else { throw Failure.invalid }
    }
    func encoded() throws -> Data {
        try validate()
        let data = try JSONEncoder().encode(self)
        guard data.count <= 8192 else { throw Failure.invalid }
        return data
    }
    static func read(from defaults: UserDefaults) throws -> Self {
        if let raw = defaults.object(forKey: key) {
            guard let data = raw as? Data, data.count <= 8192 else { throw Failure.invalid }
            let config = try JSONDecoder().decode(Self.self, from: data)
            try config.validate()
            return config
        }
        if let raw = defaults.object(forKey: legacyKey) {
            guard let data = raw as? Data, data.count <= 8192 else { throw Failure.invalid }
            let legacy = try JSONDecoder().decode(LegacyV1.self, from: data)
            guard legacy.version == 1 else { throw Failure.invalid }
            var migrated = Self()
            migrated.inputUID = legacy.inputUID
            migrated.outputUID = legacy.outputUID
            migrated.inputChannel = legacy.inputChannel
            migrated.inputRight = legacy.inputChannel == 127 ? 126 : legacy.inputChannel + 1
            migrated.outputLeft = legacy.outputLeft
            migrated.outputRight = legacy.outputRight
            try migrated.validate()
            return migrated
        }
        return Self()
    }
}
