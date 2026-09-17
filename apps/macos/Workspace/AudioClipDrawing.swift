import AppKit

/// Presentation only: uses the same peak cache, source offset and fade lengths
/// as clip editing. It never changes audio or derives fake plugin/signal data.
@MainActor
enum AudioClipDrawing {
    static func color(for clip: ClipGeometry, accent: NSColor) -> NSColor {
        clip.color == 0 ? accent : dawColorFromHex(clip.color)
    }
    static func fadePositions(_ clip: ClipGeometry, in rect: NSRect) -> (CGFloat?, CGFloat?) {
        guard clip.length > 0 else { return (nil, nil) }
        let fadeIn = clip.fadeIn == 0 ? nil : rect.minX + rect.width * CGFloat(Double(min(clip.fadeIn, clip.length)) / Double(clip.length))
        let fadeOut = clip.fadeOut == 0 ? nil : rect.maxX - rect.width * CGFloat(Double(min(clip.fadeOut, clip.length)) / Double(clip.length))
        return (fadeIn, fadeOut)
    }
    static func draw(_ clip: ClipGeometry, title: String, index: Int, rect: NSRect, accent: NSColor,
                     selected: Bool, hovered: Bool, fallbackPeaks: [Float], sourceFrames: UInt64) {
        guard rect.width > 0, rect.height > 0, clip.length > 0 else { return }
        let color = color(for: clip, accent: accent)
        let path = NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4)
        color.withAlphaComponent(clip.muted ? 0.12 : (selected ? 0.38 : 0.22)).setFill(); path.fill()
        NSGraphicsContext.saveGraphicsState()
        path.addClip()
        defer { NSGraphicsContext.restoreGraphicsState() }
        let header = NSRect(x: rect.minX, y: rect.minY, width: rect.width, height: min(14, rect.height))
        color.withAlphaComponent(selected ? 0.42 : 0.25).setFill(); header.fill()
        let waveRect = NSRect(x: rect.minX + 1, y: header.maxY + 1, width: max(0, rect.width - 2), height: max(0, rect.maxY - header.maxY - 3))
        let peaks = clip.sourcePeaks.isEmpty ? fallbackPeaks : clip.sourcePeaks
        let frames = clip.sourceFramesForTake > 0 ? clip.sourceFramesForTake : sourceFrames
        let maximum = max(0.000001, peaks.filter { $0.isFinite }.max() ?? 0)
        let waveform = NSBezierPath(); waveform.lineWidth = 1
        let visibleStart = Double(clip.sourceOffset)
        let visibleEnd = visibleStart + Double(clip.length)
        for (i, peak) in peaks.enumerated() where peak.isFinite {
            let begin = max(Double(i) / Double(peaks.count) * Double(frames), visibleStart)
            let end = min(Double(i + 1) / Double(peaks.count) * Double(frames), visibleEnd)
            guard end > begin else { continue }
            let x = rect.minX + CGFloat(((begin + end) * 0.5 - visibleStart) / Double(clip.length)) * rect.width
            let height = CGFloat(max(0, min(1, peak / maximum))) * waveRect.height * 0.46
            waveform.move(to: NSPoint(x: x, y: waveRect.midY - height))
            waveform.line(to: NSPoint(x: x, y: waveRect.midY + height))
        }
        let ink = color.blended(withFraction: 0.30, of: .white) ?? color
        ink.withAlphaComponent(clip.muted ? 0.25 : 0.95).setStroke(); waveform.stroke()
        // Only real nonzero fades are drawn as ramps. Zero-fade handles remain
        // discoverable on hover/selection without pretending a fade is applied.
        let fades = fadePositions(clip, in: rect)
        NSColor.white.withAlphaComponent(selected || hovered ? 0.75 : 0.38).setStroke()
        if let end = fades.0 {
            let ramp = NSBezierPath(); ramp.move(to: NSPoint(x: rect.minX, y: rect.maxY))
            ramp.line(to: NSPoint(x: end, y: waveRect.minY)); ramp.stroke()
        }
        if let start = fades.1 {
            let ramp = NSBezierPath(); ramp.move(to: NSPoint(x: start, y: waveRect.minY))
            ramp.line(to: NSPoint(x: rect.maxX, y: rect.maxY)); ramp.stroke()
        }
        if selected || hovered {
            ink.setFill()
            for x in [rect.minX + 1, rect.maxX - 3] {
                NSRect(x: x, y: waveRect.midY - 5, width: 2, height: 10).fill()
            }
            for x in [rect.minX + min(12, rect.width * 0.2), rect.maxX - min(12, rect.width * 0.2)] {
                NSBezierPath(ovalIn: NSRect(x: x - 2, y: rect.maxY - 5, width: 4, height: 4)).fill()
            }
        }
        let paragraph = NSMutableParagraphStyle(); paragraph.lineBreakMode = .byTruncatingTail
        let tag = "\(title) · \(index + 1)\(clip.looped ? " · ↻" : "")\(clip.muted ? " · MUTE" : "")\(clip.gainDb == 0 ? "" : String(format: " · %+.1f dB", clip.gainDb))"
        (tag as NSString).draw(in: header.insetBy(dx: 6, dy: 1), withAttributes: [
            .font: NSFont.systemFont(ofSize: 9, weight: .medium), .foregroundColor: DAWDesignTokens.Color.text,
            .paragraphStyle: paragraph
        ])
        (selected ? ink : color.withAlphaComponent(hovered ? 0.90 : 0.65)).setStroke()
        path.lineWidth = selected ? 2 : 1; path.stroke()
    }
}
