import AppKit

@MainActor
final class MixerActionButton: NSButton {
    var invoke: (() -> Void)?
    var contextMenu: (() -> NSMenu)?
    var invokeWithModifiers: ((NSEvent.ModifierFlags) -> Void)?
    private var clickModifiers: NSEvent.ModifierFlags = []
    override func mouseDown(with event: NSEvent) {
        clickModifiers = event.modifierFlags
        defer { clickModifiers = [] }
        super.mouseDown(with: event)
    }
    init(_ title: String, action: (() -> Void)? = nil) {
        super.init(frame: .zero)
        self.title = title; invoke = action
        bezelStyle = .texturedRounded; controlSize = .small
        font = .systemFont(ofSize: 11, weight: .medium)
        target = self; self.action = #selector(fire)
        lineBreakMode = .byTruncatingTail
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func fire() {
        if let invokeWithModifiers { invokeWithModifiers(clickModifiers) } else { invoke?() }
    }
    override func menu(for event: NSEvent) -> NSMenu? { contextMenu?() ?? super.menu(for: event) }
}
@MainActor
final class MixerMenuItem: NSMenuItem {
    private var invoke: (() -> Void)?
    init(_ title: String, enabled: Bool = true, action: @escaping () -> Void) {
        super.init(title: title, action: nil, keyEquivalent: "")
        invoke = action; target = self; self.action = #selector(fire); isEnabled = enabled
    }
    required init(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func fire() { invoke?() }
}
@MainActor
final class MixerMeterView: NSView {
    var snapshot = MixerMeterSnapshot() {
        didSet {
            clipped = clipped || snapshot.leftPeak >= 1 || snapshot.rightPeak >= 1
            needsDisplay = true
        }
    }
    private(set) var clipped = false
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true); setAccessibilityRole(.levelIndicator)
        toolTip = "Stereo sample peak · −72…+6 dBFS. Click to reset clip. Not a true-peak meter."
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func resetClip() { clipped = false; needsDisplay = true }
    override func mouseDown(with event: NSEvent) { resetClip() }
    override func accessibilityValue() -> Any? {
        let peak = max(snapshot.leftPeak, snapshot.rightPeak)
        return peak > 0 ? String(format: "%.1f dBFS%@", MixerScale.levelDb(peak), clipped ? ", clip" : "") : "Silence"
    }
    override func draw(_ dirtyRect: NSRect) {
        let area = bounds.insetBy(dx: 2, dy: 7)
        DAWDesignTokens.Color.canvas.setFill(); bounds.fill()
        for (i, peak) in [snapshot.leftPeak, snapshot.rightPeak].enumerated() {
            let width = max(2, (area.width - 3) / 2)
            let x = area.minX + CGFloat(i) * (width + 3)
            let normalized = MixerScale.meterPosition(peak)
            // Fixed dB color boundaries do not change with the instantaneous peak.
            let bands: [(Double, Double, NSColor)] = [(-72,-18,.systemMint),(-18,-6,.systemGreen),(-6,0,.systemYellow),(0,6,.systemRed)]
            for (lo, hi, color) in bands {
                let low = (lo + 72) / 78, high = min(normalized, (hi + 72) / 78)
                guard high > low else { continue }
                color.withAlphaComponent(0.86).setFill()
                NSRect(x: x, y: area.maxY - area.height * high, width: width, height: area.height * (high - low)).fill()
            }
            let hold = i == 0 ? snapshot.leftHold : snapshot.rightHold
            if hold > 0 {
                NSColor.white.withAlphaComponent(0.8).setFill()
                NSRect(x: x, y: area.maxY - area.height * MixerScale.meterPosition(hold), width: width, height: 1).fill()
            }
        }
        (clipped ? NSColor.systemRed : DAWDesignTokens.Color.border).setFill()
        NSBezierPath(roundedRect: NSRect(x: 2, y: 0, width: max(1,bounds.width-4), height: 4), xRadius: 1, yRadius: 1).fill()
    }
}

@MainActor
private final class MixerFaderCell: NSSliderCell {
    override func drawBar(inside rect: NSRect, flipped: Bool) {
        let rail = NSRect(x: rect.midX - 2, y: rect.minY, width: 4, height: rect.height)
        DAWDesignTokens.Color.canvas.setFill()
        NSBezierPath(roundedRect: rail, xRadius: 2, yRadius: 2).fill()
    }
    override func drawKnob(_ rect: NSRect) {
        let cap = NSRect(x: rect.midX - 10, y: rect.midY - 8, width: 20, height: 16)
        (isEnabled ? NSColor(white: 0.82, alpha: 1) : NSColor(white: 0.35, alpha: 1)).setFill()
        NSBezierPath(roundedRect: cap, xRadius: 3, yRadius: 3).fill()
        NSColor(white: 0.14, alpha: 1).setFill()
        NSRect(x: cap.minX + 2, y: cap.midY, width: cap.width - 4, height: 1).fill()
    }
}
@MainActor
final class MixerFaderView: NSSlider {
    var onCancel: (() -> Void)?
    private var canceled = false
    private var trackingStart: Double = 0
    func displayPreview(_ value: Double) { doubleValue = MixerScale.position(value) }
    override func cancelOperation(_ sender: Any?) {
        guard tracking else { super.cancelOperation(sender); return }
        canceled = true; displayPreview(trackingStart); onCancel?()
    }
    var onBegin: (() -> Void)?
    var onChange: ((Double) -> Void)?
    var onEnd: ((Double) -> Void)?
    private(set) var tracking = false
    var valueDb: Double {
        get { MixerScale.decibels(doubleValue) }
        set { if !tracking { doubleValue = MixerScale.position(newValue) } }
    }
    override init(frame: NSRect) {
        super.init(frame: frame)
        cell = MixerFaderCell()
        minValue = 0; maxValue = 1; doubleValue = MixerScale.position(0)
        isVertical = true; isContinuous = true; altIncrementValue = 0.001
        target = self; action = #selector(changed)
        setAccessibilityHelp("Up/Down: 0.5 dB. Shift: 0.1 dB. Double-click: unity. Option: fine drag.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func changed() { if !canceled { onChange?(valueDb) } }
    override func mouseDown(with event: NSEvent) {
        guard isEnabled else { return }
        window?.makeFirstResponder(self)
        if event.clickCount == 2 { commit(0); return }
        canceled = false; trackingStart = valueDb
        tracking = true; onBegin?()
        super.mouseDown(with: event)
        tracking = false
        if canceled { displayPreview(trackingStart) } else { onEnd?(valueDb) }
        canceled = false
    }
    func commit(_ value: Double) {
        guard isEnabled, value.isFinite else { return }
        onBegin?(); doubleValue = MixerScale.position(value); onChange?(valueDb); onEnd?(valueDb)
        NSAccessibility.post(element: self, notification: .valueChanged)
    }
    override func keyDown(with event: NSEvent) {
        let step = event.modifierFlags.contains(.shift) ? 0.1 : 0.5
        switch event.keyCode {
        case 125,123: commit(valueDb - step)
        case 126,124: commit(valueDb + step)
        default: super.keyDown(with: event)
        }
    }
    override func accessibilityValue() -> Any? { "\(MixerScale.label(valueDb)) dB" }
    override func accessibilityPerformIncrement() -> Bool { guard isEnabled else { return false }; commit(valueDb + 0.5); return true }
    override func accessibilityPerformDecrement() -> Bool { guard isEnabled else { return false }; commit(valueDb - 0.5); return true }
}

/// Native balance control. Model refreshes never fight a tracked gesture.
@MainActor
final class MixerBalanceView: NSSlider {
    var onBegin: (() -> Void)?
    var onChange: ((Double) -> Void)?
    var onEnd: ((Double) -> Void)?
    private(set) var tracking = false
    var value: Double {
        get { doubleValue }
        set { if !tracking { doubleValue = newValue } }
    }
    override init(frame: NSRect) {
        super.init(frame: frame)
        minValue = -1; maxValue = 1; doubleValue = 0
        isContinuous = true; altIncrementValue = 0.005; controlSize = .small
        target = self; action = #selector(changed)
        setAccessibilityHelp("Stereo balance. Arrows: 2 percent, Shift: 0.5 percent. Double-click: center. Option: fine drag.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    @objc private func changed() {
        if tracking { onChange?(doubleValue) }
        else { commit(doubleValue) }
    }
    override func mouseDown(with event: NSEvent) {
        guard isEnabled else { return }
        window?.makeFirstResponder(self)
        if event.clickCount == 2 { commit(0); return }
        tracking = true; onBegin?()
        super.mouseDown(with: event)
        tracking = false; onEnd?(doubleValue)
    }
    func commit(_ value: Double) {
        guard isEnabled, value.isFinite else { return }
        onBegin?(); doubleValue = max(-1, min(1, value))
        onChange?(doubleValue); onEnd?(doubleValue)
        NSAccessibility.post(element: self, notification: .valueChanged)
    }
    override func keyDown(with event: NSEvent) {
        guard event.modifierFlags.intersection([.command,.option,.control]).isEmpty else { super.keyDown(with:event); return }
        let step = event.modifierFlags.contains(.shift) ? 0.005 : 0.02
        switch event.keyCode {
        case 123,125: commit(doubleValue - step)
        case 124,126: commit(doubleValue + step)
        default: super.keyDown(with: event)
        }
    }
    override func accessibilityValue() -> Any? {
        doubleValue == 0 ? "Center" : String(format: "%@ %.1f percent",doubleValue < 0 ? "Left" : "Right",abs(doubleValue)*100)
    }
    override func accessibilityPerformIncrement() -> Bool { guard isEnabled else { return false }; commit(doubleValue + 0.02); return true }
    override func accessibilityPerformDecrement() -> Bool { guard isEnabled else { return false }; commit(doubleValue - 0.02); return true }
}
