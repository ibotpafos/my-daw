import AppKit

@MainActor
final class ArrangementToolBar: NSStackView {
    var onSelect: ((ArrangementTool) -> Void)?
    private var buttons: [ArrangementTool: NSButton] = [:]
    private var widths: [NSLayoutConstraint] = []
    private var compact = false
    override init(frame: NSRect) {
        super.init(frame: frame)
        orientation = .horizontal; spacing = 2
        for tool in ArrangementTool.allCases {
            let button = NSButton(title: "\(tool.rawValue)", target: self, action: #selector(selectTool(_:)))
            button.tag = tool.rawValue
            button.image = NSImage(systemSymbolName: tool.symbol, accessibilityDescription: tool.title)
            button.imagePosition = .imageLeading
            button.font = .monospacedDigitSystemFont(ofSize: 9, weight: .medium)
            button.setButtonType(.toggle)
            button.toolTip = "\(tool.rawValue) · \(tool.title)"
            button.setAccessibilityLabel(button.toolTip)
            WorkspaceControlStyle.icon(button)
            let width = button.widthAnchor.constraint(equalToConstant: 34); width.isActive = true; widths.append(width)
            buttons[tool] = button; addArrangedSubview(button)
        }
        display(.pointer)
        setAccessibilityLabel("Инструменты аранжировки · клавиши 1–9")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    func setCompact(_ value: Bool) {
        guard compact != value else { return }
        compact = value
        for width in widths { width.constant = value ? 24 : 34 }
        for button in buttons.values { button.imagePosition = value ? .noImage : .imageLeading }
    }
    func display(_ selected: ArrangementTool) {
        for (tool, button) in buttons {
            button.state = tool == selected ? .on : .off
            button.contentTintColor = tool == selected ? DAWDesignTokens.Color.accent : DAWDesignTokens.Color.text
        }
    }
    @objc private func selectTool(_ sender: NSButton) {
        guard let tool = ArrangementTool(rawValue: sender.tag) else { return }
        onSelect?(tool)
    }
}

/// Transparent document-space feedback; never creates a second editable model.
@MainActor
final class ArrangementGestureOverlay: NSView {
    var rectangles: [NSRect] = [] {
        didSet { for rect in oldValue + rectangles { setNeedsDisplay(rect.insetBy(dx: -3, dy: -3)) } }
    }
    var ramps: [(rect: NSRect, left: Bool, ratio: CGFloat)] = [] {
        didSet { for ramp in oldValue + ramps { setNeedsDisplay(ramp.rect) } }
    }
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
    override func draw(_ dirtyRect: NSRect) {
        for rect in rectangles where rect.intersects(dirtyRect) {
            NSColor.controlAccentColor.withAlphaComponent(0.17).setFill(); rect.fill()
            NSColor.controlAccentColor.setStroke()
            let path = NSBezierPath(rect: rect.insetBy(dx: 0.5, dy: 0.5)); path.lineWidth = 1.5; path.stroke()
        }
        for ramp in ramps where ramp.rect.intersects(dirtyRect) {
            let rect = ramp.rect, width = rect.width * min(1, max(0, ramp.ratio))
            let path = NSBezierPath(); path.lineWidth = 2
            path.move(to: NSPoint(x: ramp.left ? rect.minX : rect.maxX - width,
                                 y: ramp.left ? rect.maxY : rect.minY))
            path.line(to: NSPoint(x: ramp.left ? rect.minX + width : rect.maxX,
                                 y: ramp.left ? rect.minY : rect.maxY))
            NSColor.white.setStroke(); path.stroke()
        }
    }
}
