import AppKit

/// Model-driven drawing primitives; renderers never invent audio or MIDI samples.
struct DAWMeterSample { let peak: CGFloat; let rms: CGFloat }
struct DAWMidiNote { let beat: Double; let duration: Double; let key: Int; let velocity: CGFloat }

enum DAWDataVisuals {
    static func waveformPath(samples: [Float], in rect: CGRect) -> NSBezierPath {
        let path = NSBezierPath(); guard samples.count > 1 else { return path }
        for (index, sample) in samples.enumerated() { let x = rect.minX + rect.width * CGFloat(index) / CGFloat(samples.count - 1); let y = rect.midY - CGFloat(sample) * rect.height * 0.5; index == 0 ? path.move(to: CGPoint(x: x, y: y)) : path.line(to: CGPoint(x: x, y: y)) }; return path
    }
    static func meterColor(for peak: CGFloat) -> NSColor { peak > 0.93 ? DAWDesignTokens.Color.coral : peak > 0.72 ? DAWDesignTokens.Color.warning : DAWDesignTokens.Color.mint }
    static func gridPath(in rect: CGRect, beatWidth: CGFloat, rowHeight: CGFloat) -> NSBezierPath { let path = NSBezierPath(); stride(from: rect.minX, through: rect.maxX, by: max(beatWidth, 1)).forEach { path.move(to: CGPoint(x: $0, y: rect.minY)); path.line(to: CGPoint(x: $0, y: rect.maxY)) }; stride(from: rect.minY, through: rect.maxY, by: max(rowHeight, 1)).forEach { path.move(to: CGPoint(x: rect.minX, y: $0)); path.line(to: CGPoint(x: rect.maxX, y: $0)) }; return path }
}
