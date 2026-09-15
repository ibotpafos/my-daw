import AppKit

/// Stable visual primitives for the DAW shell. Keep semantic names at call sites.
enum DAWDesignTokens {
    enum Color {
        static let canvas = NSColor(srgbRed: 0.075, green: 0.086, blue: 0.112, alpha: 1)
        static let surface = NSColor(srgbRed: 0.105, green: 0.118, blue: 0.151, alpha: 1)
        static let raisedSurface = NSColor(srgbRed: 0.135, green: 0.149, blue: 0.188, alpha: 1)
        static let border = NSColor(srgbRed: 0.235, green: 0.251, blue: 0.310, alpha: 1)
        static let text = NSColor(srgbRed: 0.890, green: 0.878, blue: 0.970, alpha: 1)
        static let secondaryText = NSColor(srgbRed: 0.620, green: 0.627, blue: 0.735, alpha: 1)
        static let accent = NSColor(srgbRed: 0.650, green: 0.460, blue: 0.980, alpha: 1)
        static let mint = NSColor(srgbRed: 0.380, green: 0.820, blue: 0.690, alpha: 1)
        static let coral = NSColor(srgbRed: 0.950, green: 0.490, blue: 0.510, alpha: 1)
        static let warning = NSColor(srgbRed: 0.950, green: 0.760, blue: 0.330, alpha: 1)
        static let success = NSColor(srgbRed: 0.370, green: 0.830, blue: 0.590, alpha: 1)
    }
    enum Space { static let xxs: CGFloat = 4; static let xs: CGFloat = 8; static let sm: CGFloat = 12; static let md: CGFloat = 16; static let lg: CGFloat = 24; static let xl: CGFloat = 32 }
    enum Radius { static let control: CGFloat = 7; static let card: CGFloat = 10; static let panel: CGFloat = 14; static let pill: CGFloat = 999 }
    enum Typography {
        static var caption: NSFont { NSFont.systemFont(ofSize: 11, weight: .medium) }
        static var body: NSFont { NSFont.systemFont(ofSize: 13, weight: .regular) }
        static var label: NSFont { NSFont.systemFont(ofSize: 13, weight: .semibold) }
        static var title: NSFont { NSFont.systemFont(ofSize: 18, weight: .semibold) }
    }
}

enum DAWControlState: String, CaseIterable { case normal, hover, selected, disabled, recording, focused, error }

extension DAWControlState {
    var foreground: NSColor { switch self { case .disabled: return DAWDesignTokens.Color.secondaryText.withAlphaComponent(0.45); case .error: return DAWDesignTokens.Color.coral; default: return DAWDesignTokens.Color.text } }
    var fill: NSColor { switch self { case .hover: return DAWDesignTokens.Color.raisedSurface; case .selected, .focused: return DAWDesignTokens.Color.accent.withAlphaComponent(0.22); case .recording: return DAWDesignTokens.Color.coral.withAlphaComponent(0.20); case .error: return DAWDesignTokens.Color.coral.withAlphaComponent(0.14); default: return DAWDesignTokens.Color.surface } }
}
