import Foundation

struct AudioHardwareFormat {
    var sampleRate: Double
    var bufferFrames: UInt32
    var minimumFrames: Double
    var maximumFrames: Double
    var sampleRates: [ClosedRange<Double>]
    var rateWritable: Bool
    var bufferWritable: Bool
    var running: Bool

    enum Failure: LocalizedError {
        case invalidBuffer, unavailableRate, readOnlyBuffer, busy
        var errorDescription: String? {
            switch self {
            case .invalidBuffer: "Введите целое число кадров в диапазоне устройства, от 1 до 4096."
            case .unavailableRate: "Устройство не поддерживает переключение на 48 кГц этого проекта."
            case .readOnlyBuffer: "Драйвер не разрешает изменение буфера из приложения."
            case .busy: "Остановите запись и воспроизведение во всех приложениях, использующих устройство."
            }
        }
    }
    func requestedFrames(_ text: String) throws -> UInt32 {
        let value = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !value.isEmpty, value.utf8.allSatisfy({ $0 >= 48 && $0 <= 57 }),
              let frames = UInt32(value), (1...4096).contains(frames) else { throw Failure.invalidBuffer }
        if running { throw Failure.busy }
        if !sampleRate.isFinite || abs(sampleRate - 48000) > 0.5 {
            guard rateWritable, sampleRates.contains(where: { $0.contains(48000) }) else { throw Failure.unavailableRate }
        }
        if frames != bufferFrames {
            guard bufferWritable else { throw Failure.readOnlyBuffer }
            guard minimumFrames.isFinite, maximumFrames.isFinite,
                  Double(frames) >= minimumFrames, Double(frames) <= maximumFrames else { throw Failure.invalidBuffer }
        }
        return frames
    }
}

struct AudioHardwareChangeResult {
    var state: UInt32
    var sampleRate: Double = 0
    var bufferFrames: UInt32 = 0
    var actualKnown = false
    var mayHaveChanged = false
    var error = ""
}
