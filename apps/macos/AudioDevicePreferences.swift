import Foundation

/// Machine settings only. A saved missing UID is kept, never replaced by a
/// default device while decoding preferences or reopening a musical project.
/// Version 2 adds the recording source shape; version-1 data migrates to mono.
struct AudioDevicePreferences: Codable, Equatable {
    static let key = "audio.devices.v1"
    var version = 2
    var inputUID = ""
    var outputUID = ""
    var inputChannel: UInt32 = 0
    var inputRight: UInt32 = 1
    var inputChannels: UInt32 = 1
    var outputLeft: UInt32 = 0
    var outputRight: UInt32 = 1

    enum CodingKeys: String, CodingKey {
        case version, inputUID, outputUID, inputChannel, inputRight, inputChannels, outputLeft, outputRight
    }
    init() {}
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        let storedVersion = try values.decodeIfPresent(Int.self, forKey: .version) ?? 1
        guard storedVersion == 1 || storedVersion == 2 else { throw Failure.invalid }
        inputUID = try values.decodeIfPresent(String.self, forKey: .inputUID) ?? ""
        outputUID = try values.decodeIfPresent(String.self, forKey: .outputUID) ?? ""
        inputChannel = try values.decodeIfPresent(UInt32.self, forKey: .inputChannel) ?? 0
        outputLeft = try values.decodeIfPresent(UInt32.self, forKey: .outputLeft) ?? 0
        outputRight = try values.decodeIfPresent(UInt32.self, forKey: .outputRight) ?? 1
        if storedVersion >= 2 {
            inputRight = try values.decodeIfPresent(UInt32.self, forKey: .inputRight) ?? 1
            inputChannels = try values.decodeIfPresent(UInt32.self, forKey: .inputChannels) ?? 1
        } else {
            inputRight = inputChannel == 127 ? 126 : inputChannel + 1
            inputChannels = 1
        }
        version = 2
        try validate()
    }
    func encode(to encoder: Encoder) throws {
        try validate()
        var values = encoder.container(keyedBy: CodingKeys.self)
        try values.encode(2, forKey: .version)
        try values.encode(inputUID, forKey: .inputUID)
        try values.encode(outputUID, forKey: .outputUID)
        try values.encode(inputChannel, forKey: .inputChannel)
        try values.encode(inputRight, forKey: .inputRight)
        try values.encode(inputChannels, forKey: .inputChannels)
        try values.encode(outputLeft, forKey: .outputLeft)
        try values.encode(outputRight, forKey: .outputRight)
    }

    enum Failure: LocalizedError {
        case invalid
        var errorDescription: String? { "Некорректные настройки аудио. Проверьте устройства и номера каналов." }
    }
    func validate() throws {
        guard version == 2, inputUID.utf8.count <= 480, outputUID.utf8.count <= 480,
              !inputUID.contains("\0"), !outputUID.contains("\0"), inputChannel < 128, inputRight < 128,
              (inputChannels == 1 || inputChannels == 2),
              (inputChannels == 1 || inputChannel != inputRight),
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
        return try JSONDecoder().decode(Self.self, from: data)
    }
}
