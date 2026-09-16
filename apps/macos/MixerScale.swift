import Foundation

/// UI-only scale. The engine continues to store exact finite dB (-120...24).
/// Unity gets useful travel; the bottom is -120 dB, not a dishonest -infinity.
enum MixerScale {
    static let ticks: [Double] = [-60, -36, -18, -6, 0, 6, 12, 24]
    private static let db: [Double] = [-120, -60, -36, -18, -6, 0, 6, 12, 24]
    private static let travel: [Double] = [0, 0.08, 0.20, 0.38, 0.58, 0.72, 0.82, 0.90, 1]
    static func position(_ value: Double) -> Double { interpolate(value, from: db, to: travel) }
    static func decibels(_ position: Double) -> Double { interpolate(position, from: travel, to: db) }
    private static func interpolate(_ value: Double, from: [Double], to: [Double]) -> Double {
        guard value.isFinite else { return to[0] }
        let x = min(from.last!, max(from[0], value))
        for i in 1..<from.count where x <= from[i] {
            return to[i - 1] + (x - from[i - 1]) / (from[i] - from[i - 1]) * (to[i] - to[i - 1])
        }
        return to.last!
    }
    static func levelDb(_ amplitude: Float) -> Double {
        guard amplitude.isFinite, amplitude > 0 else { return -120 }
        return 20 * log10(Double(amplitude))
    }
    static func meterPosition(_ amplitude: Float) -> Double {
        min(1, max(0, (levelDb(amplitude) + 72) / 78))
    }
    static func parseDb(_ text: String) -> Double? {
        let cleaned = text.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: ",", with: ".")
            .replacingOccurrences(of: "−", with: "-")
        guard let value = Double(cleaned), value.isFinite, (-120...24).contains(value) else { return nil }
        return value
    }
    static func label(_ value: Double) -> String { String(format: "%+.1f", value) }
}
