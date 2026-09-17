import AppKit

/// Mouse preview is local; only mouse-up calls the existing transport command.
/// System AppKit owns tracking, autoscroll, focus, menus and cursor feedback.
@MainActor
final class TimelineRangeView: NSView {
    var projectFrames: UInt64 = 1 { didSet { if oldValue != projectFrames { cancelGesture() }; needsDisplay = true } }
    var playableFrames: UInt64 = 0 { didSet { if oldValue != playableFrames { cancelGesture() }; needsDisplay = true } }
    var selection: TimelineFrameRange? { didSet { if oldValue != selection { cancelGesture() }; needsDisplay = true } }
    var loopEnabled = false { didSet { if oldValue != loopEnabled { cancelGesture() }; needsDisplay = true } }
    var editingEnabled = true { didSet { if !editingEnabled { cancelGesture() }; needsDisplay = true } }
    var snapFrame: ((UInt64) -> UInt64)?
    var onCommit: ((TimelineFrameRange) -> Bool)?
    var onToggle: (() -> Void)?
    var onClear: (() -> Void)?
    var isGesturing: Bool { gesture != nil }
    private var gesture: TimelineRangeGesture?
    private var mouseDownX: CGFloat = 0
    private var didDrag = false
    private(set) var preview: TimelineFrameRange?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Диапазон цикла")
        setAccessibilityHelp("Потяни пустую полосу для нового диапазона, край для изменения границы, середину для переноса. Shift отключает сетку. Escape отменяет жест. Меню: переключить цикл или очистить диапазон.")
        toolTip = "Потяни — задать цикл · края — изменить · середина — перенести · Shift — без сетки · Esc — отмена"
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func frame(atX x: CGFloat) -> UInt64 {
        guard x.isFinite, bounds.width.isFinite, bounds.width > 0 else { return 0 }
        let fraction = min(1, max(0, Double(x / bounds.width)))
        let duration = min(projectFrames, BeatFrameMap.timelineLimitFrame)
        return min(playableFrames, UInt64((fraction * Double(duration)).rounded()))
    }
    func rect(for range: TimelineFrameRange) -> NSRect {
        let scale = bounds.width / CGFloat(max(1, projectFrames))
        return NSRect(x: CGFloat(range.start) * scale, y: 2,
                      width: max(1, CGFloat(range.length) * scale), height: max(1, bounds.height - 4))
    }
    func cancelGesture() { gesture = nil; preview = nil; didDrag = false; needsDisplay = true }

    override func mouseDown(with event: NSEvent) {
        guard editingEnabled, playableFrames > 0, bounds.contains(convert(event.locationInWindow, from: nil)) else { return }
        if event.modifierFlags.contains(.control) {
            if let menu = menu(for: event) { NSMenu.popUpContextMenu(menu, with: event, for: self) }
            return
        }
        window?.makeFirstResponder(self)
        let point = convert(event.locationInWindow, from: nil)
        var mode = TimelineRangeGesture.Mode.create
        // Option-drag always creates a new range, including inside an old one.
        if let selection, !event.modifierFlags.contains(.option) {
            let rect = rect(for: selection)
            let left = abs(point.x - rect.minX), right = abs(point.x - rect.maxX)
            if min(left, right) <= 6 { mode = left <= right ? .resizeStart : .resizeEnd }
            else if rect.contains(point) { mode = .move }
        }
        mouseDownX = point.x; didDrag = false; preview = nil
        gesture = TimelineRangeGesture(mode: mode, original: selection, anchor: frame(atX: point.x),
                                       limit: min(playableFrames, BeatFrameMap.timelineLimitFrame))
    }
    override func mouseDragged(with event: NSEvent) {
        guard editingEnabled, let gesture else { cancelGesture(); return }
        _ = autoscroll(with: event)
        let point = convert(event.locationInWindow, from: nil)
        guard point.x.isFinite else { return }
        didDrag = didDrag || abs(point.x - mouseDownX) >= 3
        guard didDrag else { return }
        preview = gesture.range(at: frame(atX: point.x)) { [self] frame in
            event.modifierFlags.contains(.shift) ? frame : (snapFrame?(frame) ?? frame)
        }
        needsDisplay = true
    }
    override func mouseUp(with event: NSEvent) {
        guard editingEnabled, gesture != nil else { cancelGesture(); return }
        let result = didDrag ? preview : nil
        cancelGesture()
        // No eager updates: a click, zero-width gesture or Escape is a no-op.
        if let result, result != selection { _ = onCommit?(result) }
    }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53 { cancelGesture(); return }
        if event.modifierFlags.intersection(.deviceIndependentFlagsMask).isEmpty,
           event.keyCode == 51 || event.keyCode == 117 {
            clearRange(); return
        }
        super.keyDown(with: event)
    }
    override func resetCursorRects() {
        super.resetCursorRects()
        guard editingEnabled else { return }
        addCursorRect(bounds, cursor: .crosshair)
        if let selection {
            let rect = rect(for: selection)
            addCursorRect(rect, cursor: .openHand)
            for x in [rect.minX, rect.maxX] {
                addCursorRect(NSRect(x: x - 6, y: 0, width: 12, height: bounds.height).intersection(bounds), cursor: .resizeLeftRight)
            }
        }
    }
    override func menu(for event: NSEvent) -> NSMenu? {
        let menu = NSMenu(); menu.autoenablesItems = false
        let toggle = NSMenuItem(title: loopEnabled ? "Выключить цикл" : "Включить цикл", action: #selector(toggleRange), keyEquivalent: "")
        toggle.target = self; toggle.isEnabled = editingEnabled && selection != nil
        let clear = NSMenuItem(title: "Очистить диапазон", action: #selector(clearRange), keyEquivalent: "")
        clear.target = self; clear.isEnabled = editingEnabled && selection != nil
        menu.addItem(toggle); menu.addItem(clear)
        return menu
    }
    @objc func toggleRange() { guard editingEnabled, selection != nil else { return }; cancelGesture(); onToggle?() }
    @objc func clearRange() { guard editingEnabled else { return }; cancelGesture(); onClear?() }
    override func accessibilityValue() -> Any? {
        guard let selection else { return "Диапазон не задан" }
        return String(format: "%.2f–%.2f секунд, цикл %@", Double(selection.start) / 48000,
                      Double(selection.end) / 48000, loopEnabled ? "включён" : "выключен")
    }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.surface.setFill(); bounds.fill()
        let range = preview ?? selection
        let paragraph = NSMutableParagraphStyle(); paragraph.lineBreakMode = .byTruncatingTail
        let attributes: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedDigitSystemFont(ofSize: 10, weight: .medium),
            .foregroundColor: DAWDesignTokens.Color.text, .paragraphStyle: paragraph]
        if let range {
            let rect = rect(for: range)
            let tint = preview != nil || loopEnabled ? DAWDesignTokens.Color.warning : DAWDesignTokens.Color.secondaryText
            (tint.blended(withFraction: editingEnabled ? 0.70 : 0.88, of: DAWDesignTokens.Color.surface) ?? tint).setFill()
            NSBezierPath(roundedRect: rect, xRadius: 3, yRadius: 3).fill()
            tint.setFill()
            for x in [rect.minX, rect.maxX - 2] { NSRect(x: x, y: 4, width: 2, height: max(1, bounds.height - 8)).fill() }
            if rect.width > 70 {
                let text = String(format: "↻  %.2f – %.2f s", Double(range.start) / 48000, Double(range.end) / 48000)
                (text as NSString).draw(in: rect.insetBy(dx: 8, dy: 3), withAttributes: attributes)
            }
        } else {
            let hint = playableFrames == 0 ? "Цикл: добавь аудио или MIDI" : "Потяни здесь — задать цикл · Shift — без сетки"
            (hint as NSString).draw(in: bounds.insetBy(dx: 8, dy: 5), withAttributes: attributes)
        }
    }
}
