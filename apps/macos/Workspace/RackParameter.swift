import Foundation

/// A projection of the host's published native range, not DSP or a vendor UI.
/// VST3 currently publishes normalized 0...1 through the existing bridge; its
/// indexed flag does not provide a step count, so we must not round to integers.
struct RackParameter: Equatable {
    let id: UInt32
    let name: String
    let minimum: Double
    let maximum: Double
    let value: Double
    let writable: Bool
    let logarithmic: Bool

    init?(id: UInt32, name: String, minimum: Double, maximum: Double,
          value: Double, writable: Bool, logarithmic: Bool) {
        guard minimum.isFinite, maximum.isFinite, value.isFinite,
              minimum <= maximum, abs(minimum) <= Double(Float.greatestFiniteMagnitude),
              abs(maximum) <= Double(Float.greatestFiniteMagnitude) else { return nil }
        self.id = id; self.name = name; self.minimum = minimum; self.maximum = maximum
        self.value = min(maximum, max(minimum, value))
        self.writable = writable && minimum < maximum
        self.logarithmic = logarithmic && minimum > 0 && maximum > minimum
    }
    var position: Double {
        guard minimum < maximum else { return 0 }
        if logarithmic { return (log(value) - log(minimum)) / (log(maximum) - log(minimum)) }
        return (value - minimum) / (maximum - minimum)
    }
    func nativeValue(at position: Double) -> Double? {
        guard position.isFinite, (0...1).contains(position) else { return nil }
        if position == 0 { return minimum }; if position == 1 { return maximum }
        let result = logarithmic ? exp(log(minimum) + position * (log(maximum) - log(minimum)))
            : minimum + position * (maximum - minimum)
        return min(maximum, max(minimum, result))
    }
    func accepts(_ value: Double) -> Bool { writable && value.isFinite && (minimum...maximum).contains(value) }
    func parse(_ text: String) -> Double? {
        let text = text.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")
        guard let number = Double(text), accepts(number) else { return nil }; return number
    }
    var displayValue: String { String(format: "%.5g", value) }
}
