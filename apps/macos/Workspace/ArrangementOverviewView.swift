import AppKit

struct ArrangementOverviewClip {
    let start: UInt64
    let length: UInt64
    let color: NSColor
}

struct ArrangementOverviewContentKey: Equatable {
    let documentID: UUID
    let revision: UInt64
    let frames: UInt64
}

/// A miniature projection, not a second timeline or transport. The native clip
/// view owns all scrolling/clamping; no C ABI or audio calls live in this view.
@MainActor
final class ArrangementOverviewView: NSView {
    private(set) var contentKey: ArrangementOverviewContentKey?
    private(set) var tracks: [[ArrangementOverviewClip]] = []
    private(set) var contentGeneration = 0
    private(set) var geometry = ArrangementOverviewGeometry(documentWidth: 1, viewportX: 0, viewportWidth: 1)
    weak var scrollView: NSScrollView?
    var onFitAll: (() -> Void)?
    var playhead: UInt64 = 0 { didSet { if playhead != oldValue { needsDisplay = true } } }
    var cycle: TimelineFrameRange? { didSet { if cycle != oldValue { needsDisplay = true } } }
    private var paths: [(NSColor, NSBezierPath)] = []
    private var pathSize: NSSize = .zero
    private var drag: (start: Double, pointerOffset: Double)?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    var hasGesture: Bool { drag != nil }
    var mapRect: NSRect { bounds.insetBy(dx: 6, dy: 5) }
    var viewportRect: NSRect {
        let map = mapRect
        return NSRect(x: map.minX + map.width * geometry.start, y: map.minY,
                      width: map.width * geometry.extent, height: map.height)
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true)
        setAccessibilityRole(.slider)
        setAccessibilityLabel("Обзор проекта: горизонтальная прокрутка")
        setAccessibilityHelp("Клик или перетаскивание рамки перемещают видимую область, не курсор воспроизведения. Стрелки и Home/End — навигация; двойной клик — весь проект; Escape — отмена перетаскивания.")
        toolTip = "Обзор проекта · перетащи рамку для прокрутки · двойной клик — весь проект"
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    func bind(to scroll: NSScrollView) {
        NotificationCenter.default.removeObserver(self)
        scrollView = scroll
        drag = nil
        for view in [scroll.contentView, scroll.documentView].compactMap({ $0 }) {
            view.postsBoundsChangedNotifications = true
            view.postsFrameChangedNotifications = true
            for name in [NSView.boundsDidChangeNotification, NSView.frameDidChangeNotification] {
                NotificationCenter.default.addObserver(self, selector: #selector(viewportChanged(_:)), name: name, object: view)
            }
        }
        synchronizeViewport()
    }

    func updateContent(key: ArrangementOverviewContentKey, tracks: [[ArrangementOverviewClip]]) {
        guard contentKey != key else { return }
        // A changed/reopened project invalidates a held mouse gesture, including
        // reopen at the same revision. Late mouse-up never affects a new document.
        drag = nil
        contentKey = key
        self.tracks = tracks
        contentGeneration += 1
        rebuildPaths()
        needsDisplay = true
    }

    @objc private func viewportChanged(_ notification: Notification) { synchronizeViewport() }
    func synchronizeViewport() {
        guard let scroll = scrollView, let document = scroll.documentView else { return }
        let next = ArrangementOverviewGeometry(documentWidth: document.bounds.width,
            viewportX: scroll.contentView.bounds.minX - document.bounds.minX,
            viewportWidth: scroll.contentView.bounds.width)
        if next.documentWidth != geometry.documentWidth || next.viewportWidth != geometry.viewportWidth { drag = nil }
        if next != geometry { geometry = next; needsDisplay = true }
    }

    override func layout() {
        super.layout()
        if pathSize != bounds.size { drag = nil; rebuildPaths() }
        synchronizeViewport()
    }
    private func rebuildPaths() {
        pathSize = bounds.size
        paths.removeAll(keepingCapacity: true)
        guard let key = contentKey, !tracks.isEmpty, mapRect.width > 0, mapRect.height > 0 else { return }
        let rowHeight = mapRect.height / CGFloat(tracks.count)
        for (index, clips) in tracks.enumerated() {
            var byColor: [NSColor: NSBezierPath] = [:]
            for clip in clips {
                guard let interval = ArrangementOverviewGeometry.segment(start: clip.start, length: clip.length, total: key.frames) else { continue }
                let color = clip.color
                let path = byColor[color] ?? NSBezierPath()
                path.appendRect(NSRect(x: mapRect.minX + interval.lowerBound * mapRect.width,
                    y: mapRect.minY + CGFloat(index) * rowHeight,
                    width: max(0.5, (interval.upperBound - interval.lowerBound) * mapRect.width),
                    height: max(0.5, rowHeight - 0.75)))
                byColor[color] = path
            }
            paths += byColor.map { ($0.key, $0.value) }
        }
    }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.canvas.setFill(); bounds.fill()
        guard mapRect.width > 0, mapRect.height > 0 else { return }
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: mapRect).addClip()
        for (color, path) in paths where path.bounds.intersects(dirtyRect) {
            color.withAlphaComponent(color.alphaComponent * 0.65).setFill(); path.fill()
        }
        if let cycle, let total = contentKey?.frames,
           let span = ArrangementOverviewGeometry.segment(start: cycle.start, length: cycle.length, total: total) {
            DAWDesignTokens.Color.warning.withAlphaComponent(0.16).setFill()
            NSRect(x: mapRect.minX + span.lowerBound * mapRect.width, y: mapRect.minY,
                   width: (span.upperBound - span.lowerBound) * mapRect.width, height: mapRect.height).fill()
        }
        if let total = contentKey?.frames, total > 0 {
            let x = mapRect.minX + Double(min(playhead, total)) / Double(total) * mapRect.width
            DAWDesignTokens.Color.mint.setFill()
            NSRect(x: x, y: mapRect.minY, width: 1, height: mapRect.height).fill()
        }
        NSGraphicsContext.restoreGraphicsState()
        let viewport = viewportRect.insetBy(dx: 0.5, dy: 0.5)
        DAWDesignTokens.Color.accent.withAlphaComponent(0.09).setFill(); viewport.fill()
        (window?.firstResponder === self ? DAWDesignTokens.Color.mint : DAWDesignTokens.Color.accent).setStroke()
        let outline = NSBezierPath(roundedRect: viewport, xRadius: 2, yRadius: 2)
        outline.lineWidth = 1; outline.stroke()
        if tracks.allSatisfy({ $0.isEmpty }) {
            let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 10), .foregroundColor: DAWDesignTokens.Color.secondaryText]
            ("Обзор проекта" as NSString).draw(at: NSPoint(x: 12, y: bounds.midY - 6), withAttributes: attrs)
        }
    }

    @discardableResult
    func navigate(to start: Double) -> Bool {
        guard start.isFinite, let scroll = scrollView, let document = scroll.documentView else { return false }
        let clip = scroll.contentView
        var requested = clip.bounds
        requested.origin.x = document.bounds.minX + geometry.origin(forStart: start)
        clip.scroll(to: clip.constrainBoundsRect(requested).origin)
        scroll.reflectScrolledClipView(clip)
        synchronizeViewport()
        return true
    }
    private func fraction(for event: NSEvent) -> Double {
        let x = convert(event.locationInWindow, from: nil).x - mapRect.minX
        return ArrangementOverviewGeometry.fraction(at: x, width: mapRect.width)
    }
    override func mouseDown(with event: NSEvent) {
        guard mapRect.width > 0, bounds.contains(convert(event.locationInWindow, from: nil)), scrollView != nil else { return }
        window?.makeFirstResponder(self)
        synchronizeViewport()
        if event.clickCount >= 2 { drag = nil; onFitAll?(); synchronizeViewport(); return }
        let fraction = fraction(for: event)
        let original = geometry.start
        let inside = fraction >= original && fraction <= original + geometry.extent
        let offset = inside ? fraction - original : geometry.extent / 2
        drag = (original, offset)
        if !inside { navigate(to: geometry.centeredStart(at: fraction)) }
    }
    override func mouseDragged(with event: NSEvent) {
        guard let drag else { return }
        navigate(to: fraction(for: event) - drag.pointerOffset)
    }
    override func mouseUp(with event: NSEvent) { drag = nil }
    override func resignFirstResponder() -> Bool { drag = nil; needsDisplay = true; return super.resignFirstResponder() }
    override func becomeFirstResponder() -> Bool { needsDisplay = true; return super.becomeFirstResponder() }

    /// Called at DAWWindow's local-focus boundary before destructive/global keys.
    func handleFocusedKey(_ event: NSEvent) -> Bool {
        guard window?.firstResponder === self, event.type == .keyDown else { return false }
        let flags = event.modifierFlags.intersection([.command, .control, .option, .shift])
        if event.keyCode == 51 || event.keyCode == 117 { return true }
        guard flags.isEmpty else { return false }
        switch event.keyCode {
        case 53:
            if let drag { self.drag = nil; navigate(to: drag.start) }
            return true
        case 123: drag = nil; return navigate(to: geometry.start - geometry.extent / 10)
        case 124: drag = nil; return navigate(to: geometry.start + geometry.extent / 10)
        case 116: drag = nil; return navigate(to: geometry.start - geometry.extent * 0.9)
        case 121: drag = nil; return navigate(to: geometry.start + geometry.extent * 0.9)
        case 115: drag = nil; return navigate(to: 0)
        case 119: drag = nil; return navigate(to: geometry.maximumStart)
        default: return false
        }
    }
    override func keyDown(with event: NSEvent) {
        if !handleFocusedKey(event) { super.keyDown(with: event) }
    }
    override func accessibilityValue() -> Any? { NSNumber(value: geometry.start) }
    override func accessibilityMinValue() -> Any? { NSNumber(value: 0) }
    override func accessibilityMaxValue() -> Any? { NSNumber(value: geometry.maximumStart) }
    override func setAccessibilityValue(_ value: Any?) {
        guard let number = value as? NSNumber else { return }
        drag = nil; navigate(to: number.doubleValue)
    }
    override func accessibilityPerformIncrement() -> Bool { drag = nil; return navigate(to: geometry.start + geometry.extent / 10) }
    override func accessibilityPerformDecrement() -> Bool { drag = nil; return navigate(to: geometry.start - geometry.extent / 10) }
}

/// Small stable container; hiding/focusing the mixer never reparents editors.
@MainActor
final class ArrangementOverviewContainer: NSView {
    private let arrangement: NSView
    let overview: ArrangementOverviewView
    override var isFlipped: Bool { true }
    init(arrangement: NSView, overview: ArrangementOverviewView) {
        self.arrangement = arrangement; self.overview = overview
        super.init(frame: .zero)
        for view in [overview, arrangement] {
            view.translatesAutoresizingMaskIntoConstraints = true
            view.autoresizingMask = []
            addSubview(view)
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func layout() {
        super.layout()
        let height = min(38, bounds.height)
        overview.frame = NSRect(x: 0, y: 0, width: bounds.width, height: height)
        arrangement.frame = NSRect(x: 0, y: height, width: bounds.width, height: max(0, bounds.height - height))
    }
}
