import AppKit

@MainActor
final class PRProCanvas: NSView {
    private enum DragMode { case none, marquee, move, resizeStart, resizeEnd, draw, erase, velocity, hand }

    let state: PRProState
    weak var workspace: PRProWorkspaceView?
    var onUndo: (() -> Void)?
    var onRedo: (() -> Void)?
    var onPlayToggle: (() -> Void)?

    private var dragMode: DragMode = .none
    private var downPoint = NSPoint.zero
    private var handWindowPoint = NSPoint.zero
    private var handOrigin = NSPoint.zero
    private var base: [PRNoteEntity] = []
    private var anchorID: UInt64?
    private var initialSelection = Set<UInt64>()
    private var marqueeRect: NSRect?
    private var optionDuplicate = false
    private var duplicated = false
    private var drawID: UInt64 = 0
    private var drawStartBeat = 0.0
    private var drawPitch: UInt8 = 60
    private var tracking: NSTrackingArea?

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    init(state: PRProState) {
        self.state = state
        super.init(frame: .zero)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Нотное поле Piano Roll")
        setAccessibilityHelp("V выбор, B нота, E ластик, S разрез, Y velocity, H рука. Стрелки двигают ноты, Shift — крупный шаг, Option+вверх/вниз — velocity.")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private var rows: PRPitchRows { workspace?.rows ?? state.pitchRows }

    private func visibleEntities(in rect: NSRect) -> [PRNoteEntity] {
        guard let map = state.timeMap, state.pixelsPerBeat > 0 else { return [] }
        let leftBeat = max(0, Double(rect.minX - 4) / state.pixelsPerBeat)
        let rightBeat = min(map.durationBeats, Double(rect.maxX + 4) / state.pixelsPerBeat)
        let startFrame = map.frame(at: leftBeat)
        let endFrame = max(startFrame + 1, map.frame(at: rightBeat))
        let firstRow = max(0, min(rows.pitches.count - 1, Int(floor(rect.minY / state.rowHeight))))
        let lastRow = max(firstRow, min(rows.pitches.count - 1, Int(floor(rect.maxY / state.rowHeight))))
        let pitches = Array(rows.pitches[firstRow...lastRow])
        if state.preview != nil {
            return state.entities.filter { entity in
                guard pitches.contains(Int(entity.note.pitch)) else { return false }
                let end = entity.note.startFrames.addingReportingOverflow(entity.note.lengthFrames)
                return entity.note.startFrames < endFrame && (end.overflow || end.partialValue > startFrame)
            }
        }
        return state.index.query(start: startFrame, end: endFrame, pitches: pitches)
    }

    private func noteRect(_ entity: PRNoteEntity) -> NSRect? {
        PRProDrawing.noteRect(entity.note, state: state, rows: rows)
    }

    private func hit(_ point: NSPoint) -> PRNoteEntity? {
        let probe = NSRect(x: max(0, point.x - 4), y: point.y - 1, width: 8, height: 2)
        return visibleEntities(in: probe).filter { entity in
            noteRect(entity)?.insetBy(dx: -2, dy: 0).contains(point) == true
        }.sorted { lhs, rhs in
            let l = state.selection.contains(lhs.id)
            let r = state.selection.contains(rhs.id)
            return l == r ? lhs.id > rhs.id : l
        }.first
    }

    private func snappedBeat(at point: NSPoint, event: NSEvent) -> Double {
        guard let map = state.timeMap else { return 0 }
        let raw = Double(point.x) / state.pixelsPerBeat
        let snapped = state.grid.snap(raw, origin: map.originBeat,
                                      pointsPerBeat: state.pixelsPerBeat,
                                      bypass: event.modifierFlags.contains(.control))
        return min(map.durationBeats, max(0, snapped))
    }

    private func pitch(at point: NSPoint) -> Int {
        rows.pitch(at: Int(floor(point.y / state.rowHeight)))
    }

    override func draw(_ dirtyRect: NSRect) {
        PRProDrawing.canvas.setFill(); dirtyRect.fill()
        guard state.timeMap != nil else {
            PRProDrawing.label("Выберите MIDI-клип", in: NSRect(x: 28, y: 36, width: 360, height: 24), size: 14)
            return
        }
        let firstRow = max(0, Int(floor(dirtyRect.minY / state.rowHeight)))
        let lastRow = min(rows.pitches.count - 1, Int(floor(dirtyRect.maxY / state.rowHeight)))
        if lastRow >= firstRow {
            for row in firstRow...lastRow {
                let pitch = rows.pitches[row]
                let y = Double(row) * state.rowHeight
                let rowRect = NSRect(x: dirtyRect.minX, y: y,
                                     width: dirtyRect.width, height: state.rowHeight)
                if PRPitch.isBlack(pitch) {
                    NSColor.black.withAlphaComponent(0.16).setFill(); rowRect.fill()
                }
                if state.scale.kind != .chromatic, state.scale.contains(pitch) {
                    PRProDrawing.mint.withAlphaComponent(0.035).setFill(); rowRect.fill()
                }
                let octaveBoundary = pitch % 12 == 11
                PRProDrawing.line(NSPoint(x: dirtyRect.minX, y: y),
                                  NSPoint(x: dirtyRect.maxX, y: y),
                                  color: PRProDrawing.border.withAlphaComponent(octaveBoundary ? 0.5 : 0.18))
            }
        }
        PRProDrawing.drawGrid(in: dirtyRect, height: bounds.height, state: state)
        let visible = visibleEntities(in: dirtyRect.insetBy(dx: -4, dy: 0))
        for selected in [false, true] {
            for entity in visible where state.selection.contains(entity.id) == selected {
                guard let rect = noteRect(entity), rect.intersects(dirtyRect) else { continue }
                let color = PRProDrawing.noteColor(entity.note)
                color.setFill()
                let path = NSBezierPath(roundedRect: rect, xRadius: 3, yRadius: 3)
                path.fill()
                if selected {
                    PRProDrawing.text.setStroke(); path.lineWidth = 1.5; path.stroke()
                }
                if rect.width >= 28, rect.height >= 13 {
                    PRProDrawing.label(PRPitch.name(entity.note.pitch),
                                       in: rect.insetBy(dx: 4, dy: 1), color: .white, size: 9)
                }
                if selected, rect.width >= 18 {
                    PRProDrawing.line(NSPoint(x: rect.minX + 4, y: rect.minY + 4),
                                      NSPoint(x: rect.minX + 4, y: rect.maxY - 4),
                                      color: .white.withAlphaComponent(0.55))
                    PRProDrawing.line(NSPoint(x: rect.maxX - 4, y: rect.minY + 4),
                                      NSPoint(x: rect.maxX - 4, y: rect.maxY - 4),
                                      color: .white.withAlphaComponent(0.72))
                }
            }
        }
        if let marqueeRect {
            PRProDrawing.accent.withAlphaComponent(0.14).setFill(); marqueeRect.fill()
            PRProDrawing.accent.setStroke(); NSBezierPath(rect: marqueeRect).stroke()
        }
        PRProDrawing.drawPlayhead(state: state, offsetX: 0, height: bounds.height)
        if window?.firstResponder === self {
            PRProDrawing.accent.withAlphaComponent(0.55).setStroke()
            NSBezierPath(rect: visibleRect.insetBy(dx: 1, dy: 1)).stroke()
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let point = convert(event.locationInWindow, from: nil)
        downPoint = point
        initialSelection = state.selection
        base = state.entities
        marqueeRect = nil
        duplicated = false
        optionDuplicate = event.modifierFlags.contains(.option)

        if state.tool == .hand || event.buttonNumber == 2 {
            dragMode = .hand
            handWindowPoint = event.locationInWindow
            handOrigin = workspace?.scrollOrigin ?? .zero
            return
        }

        guard state.editable, let map = state.timeMap else {
            state.fail(PREditError.unavailable)
            return
        }
        let item = hit(point)
        if event.modifierFlags.contains(.command), let item {
            if state.selection.contains(item.id) { state.selection.remove(item.id) }
            else { state.selection.insert(item.id) }
            state.changed()
            dragMode = .none
            return
        }
        if let item {
            if event.modifierFlags.contains(.shift) { state.selection.insert(item.id) }
            else if !state.selection.contains(item.id) { state.selection = [item.id] }
        }
        state.insertionBeat = snappedBeat(at: point, event: event)
        anchorID = item?.id

        if state.tool == .split, let item {
            let ids = state.selection.contains(item.id) ? state.selection : Set([item.id])
            let frame = map.frame(at: state.insertionBeat)
            let next = state.nextID
            state.perform { entities, timeMap in
                try PREdits.split(entities, selected: ids, at: frame, nextID: next, map: timeMap)
            }
            dragMode = .none
            return
        }

        if state.tool == .erase, let item {
            guard state.beginGesture() else { return }
            dragMode = .erase
            base.removeAll { $0.id == item.id }
            state.previewGesture(base)
            return
        }

        if (state.tool == .draw && item == nil) || (event.clickCount == 2 && item == nil) {
            guard state.beginGesture() else { return }
            dragMode = .draw
            drawID = state.nextID
            drawStartBeat = state.insertionBeat
            let rawPitch = pitch(at: point)
            drawPitch = state.lockScale ? state.scale.nearest(rawPitch) : UInt8(rawPitch)
            do {
                let startFrame = map.frame(at: drawStartBeat)
                let maxFrame = startFrame + min(PRLimits.noteFrames, map.clipLength - startFrame)
                let maxBeat = map.beat(at: maxFrame)
                let endBeat = min(maxBeat, drawStartBeat + state.defaultLengthBeats)
                let note = try map.note(start: drawStartBeat, end: endBeat,
                                        pitch: drawPitch, channel: state.defaultChannel,
                                        velocity: state.defaultVelocity)
                state.selection = [drawID]
                state.previewGesture(base + [PRNoteEntity(id: drawID, note: note)])
            } catch {
                state.cancelGesture(); state.fail(error); dragMode = .none
            }
            return
        }

        if let item, let rect = noteRect(item) {
            guard state.beginGesture() else { return }
            if state.tool == .velocity { dragMode = .velocity }
            else if rect.width > 18, point.x <= rect.minX + 5 { dragMode = .resizeStart }
            else if rect.width > 18, point.x >= rect.maxX - 5 { dragMode = .resizeEnd }
            else { dragMode = .move }
            state.changed()
        } else {
            dragMode = .marquee
            if !event.modifierFlags.contains(.shift) {
                state.selection.removeAll(); initialSelection.removeAll()
            }
            state.changed()
        }
    }

    override func mouseDragged(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        if dragMode == .hand {
            let dx = event.locationInWindow.x - handWindowPoint.x
            let dy = event.locationInWindow.y - handWindowPoint.y
            workspace?.scroll(to: NSPoint(x: handOrigin.x - dx, y: handOrigin.y + dy))
            return
        }
        if dragMode == .marquee {
            _ = autoscroll(with: event)
            let rect = NSRect(x: min(downPoint.x, point.x), y: min(downPoint.y, point.y),
                              width: abs(point.x - downPoint.x), height: abs(point.y - downPoint.y))
            marqueeRect = rect
            let ids = visibleEntities(in: rect).filter { entity in
                noteRect(entity)?.intersects(rect) == true
            }.map(\.id)
            state.selection = initialSelection.union(ids)
            state.changed()
            needsDisplay = true
            return
        }
        guard state.isGesturing, let map = state.timeMap else { return }
        _ = autoscroll(with: event)
        do {
            switch dragMode {
            case .move:
                guard var activeAnchor = base.first(where: { $0.id == anchorID }) else { return }
                if optionDuplicate, !duplicated,
                   hypot(point.x - downPoint.x, point.y - downPoint.y) >= 3 {
                    let selected = base.filter { state.selection.contains($0.id) }
                    guard selected.count <= PRLimits.noteCount - base.count else { throw PREditError.tooManyNotes }
                    var id = state.nextID
                    var mapping: [UInt64: UInt64] = [:]
                    var copies: [PRNoteEntity] = []
                    for item in selected {
                        let currentID = id
                        let overflow = id.addingReportingOverflow(1)
                        if overflow.overflow { throw PREditError.invalidNote }
                        id = overflow.partialValue
                        mapping[item.id] = currentID
                        copies.append(PRNoteEntity(id: currentID, note: item.note))
                    }
                    base.append(contentsOf: copies)
                    state.selection = Set(copies.map(\.id))
                    if let anchorID, let replacement = mapping[anchorID] {
                        self.anchorID = replacement
                        activeAnchor = copies.first(where: { $0.id == replacement }) ?? activeAnchor
                    }
                    duplicated = true
                } else if optionDuplicate, !duplicated { return }
                let rawTarget = map.start(activeAnchor.note) + Double(point.x - downPoint.x) / state.pixelsPerBeat
                let target = state.grid.snap(rawTarget, origin: map.originBeat,
                                             pointsPerBeat: state.pixelsPerBeat,
                                             bypass: event.modifierFlags.contains(.control))
                let fromPitch = pitch(at: downPoint)
                var toPitch = pitch(at: point)
                if state.lockScale { toPitch = Int(state.scale.nearest(toPitch)) }
                let deltaBeat = abs(point.x - downPoint.x) < 2 ? 0 : target - map.start(activeAnchor.note)
                let candidate = try PREdits.move(base, selected: state.selection,
                                                 beats: deltaBeat,
                                                 semitones: toPitch - fromPitch,
                                                 map: map)
                state.previewGesture(candidate)

            case .resizeStart, .resizeEnd:
                guard let anchor = base.first(where: { $0.id == anchorID }) else { return }
                let edgeBeat = dragMode == .resizeStart ? map.start(anchor.note) : map.end(anchor.note)
                let rawTarget = edgeBeat + Double(point.x - downPoint.x) / state.pixelsPerBeat
                let target = state.grid.snap(rawTarget, origin: map.originBeat,
                                             pointsPerBeat: state.pixelsPerBeat,
                                             bypass: event.modifierFlags.contains(.control))
                state.previewGesture(try PREdits.resize(base, selected: state.selection,
                                                        delta: target - edgeBeat,
                                                        edge: dragMode == .resizeStart ? .start : .end,
                                                        map: map))

            case .velocity:
                let sensitivity = event.modifierFlags.contains(.shift) ? 0.2 : 1.0
                let delta = Int(Double(downPoint.y - point.y) * sensitivity)
                state.previewGesture(PREdits.velocity(base, selected: state.selection, delta: delta))

            case .erase:
                if let item = hit(point), base.contains(where: { $0.id == item.id }) {
                    base.removeAll { $0.id == item.id }
                    state.previewGesture(base)
                }

            case .draw:
                let startFrame = map.frame(at: drawStartBeat)
                let maxFrame = startFrame + min(PRLimits.noteFrames, map.clipLength - startFrame)
                let maxBeat = map.beat(at: maxFrame)
                let minEndFrame = min(map.clipLength, startFrame + 1)
                let minEndBeat = map.beat(at: minEndFrame)
                let endBeat = min(maxBeat, max(minEndBeat, snappedBeat(at: point, event: event)))
                let note = try map.note(start: drawStartBeat, end: endBeat,
                                        pitch: drawPitch, channel: state.defaultChannel,
                                        velocity: state.defaultVelocity)
                state.previewGesture(base + [PRNoteEntity(id: drawID, note: note)])
                state.defaultLengthBeats = max(1.0 / 64, endBeat - drawStartBeat)

            default: break
            }
        } catch { state.fail(error) }
    }

    override func mouseUp(with event: NSEvent) {
        if state.isGesturing { state.finishGesture() }
        dragMode = .none
        base = []
        anchorID = nil
        marqueeRect = nil
        optionDuplicate = false
        duplicated = false
        state.changed()
        needsDisplay = true
    }

    override func otherMouseDown(with event: NSEvent) { mouseDown(with: event) }
    override func otherMouseDragged(with event: NSEvent) { mouseDragged(with: event) }
    override func otherMouseUp(with event: NSEvent) { mouseUp(with: event) }

    override func magnify(with event: NSEvent) {
        workspace?.zoomHorizontal(factor: max(0.2, 1 + Double(event.magnification)), anchorInCanvas: convert(event.locationInWindow, from: nil))
    }

    override func scrollWheel(with event: NSEvent) {
        if event.modifierFlags.contains(.command) {
            workspace?.zoomHorizontal(factor: exp(Double(event.scrollingDeltaY) * 0.015), anchorInCanvas: convert(event.locationInWindow, from: nil))
        } else if event.modifierFlags.contains(.option) {
            workspace?.zoomVertical(factor: exp(Double(event.scrollingDeltaY) * 0.012), anchorInCanvas: convert(event.locationInWindow, from: nil))
        } else {
            super.scrollWheel(with: event)
        }
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: .zero,
                                  options: [.activeInKeyWindow, .inVisibleRect, .mouseMoved],
                                  owner: self, userInfo: nil)
        addTrackingArea(area)
        tracking = area
    }

    override func mouseMoved(with event: NSEvent) {
        guard !state.isGesturing else { return }
        let point = convert(event.locationInWindow, from: nil)
        if state.tool == .hand { NSCursor.openHand.set(); return }
        if let item = hit(point), let rect = noteRect(item), rect.width > 18,
           min(abs(point.x - rect.minX), abs(point.x - rect.maxX)) <= 5 {
            NSCursor.resizeLeftRight.set()
        } else if state.tool == .draw || state.tool == .split {
            NSCursor.crosshair.set()
        } else {
            NSCursor.arrow.set()
        }
    }

    override func keyDown(with event: NSEvent) {
        if handleKey(event) { return }
        super.keyDown(with: event)
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if window?.firstResponder === self, handleKey(event) { return true }
        return super.performKeyEquivalent(with: event)
    }

    private func physicalKey(_ event: NSEvent) -> String {
        let map: [UInt16: String] = [0: "a", 1: "s", 2: "d", 3: "f", 4: "h", 5: "g",
                                     7: "x", 8: "c", 9: "v", 11: "b", 12: "q", 14: "e", 16: "y"]
        return map[event.keyCode] ?? event.charactersIgnoringModifiers?.lowercased() ?? ""
    }

    func handleKey(_ event: NSEvent) -> Bool {
        let key = physicalKey(event)
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let command = flags.contains(.command)
        let shift = flags.contains(.shift)

        if event.keyCode == 53 {
            if state.isGesturing { state.cancelGesture() }
            dragMode = .none; marqueeRect = nil; needsDisplay = true
            return true
        }
        if state.isGesturing { return true }

        if command {
            guard flags.subtracting([.command, .shift]).isEmpty else { return false }
            switch key {
            case "a": state.selectAll()
            case "c": state.copyToPasteboard()
            case "x": state.cutToPasteboard()
            case "v": state.pasteFromPasteboard()
            case "d": state.duplicateSelection()
            case "z": shift ? onRedo?() : onUndo?()
            case "=", "+": workspace?.zoomHorizontal(factor: 1.25, anchorInCanvas: nil)
            case "-": workspace?.zoomHorizontal(factor: 0.8, anchorInCanvas: nil)
            case "0": workspace?.fit(selectionOnly: shift)
            default: return false
            }
            return true
        }

        if flags.contains(.control) { return false }
        if event.keyCode == 49 { onPlayToggle?(); return true }
        if event.keyCode == 51 || event.keyCode == 117 { state.deleteSelected(); return true }

        let step = state.grid.step(state.pixelsPerBeat)
        switch event.keyCode {
        case 123: state.nudge(beats: -(shift ? step * 4 : step)); return true
        case 124: state.nudge(beats: shift ? step * 4 : step); return true
        case 125:
            if flags.contains(.option) { state.adjustVelocity(shift ? -10 : -1) }
            else { state.nudge(semitones: shift ? -12 : -1) }
            return true
        case 126:
            if flags.contains(.option) { state.adjustVelocity(shift ? 10 : 1) }
            else { state.nudge(semitones: shift ? 12 : 1) }
            return true
        default: break
        }

        switch key {
        case "q": state.quantize(); return true
        case "f": state.fold.toggle(); state.changed(); workspace?.refreshGeometry(); return true
        case "g": state.grid.enabled.toggle(); state.changed(); return true
        default:
            if let tool = PRTool.allCases.first(where: { $0.shortcut.lowercased() == key }) {
                state.tool = tool; state.changed(); return true
            }
        }
        return false
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        window?.makeFirstResponder(self)
        if let item = hit(convert(event.locationInWindow, from: nil)), !state.selection.contains(item.id) {
            state.selection = [item.id]; state.changed()
        }
        let menu = NSMenu()
        let commands: [(String, Selector)] = [
            ("Копировать", #selector(copyNotes)), ("Вырезать", #selector(cutNotes)),
            ("Вставить", #selector(pasteNotes)), ("Дублировать", #selector(duplicateNotes)),
            ("Квантовать", #selector(quantizeNotes)), ("Legato", #selector(legatoNotes)),
            ("Humanize", #selector(humanizeNotes)), ("Удалить", #selector(deleteNotes))]
        for (title, action) in commands {
            let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
            item.target = self; menu.addItem(item)
        }
        return menu
    }

    @objc private func copyNotes() { state.copyToPasteboard() }
    @objc private func cutNotes() { state.cutToPasteboard() }
    @objc private func pasteNotes() { state.pasteFromPasteboard() }
    @objc private func duplicateNotes() { state.duplicateSelection() }
    @objc private func quantizeNotes() { state.quantize() }
    @objc private func legatoNotes() { state.legato() }
    @objc private func humanizeNotes() { state.humanize() }
    @objc private func deleteNotes() { state.deleteSelected() }
}
