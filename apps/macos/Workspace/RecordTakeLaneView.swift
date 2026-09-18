import AppKit

struct RecordTakeSnapshot {
    let index: UInt32
    let title: String
    let start: UInt64
    let frames: UInt64
    let peaks: [Float]
}

/// Reuses Arrange's peak renderer. The mouse-up command is revision-bound.
@MainActor
final class RecordTakeLaneView: NSView {
    var title = ""
    var take: RecordTakeSnapshot?
    var clips: [ClipGeometry] = []
    var documentID = UUID()
    var trackID: UInt64 = 0
    var revision: UInt64 = 0
    var viewStart: UInt64 = 0
    var viewEnd: UInt64 = 1
    var editable = false
    var selected = false
    var playhead: UInt64 = 0 { didSet { needsDisplay = true } }
    var preview: TimelineFrameRange? { didSet { needsDisplay = true } }
    var onSelect: ((UInt32) -> Void)?
    var onCommit: ((RecordCompRequest) -> Void)?
    var onPreview: ((TimelineFrameRange?) -> Void)?
    var onTogglePlayback: (() -> Void)?
    private var swipe: RecordCompSwipe?
    var isGesturing: Bool { swipe != nil }
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    var timelineRect: NSRect { NSRect(x: 148, y: 7, width: max(1, bounds.width - 158), height: max(1, bounds.height - 14)) }
    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true); setAccessibilityRole(.button)
        setAccessibilityHelp("Протяните по дублю для сборки фразы. Escape отменяет жест. Пробел — воспроизведение. Доступны также поля диапазона и кнопка В comp.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func resetCursorRects() {
        super.resetCursorRects()
        if editable && take != nil { addCursorRect(timelineRect, cursor: .crosshair) }
    }
    func frame(at x: CGFloat) -> UInt64 {
        guard x.isFinite, viewEnd > viewStart else { return viewStart }
        let fraction = min(1, max(0, Double((x - timelineRect.minX) / timelineRect.width)))
        return viewStart + UInt64((Double(viewEnd - viewStart) * fraction).rounded())
    }
    func x(for frame: UInt64) -> CGFloat {
        timelineRect.minX + CGFloat((Double(frame) - Double(viewStart)) / Double(max(1, viewEnd - viewStart))) * timelineRect.width
    }
    func cancelSwipe() { swipe = nil; preview = nil; onPreview?(nil) }
    override func viewWillMove(toWindow newWindow: NSWindow?) {
        if newWindow == nil { cancelSwipe() }
        super.viewWillMove(toWindow: newWindow)
    }
    override func mouseDown(with event: NSEvent) {
        guard editable, let take, !isHiddenOrHasHiddenAncestor else { return }
        guard window?.makeFirstResponder(self) == true else { return }
        let point = convert(event.locationInWindow, from: nil)
        onSelect?(take.index)
        guard timelineRect.contains(point) else { return }
        swipe = RecordCompSwipe(documentID: documentID, trackID: trackID, takeIndex: take.index, revision: revision,
                                takeStart: take.start, takeFrames: take.frames, anchor: frame(at: point.x))
    }
    override func mouseDragged(with event: NSEvent) {
        guard editable, let swipe else { return }
        preview = swipe.request(at: frame(at: convert(event.locationInWindow, from: nil).x))?.range
        onPreview?(preview)
    }
    override func mouseUp(with event: NSEvent) {
        guard let gesture = swipe else { return }
        let request = gesture.request(at: frame(at: convert(event.locationInWindow, from: nil).x))
        swipe = nil; preview = nil
        guard editable, !isHiddenOrHasHiddenAncestor, let request else { onPreview?(nil); return }
        onCommit?(request)
    }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53 { cancelSwipe() }
        else if event.keyCode == 49 && event.modifierFlags.intersection([.command, .control, .option]).isEmpty {
            if !isGesturing { onTogglePlayback?() }
        } else if event.keyCode == 36 || event.keyCode == 76 { _ = accessibilityPerformPress() }
        else { super.keyDown(with: event) }
    }
    override func cancelOperation(_ sender: Any?) { cancelSwipe() }
    override func accessibilityPerformPress() -> Bool {
        guard editable, let take else { return false }
        onSelect?(take.index); return true
    }
    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        (selected ? DAWDesignTokens.Color.accent.withAlphaComponent(0.10) : DAWDesignTokens.Color.surface).setFill()
        bounds.fill()
        (title as NSString).draw(in: NSRect(x: 12, y: 12, width: 128, height: 36), withAttributes: [
            .font: NSFont.systemFont(ofSize: 12, weight: .semibold), .foregroundColor: DAWDesignTokens.Color.text])
        let detail = take.map { String(format: "%.2f с · дубль %d", Double($0.frames) / 48000, $0.index + 1) } ?? "Итоговая сборка"
        (detail as NSString).draw(in: NSRect(x: 12, y: 49, width: 132, height: 26), withAttributes: [
            .font: NSFont.systemFont(ofSize: 10), .foregroundColor: DAWDesignTokens.Color.secondaryText])
        NSGraphicsContext.saveGraphicsState(); NSBezierPath(rect: timelineRect).addClip()
        defer { NSGraphicsContext.restoreGraphicsState() }
        DAWDesignTokens.Color.canvas.setFill(); timelineRect.fill()
        DAWDesignTokens.Color.border.setStroke()
        for i in 0...8 {
            let line = NSBezierPath(); let x = timelineRect.minX + CGFloat(i) * timelineRect.width / 8
            line.move(to: NSPoint(x: x, y: 0)); line.line(to: NSPoint(x: x, y: bounds.height)); line.stroke()
        }
        let rendered: [ClipGeometry]
        if let take {
            rendered = [ClipGeometry(start: take.start, sourceOffset: 0, length: take.frames,
                fadeIn: 0, fadeOut: 0, takeIndex: take.index, sourceFramesForTake: take.frames, sourcePeaks: take.peaks)]
        } else { rendered = clips }
        let palette: [NSColor] = [.systemPurple, .systemBlue, .systemTeal, .systemGreen]
        for clip in rendered where clip.length > 0 {
            let rect = NSRect(x: x(for: clip.start), y: timelineRect.minY,
                width: CGFloat(Double(clip.length) / Double(max(1, viewEnd - viewStart))) * timelineRect.width,
                height: timelineRect.height)
            guard rect.intersects(timelineRect) else { continue }
            AudioClipDrawing.draw(clip, title: "Дубль", index: Int(clip.takeIndex), rect: rect,
                accent: palette[Int(clip.takeIndex) % palette.count], selected: false, hovered: false,
                fallbackPeaks: [], sourceFrames: clip.sourceFramesForTake)
        }
        if let take {
            for clip in clips where clip.takeIndex == take.index && !clip.looped && clip.sourceOffset <= take.frames && clip.length <= take.frames - clip.sourceOffset {
                let start = take.start + clip.sourceOffset
                DAWDesignTokens.Color.mint.setFill()
                NSRect(x: x(for: start), y: timelineRect.maxY - 5,
                       width: x(for: start + clip.length) - x(for: start), height: 4).fill()
            }
        }
        if let preview {
            let rect = NSRect(x: x(for: preview.start), y: timelineRect.minY,
                width: x(for: preview.end) - x(for: preview.start), height: timelineRect.height)
            NSColor.white.withAlphaComponent(0.15).setFill(); rect.fill()
            NSColor.white.withAlphaComponent(0.8).setStroke(); NSBezierPath(rect: rect).stroke()
        }
        if playhead >= viewStart && playhead <= viewEnd {
            NSColor.white.withAlphaComponent(0.8).setStroke()
            let line = NSBezierPath(); line.move(to: NSPoint(x: x(for: playhead), y: 0))
            line.line(to: NSPoint(x: x(for: playhead), y: bounds.height)); line.stroke()
        }
    }
}
