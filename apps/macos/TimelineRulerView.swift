import AppKit

@MainActor
final class DraftCanvas: NSView {
    override var isFlipped: Bool { true }
    override init(frame: NSRect = .zero) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = DAWDesignTokens.Color.canvas.cgColor
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
}

@MainActor
final class AutomationSlider: NSSlider {
    var automationBegin: (() -> Void)?
    var automationEnd: (() -> Void)?
    override func mouseDown(with event: NSEvent) {
        automationBegin?()
        super.mouseDown(with: event)
        automationEnd?()
    }
}

/// Point locator from the session, not a second persisted arrangement section.
@MainActor
struct ProjectMarker {
    let frame: UInt64
    let name: String
}

@MainActor
final class TimelineRulerView: NSView {
    var projectFrames: UInt64 = 48000 * 12 { didSet { needsDisplay = true } }
    var playhead: UInt64 = 0 { didSet { needsDisplay = true } }
    var barMarks: [ProjectBarStart] = [] { didSet { needsDisplay = true } }
    var markers: [ProjectMarker] = [] { didSet { needsDisplay = true } }
    var onMarkerSeek: ((UInt64) -> Void)?
    var onMarkerAdd: ((UInt64) -> Void)?
    var onMarkerMenu: ((ProjectMarker) -> NSMenu?)?
    let cycleRange = TimelineRangeView(frame: .zero)
    static let cycleBand: CGFloat = 24
    static var preferredHeight: CGFloat { markerBand + barBand + 28 + cycleBand }
    static let barBand: CGFloat = 20
    static let markerBand: CGFloat = 28
    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(cycleRange)
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func layout() {
        super.layout()
        cycleRange.frame = NSRect(x: 0, y: bounds.height - Self.cycleBand, width: bounds.width, height: Self.cycleBand)
    }

    func frame(atX x: CGFloat) -> UInt64 {
        guard bounds.width.isFinite, bounds.width > 0, x.isFinite else { return 0 }
        let ratio = min(1, max(0, x / bounds.width))
        if ratio == 1 { return projectFrames }
        let value = Double(ratio) * Double(projectFrames)
        return value >= Double(projectFrames) ? projectFrames : UInt64(value)
    }
    private func x(atFrame frame: UInt64) -> CGFloat {
        CGFloat(Double(min(frame, projectFrames)) / Double(max(1, projectFrames))) * bounds.width
    }
    var markerRects: [(marker: ProjectMarker, rect: NSRect)] {
        let visible = markers.filter { $0.frame <= projectFrames }.sorted { $0.frame < $1.frame }
        return visible.enumerated().map { index, marker in
            let start = x(atFrame: marker.frame)
            let end = index + 1 < visible.count ? x(atFrame: visible[index + 1].frame) : bounds.width
            let width = max(0, min(180, end - start - 3))
            return (marker, NSRect(x: start, y: 3, width: width, height: Self.markerBand - 6))
        }
    }
    func marker(at point: NSPoint) -> ProjectMarker? {
        guard point.x.isFinite, point.y.isFinite, point.x >= 0, point.x <= bounds.width,
              point.y >= 0, point.y < Self.markerBand else { return nil }
        // Flag hit target remains available when tightly packed labels collapse.
        if let closest = markerRects.min(by: { abs($0.rect.minX - point.x) < abs($1.rect.minX - point.x) }),
           abs(closest.rect.minX - point.x) <= 4 { return closest.marker }
        return markerRects.first(where: { $0.rect.contains(point) })?.marker
    }
    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard bounds.contains(point) else { return }
        if event.modifierFlags.contains(.control) {
            if let menu = menu(for: event) { NSMenu.popUpContextMenu(menu, with: event, for: self) }
        } else if event.clickCount >= 2, point.y < Self.markerBand {
            onMarkerAdd?(frame(atX: point.x))
        } else {
            onMarkerSeek?(marker(at: point)?.frame ?? frame(atX: point.x))
        }
    }
    override func menu(for event: NSEvent) -> NSMenu? {
        guard let marker = marker(at: convert(event.locationInWindow, from: nil)) else { return nil }
        return onMarkerMenu?(marker)
    }
    private func line(at x: CGFloat, from start: CGFloat, to end: CGFloat, color: NSColor) {
        color.setStroke()
        let path = NSBezierPath()
        path.move(to: NSPoint(x: x, y: start)); path.line(to: NSPoint(x: x, y: end))
        path.lineWidth = 1; path.stroke()
    }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.canvas.setFill(); bounds.fill()
        let palette: [NSColor] = [.systemBlue, .systemGreen, .systemPurple, .systemOrange, .systemTeal]
        let paragraph = NSMutableParagraphStyle(); paragraph.lineBreakMode = .byTruncatingTail
        let markerText: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11, weight: .medium),
            .foregroundColor: DAWDesignTokens.Color.text, .paragraphStyle: paragraph
        ]
        for (index, entry) in markerRects.enumerated() {
            let tint = palette[index % palette.count]
            let fill = tint.blended(withFraction: 0.78, of: DAWDesignTokens.Color.surface) ?? DAWDesignTokens.Color.surface
            fill.setFill(); NSBezierPath(roundedRect: entry.rect, xRadius: 3, yRadius: 3).fill()
            line(at: entry.rect.minX + 0.5, from: 3, to: Self.markerBand - 3, color: tint)
            guard entry.rect.width > 22 else { continue }
            let textRect = entry.rect.insetBy(dx: 7, dy: 3)
            (entry.marker.name as NSString).draw(in: textRect, withAttributes: markerText)
        }
        if markers.isEmpty {
            ("Двойной клик — добавить маркер" as NSString).draw(
                in: NSRect(x: 8, y: 8, width: max(0, bounds.width - 16), height: 16),
                withAttributes: [.font: NSFont.systemFont(ofSize: 10), .foregroundColor: DAWDesignTokens.Color.secondaryText])
        }
        let baseline = Self.markerBand + Self.barBand
        let barText: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .medium),
            .foregroundColor: DAWDesignTokens.Color.text
        ]
        var lastLabelX: CGFloat = -40
        for mark in barMarks where mark.frame <= projectFrames {
            let x = x(atFrame: mark.frame)
            line(at: x, from: baseline - 5, to: baseline, color: DAWDesignTokens.Color.border)
            guard x - lastLabelX >= 28 else { continue }
            lastLabelX = x
            String(mark.number).draw(at: NSPoint(x: x + 4, y: Self.markerBand + 3), withAttributes: barText)
        }
        let seconds = Double(max(1, projectFrames)) / 48000
        let raw = seconds / max(1, Double(bounds.width / 100))
        let candidates: [Double] = [1, 2, 5, 10, 20, 30, 60, 120, 300, 600]
        let step = candidates.first(where: { $0 >= raw }) ?? max(600, raw)
        let secondsText: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .regular),
            .foregroundColor: DAWDesignTokens.Color.secondaryText
        ]
        // Bound the loop even for a malformed/very large displayed duration.
        for tick in 0...min(1024, Int(min(1024, seconds / step))) {
            let second = Double(tick) * step
            let x = CGFloat(second / seconds) * bounds.width
            line(at: x, from: bounds.height - Self.cycleBand - 6, to: bounds.height - Self.cycleBand, color: DAWDesignTokens.Color.border)
            String(format: "%.0f s", second).draw(at: NSPoint(x: x + 4, y: baseline + 4), withAttributes: secondsText)
        }
        let cursor = x(atFrame: playhead)
        let cursorBottom = bounds.height - Self.cycleBand
        line(at: cursor, from: Self.markerBand, to: cursorBottom, color: DAWDesignTokens.Color.accent)
        DAWDesignTokens.Color.accent.setFill()
        let pointer = NSBezierPath()
        pointer.move(to: NSPoint(x: cursor - 4, y: cursorBottom - 6))
        pointer.line(to: NSPoint(x: cursor + 4, y: cursorBottom - 6))
        pointer.line(to: NSPoint(x: cursor, y: cursorBottom)); pointer.close(); pointer.fill()
    }
}
