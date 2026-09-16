import AppKit

@MainActor
final class PRProRulerView: NSView {
    let state: PRProState
    weak var workspace: PRProWorkspaceView?
    override var isFlipped: Bool { true }

    init(state: PRProState) {
        self.state = state
        super.init(frame: .zero)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Линейка Piano Roll")
        setAccessibilityHelp("Показывает реальные такты и размеры проекта. Клик перемещает позицию воспроизведения внутри MIDI-клипа.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func draw(_ dirtyRect: NSRect) {
        PRProDrawing.surface.setFill(); bounds.fill()
        guard let map = state.timeMap else { return }
        let offset = workspace?.scrollOrigin.x ?? 0
        PRProDrawing.drawGrid(in: bounds, height: bounds.height, state: state, offsetX: offset)
        PRProDrawing.line(NSPoint(x: 0, y: bounds.maxY - 1),
                          NSPoint(x: bounds.maxX, y: bounds.maxY - 1),
                          color: PRProDrawing.border)
        if !drawProjectBars(map: map, offset: offset) {
            drawBeatFallback(map: map, offset: offset)
        }
        PRProDrawing.drawPlayhead(state: state, offsetX: offset, height: bounds.height)
    }

    private func drawProjectBars(map: PRTimeMap, offset: CGFloat) -> Bool {
        guard let app = NSApp.delegate as? DraftApp, !app.tempoBars.isEmpty else { return false }
        let clipFirstBeat = map.originBeat
        let clipLastBeat = clipFirstBeat + map.durationBeats
        let visibleLeft = max(0, Double(offset) / state.pixelsPerBeat)
        let visibleRight = min(map.durationBeats, Double(offset + bounds.width) / state.pixelsPerBeat)
        var drew = false
        var lastSignature: String?
        var lastLabelX: CGFloat = -100
        for mark in app.tempoBars where mark.beats >= clipFirstBeat - 1e-9 && mark.beats <= clipLastBeat + 1e-9 {
            let relativeBeat = mark.beats - clipFirstBeat
            guard relativeBeat >= visibleLeft - 1, relativeBeat <= visibleRight + 1 else { continue }
            let x = CGFloat(relativeBeat * state.pixelsPerBeat) - offset
            guard x >= -2, x <= bounds.width + 2 else { continue }
            drew = true
            PRProDrawing.line(NSPoint(x: x, y: 0), NSPoint(x: x, y: bounds.height),
                              color: PRProDrawing.text.withAlphaComponent(0.32), width: 1.15)
            if x - lastLabelX >= 24 {
                PRProDrawing.label(String(mark.number),
                                   in: NSRect(x: x + 4, y: 4, width: 44, height: 16),
                                   color: PRProDrawing.text, size: 10, weight: .semibold)
                lastLabelX = x
            }
            let signature = app.tempoMap.signature(atFrame: mark.frame)
            let signatureText = "\(signature.numerator)/\(signature.denominator)"
            if signatureText != lastSignature {
                PRProDrawing.label(signatureText,
                                   in: NSRect(x: x + 25, y: 5, width: 44, height: 15),
                                   color: PRProDrawing.mint, size: 8)
                lastSignature = signatureText
            }
        }
        return drew
    }

    private func drawBeatFallback(map: PRTimeMap, offset: CGFloat) {
        let left = max(0, Double(offset) / state.pixelsPerBeat)
        let right = min(map.durationBeats, Double(offset + bounds.width) / state.pixelsPerBeat)
        let labelStep = max(1.0, ceil(48 / max(1, state.pixelsPerBeat)))
        var beat = floor((left + map.originBeat) / labelStep) * labelStep
        var guardCount = 0
        while beat <= right + map.originBeat + labelStep, guardCount < 5000 {
            let relative = beat - map.originBeat
            let x = CGFloat(relative * state.pixelsPerBeat) - offset
            if x >= -30, x <= bounds.width {
                PRProDrawing.label(String(format: "%.2f", beat + 1),
                                   in: NSRect(x: x + 4, y: 5, width: 70, height: 16),
                                   color: PRProDrawing.secondary, size: 9)
            }
            beat += labelStep
            guardCount += 1
        }
    }

    override func mouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        workspace?.seek(relativeBeat: Double(point.x + (workspace?.scrollOrigin.x ?? 0)) / state.pixelsPerBeat)
        workspace?.focusCanvas()
    }
}

@MainActor
final class PRProKeyboardView: NSView {
    let state: PRProState
    weak var workspace: PRProWorkspaceView?
    override var isFlipped: Bool { true }

    init(state: PRProState) {
        self.state = state
        super.init(frame: .zero)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Клавиатура Piano Roll")
        setAccessibilityHelp("Клик выбирает ноты этой высоты. Shift добавляет высоту к текущему выделению.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func draw(_ dirtyRect: NSRect) {
        PRProDrawing.raised.setFill(); bounds.fill()
        guard let workspace else { return }
        let rows = workspace.rows
        let offset = workspace.scrollOrigin.y
        let selectedPitches = Set(state.selectedEntities.map { Int($0.note.pitch) })
        for (row, pitch) in rows.pitches.enumerated() {
            let y = CGFloat(Double(row) * state.rowHeight) - offset
            guard y + state.rowHeight > 0, y < bounds.height else { continue }
            let rectangle = NSRect(x: 0, y: y, width: bounds.width - 1, height: state.rowHeight - 1)
            let black = PRPitch.isBlack(pitch)
            (black ? NSColor(white: 0.11, alpha: 1) : NSColor(white: 0.72, alpha: 1)).setFill()
            rectangle.fill()
            if selectedPitches.contains(pitch) {
                PRProDrawing.accent.withAlphaComponent(0.56).setFill(); rectangle.fill()
            }
            if state.scale.kind != .chromatic, pitch % 12 == state.scale.root {
                PRProDrawing.mint.setFill()
                NSRect(x: 0, y: y, width: 3, height: state.rowHeight - 1).fill()
            }
            if state.rowHeight >= 12 {
                PRProDrawing.label(PRPitch.name(UInt8(pitch)),
                                   in: rectangle.insetBy(dx: 5, dy: 2),
                                   color: black ? PRProDrawing.text : PRProDrawing.canvas,
                                   size: 9, alignment: .right)
            }
        }
    }

    override func mouseDown(with event: NSEvent) {
        guard let workspace else { return }
        let point = convert(event.locationInWindow, from: nil)
        let row = Int(floor((point.y + workspace.scrollOrigin.y) / state.rowHeight))
        let pitch = workspace.rows.pitch(at: row)
        let ids = Set(state.entities.filter { Int($0.note.pitch) == pitch }.map(\.id))
        if event.modifierFlags.contains(.shift) {
            state.selection.formUnion(ids)
        } else {
            state.selection = ids
        }
        state.setStatus("\(PRPitch.name(UInt8(pitch))) · нот этой высоты: \(ids.count)")
        workspace.focusCanvas()
    }

    override func scrollWheel(with event: NSEvent) {
        guard let workspace else { return }
        workspace.scroll(to: NSPoint(x: workspace.scrollOrigin.x,
                                     y: workspace.scrollOrigin.y - event.scrollingDeltaY))
    }
}

@MainActor
final class PRProVelocityLane: NSView {
    let state: PRProState
    weak var workspace: PRProWorkspaceView?
    private var base: [PRNoteEntity] = []
    private var anchorVelocity = 100
    private var editing = false
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    init(state: PRProState) {
        self.state = state
        super.init(frame: .zero)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Velocity lane")
        setAccessibilityHelp("Перетаскивание столбика изменяет velocity выбранных нот с сохранением относительной разницы.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private var baseline: CGFloat { max(24, bounds.height - 8) }
    private var usableHeight: CGFloat { max(16, baseline - 24) }

    private func x(for note: PianoRollNote) -> CGFloat {
        CGFloat((state.timeMap?.start(note) ?? 0) * state.pixelsPerBeat) - (workspace?.scrollOrigin.x ?? 0)
    }

    private func y(for velocity: UInt8) -> CGFloat {
        baseline - CGFloat(velocity) / 127 * usableHeight
    }

    private func visibleNotes() -> [PRNoteEntity] {
        guard let map = state.timeMap, let workspace else { return [] }
        let left = max(0, Double(workspace.scrollOrigin.x - 8) / state.pixelsPerBeat)
        let right = min(map.durationBeats, Double(workspace.scrollOrigin.x + bounds.width + 8) / state.pixelsPerBeat)
        let start = map.frame(at: left)
        let end = max(start + 1, map.frame(at: right))
        let source = state.preview != nil ? state.entities : state.index.query(start: start, end: end, pitches: Array(0...127))
        return source.filter { entity in
            let x = self.x(for: entity.note)
            return x >= -8 && x <= bounds.width + 8
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        PRProDrawing.canvas.setFill(); bounds.fill()
        PRProDrawing.drawGrid(in: bounds, height: bounds.height, state: state,
                              offsetX: workspace?.scrollOrigin.x ?? 0)
        for level in [UInt8(32), 64, 96, 127] {
            PRProDrawing.line(NSPoint(x: 0, y: y(for: level)),
                              NSPoint(x: bounds.width, y: y(for: level)),
                              color: PRProDrawing.border.withAlphaComponent(0.22))
        }
        for selected in [false, true] {
            for entity in visibleNotes() where state.selection.contains(entity.id) == selected {
                let point = NSPoint(x: x(for: entity.note), y: y(for: entity.note.velocity))
                let color = selected ? PRProDrawing.text : PRProDrawing.noteColor(entity.note).withAlphaComponent(0.78)
                PRProDrawing.line(point, NSPoint(x: point.x, y: baseline), color: color, width: selected ? 3 : 2)
                color.setFill()
                NSBezierPath(ovalIn: NSRect(x: point.x - 3, y: point.y - 3, width: 6, height: 6)).fill()
            }
        }
        PRProDrawing.label("VELOCITY", in: NSRect(x: 8, y: 4, width: 100, height: 16),
                           color: PRProDrawing.secondary, size: 9)
        PRProDrawing.drawPlayhead(state: state, offsetX: workspace?.scrollOrigin.x ?? 0,
                                  height: bounds.height)
    }

    private func nearest(to point: NSPoint) -> PRNoteEntity? {
        visibleNotes().filter { abs(x(for: $0.note) - point.x) <= 8 }.sorted { lhs, rhs in
            let ld = abs(y(for: lhs.note.velocity) - point.y)
            let rd = abs(y(for: rhs.note.velocity) - point.y)
            if ld != rd { return ld < rd }
            let ls = state.selection.contains(lhs.id), rs = state.selection.contains(rhs.id)
            return ls == rs ? lhs.id > rhs.id : ls
        }.first
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        guard state.editable else { state.fail(PREditError.unavailable); return }
        let point = convert(event.locationInWindow, from: nil)
        guard let item = nearest(to: point) else { return }
        if event.modifierFlags.contains(.shift) { state.selection.insert(item.id) }
        else if !state.selection.contains(item.id) { state.selection = [item.id] }
        base = state.entities
        anchorVelocity = Int(item.note.velocity)
        editing = state.beginGesture()
        update(point)
    }

    private func update(_ point: NSPoint) {
        guard editing else { return }
        let target = Int(((baseline - point.y) / usableHeight * 127).rounded())
        let delta = min(127, max(1, target)) - anchorVelocity
        state.previewGesture(PREdits.velocity(base, selected: state.selection, delta: delta))
    }

    override func mouseDragged(with event: NSEvent) {
        update(convert(event.locationInWindow, from: nil))
    }

    override func mouseUp(with event: NSEvent) {
        if editing { state.finishGesture() }
        editing = false; base = []
    }

    override func keyDown(with event: NSEvent) {
        if workspace?.canvas.handleKey(event) == true { return }
        super.keyDown(with: event)
    }
}

@MainActor
final class PRProLaneGrip: NSView {
    var onDelta: ((CGFloat) -> Void)?
    private var lastY: CGFloat = 0
    override var isFlipped: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        PRProDrawing.surface.setFill(); bounds.fill()
        PRProDrawing.line(NSPoint(x: bounds.midX - 22, y: bounds.midY),
                          NSPoint(x: bounds.midX + 22, y: bounds.midY),
                          color: PRProDrawing.border, width: 2)
    }
    override func resetCursorRects() { addCursorRect(bounds, cursor: .resizeUpDown) }
    override func mouseDown(with event: NSEvent) { lastY = event.locationInWindow.y }
    override func mouseDragged(with event: NSEvent) {
        let current = event.locationInWindow.y
        onDelta?(current - lastY)
        lastY = current
    }
}
