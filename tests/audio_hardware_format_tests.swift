import Foundation

@main
struct AudioHardwareFormatTests {
    static func main() throws {
        var checks = 0
        func expect(_ condition: Bool) { checks += 1; precondition(condition, "Check \(checks)") }
        func rejects(_ value: String, _ format: AudioHardwareFormat) {
            do { _ = try format.requestedFrames(value); fatalError("Accepted \(value)") }
            catch { checks += 1 }
        }
        var format = AudioHardwareFormat(sampleRate: 44100, bufferFrames: 128, minimumFrames: 32,
            maximumFrames: 2048, sampleRates: [44100...44100, 48000...96000], rateWritable: true,
            bufferWritable: true, running: false)
        for value in ["32", "33", "128", "256", "2048", " 512 ", "000128"] {
            expect(try format.requestedFrames(value) == UInt32(value.trimmingCharacters(in: .whitespaces))!)
        }
        for value in ["", " ", "-1", "+128", "128.0", "1e3", "nan", "inf", "0", "31", "2049", "4097", "１２８", "١٢٨", "1 28", "128x", "4294967296"] {
            rejects(value, format)
        }
        format.rateWritable = false; rejects("256", format)
        format.sampleRate = 48000; expect(try format.requestedFrames("256") == 256)
        format.bufferWritable = false; rejects("256", format)
        expect(try format.requestedFrames("128") == 128)
        format.running = true; rejects("128", format)
        format.running = false; format.bufferWritable = true
        format.maximumFrames = .nan; rejects("256", format)
        format.maximumFrames = 8192
        expect(try format.requestedFrames("4096") == 4096)
        rejects("8192", format)
        format.sampleRate = .nan; rejects("128", format)
        print("PASS: audio hardware form: \(checks) checks")
    }
}
