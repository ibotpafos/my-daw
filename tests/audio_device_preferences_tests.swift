import Foundation

@main
struct AudioPreferencesTests {
    static func main() throws {
        var checks = 0
        func expect(_ value: Bool) { checks += 1; precondition(value) }
        func rejects(_ operation: () throws -> Void) {
            do { try operation(); preconditionFailure("Expected rejection") } catch { checks += 1 }
        }
        let name = "mydaw-audio-test-\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defer { defaults.removePersistentDomain(forName: name) }
        expect(try AudioDevicePreferences.read(from: defaults) == AudioDevicePreferences())
        var config = AudioDevicePreferences()
        config.inputUID = "Студия:микрофон"; config.outputUID = "Studio:output"
        config.inputChannel = 3; config.outputLeft = 4; config.outputRight = 5
        defaults.set(try config.encoded(), forKey: AudioDevicePreferences.key)
        expect(try AudioDevicePreferences.read(from: defaults) == config)
        let reopened = UserDefaults(suiteName: name)!
        expect(try AudioDevicePreferences.read(from: reopened) == config)
        var bad = config; bad.outputLeft = bad.outputRight
        rejects { _ = try bad.encoded() }
        bad = config; bad.inputUID = "a\0b"
        rejects { _ = try bad.encoded() }
        bad = config; bad.inputUID = String(repeating: "x", count: 481)
        rejects { _ = try bad.encoded() }
        bad = config; bad.version = 99
        rejects { _ = try bad.encoded() }
        bad = config; bad.inputChannel = 128
        rejects { _ = try bad.encoded() }
        for payload in [Data("{}".utf8), Data("[]".utf8), Data(repeating: 0, count: 8193)] {
            defaults.set(payload, forKey: AudioDevicePreferences.key)
            rejects { _ = try AudioDevicePreferences.read(from: defaults) }
        }
        defaults.set("not data", forKey: AudioDevicePreferences.key)
        rejects { _ = try AudioDevicePreferences.read(from: defaults) }
        for channel in 0..<128 {
            config.inputChannel = UInt32(channel)
            config.outputLeft = UInt32(channel)
            config.outputRight = UInt32((channel + 1) % 128)
            defaults.set(try config.encoded(), forKey: AudioDevicePreferences.key)
            expect(try AudioDevicePreferences.read(from: defaults) == config)
        }
        print("PASS: audio preferences: \(checks) checks")
    }
}
