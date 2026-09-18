import AppKit

/// Window-owned event adapter. Only previews live here; every durable edit goes
/// through an existing revision-checked C ABI command, once at mouse-up.
@MainActor
final class ArrangementEditingController: ArrangementWindowEditing {
    struct Lane {
        let track: UInt64
        let view: NSView
        let kind: ArrangementClipKey.Kind?
    }
    struct Item {
        let key: ArrangementClipKey
        let bounds: ArrangementClipBounds
    }
    enum Action: Equatable { case move, trimLeft, trimRight, fadeLeft, fadeRight, marquee, pan, zoom, draw }
    struct Gesture {
        let action: Action
        let origin: NSPoint
        let viewport: NSPoint
        let originFrame: UInt64
        let originDocument: NSPoint
        let lane: Lane
        let primary: Item?
        let items: [Item]
        let revision: UInt64
        let document: UUID
        let copy: Bool
        let additive: Bool
        var dragged = false
        var target: UInt64
        var proposed: [ArrangementClipBounds] = []
    }
    struct Clipboard {
        let key: ArrangementClipKey
        let revision: UInt64
        let document: UUID
        let cut: Bool
    }
    weak var app: DraftApp?
    weak var toolbar: ArrangementToolBar?
    let overlay = ArrangementGestureOverlay(frame: .zero)
    private(set) var tool: ArrangementTool = .pointer
    var lanes: [Lane] = []
    var items: [Item] = []
    var selection = Set<ArrangementClipKey>()
    var focusTrack: UInt64?
    var gesture: Gesture?
    var clipboard: Clipboard?
    var projectionRevision: UInt64?
    var documentID: UUID?
    var restoringSelection: [(UInt64, ArrangementClipKey.Kind, ArrangementClipBounds)] = []
    var restoreKeyboardFocus = false
    private var cursors: [ArrangementTool: NSCursor] = [:]
    static let limit = BeatFrameMap.timelineLimitFrame

    func attach(to app: DraftApp) {
        self.app = app
        app.window.acceptsMouseMovedEvents = true
        func findBar(_ view: NSView) -> WorkspaceCommandBar? {
            if let bar = view as? WorkspaceCommandBar { return bar }
            return view.subviews.lazy.compactMap(findBar).first
        }
        toolbar = app.window.contentView.flatMap(findBar)?.tools
        toolbar?.onSelect = { [weak self] in self?.selectTool($0) }
        toolbar?.display(tool)
        bindProjection()
    }

    func bindProjectionIfNeeded() {
        guard let app else { return }
        if documentID != app.midiDocumentID || projectionRevision != app.revision ||
            lanes.count != app.rows.arrangedSubviews.count || lanes.contains(where: { $0.view.window !== app.window }) {
            bindProjection()
        }
    }

    func bindProjection() {
        guard let app, app.session != nil else { return }
        // Stamp the projection with the revision it was actually rendered from,
        // never with a newer session revision while the lane views are stale.
        let revision = app.revision
        guard currentRevision() == revision else {
            cancelGesture(); selection.removeAll(); clipboard = nil; projectionRevision = nil
            items.removeAll(); syncSelection(); return
        }
        if let g = gesture, g.lane.view.window !== app.window { cancelGesture() }
        if documentID != app.midiDocumentID || projectionRevision != revision {
            cancelGesture()
            selection.removeAll()
            // An index-based clipboard must never silently name a different clip.
            if clipboard?.revision != revision || clipboard?.document != app.midiDocumentID { clipboard = nil }
        }
        if documentID != app.midiDocumentID { focusTrack = nil }
        documentID = app.midiDocumentID; projectionRevision = revision
        lanes = []; items = []
        for (row, track) in app.trackIDs.sorted(by: { $0.key < $1.key }) {
            guard row < app.rows.arrangedSubviews.count else { continue }
            let group = app.rows.arrangedSubviews[row]
            let view = (group as? NSStackView)?.arrangedSubviews.first ?? group
            if let wave = view as? WaveformView {
                lanes.append(Lane(track: track, view: wave, kind: .audio))
                for (index, clip) in wave.clips.enumerated() {
                    items.append(Item(key: .init(track: track, index: index, kind: .audio), bounds:
                        .init(start: clip.start, length: clip.length, offset: clip.sourceOffset,
                              sourceLength: clip.sourceFramesForTake > 0 ? clip.sourceFramesForTake : wave.sourceFrames,
                              fadeIn: clip.fadeIn, fadeOut: clip.fadeOut, looped: clip.looped)))
                }
            } else if let midi = view as? MidiArrangementView {
                lanes.append(Lane(track: track, view: midi, kind: .midi))
                items += midi.clips.map { Item(key: .init(track: track, index: $0.index, kind: .midi),
                    bounds: .init(start: $0.start, length: $0.length)) }
            } else if view is EmptyTimelineLaneView {
                lanes.append(Lane(track: track, view: view, kind: nil))
            }
        }
        selection = selection.intersection(Set(items.map(\.key)))
        for (track, kind, bounds) in restoringSelection {
            if let item = items.first(where: { !selection.contains($0.key) && $0.key.track == track &&
                $0.key.kind == kind && $0.bounds.start == bounds.start && $0.bounds.length == bounds.length }) {
                selection.insert(item.key)
            }
        }
        restoringSelection.removeAll()
        if let document = app.timelineDocument, overlay.superview !== document {
            overlay.removeFromSuperview(); document.addSubview(overlay, positioned: .above, relativeTo: nil)
            overlay.autoresizingMask = [.width, .height]
        }
        overlay.frame = app.timelineDocument?.bounds ?? .zero
        syncSelection()
        if restoreKeyboardFocus {
            restoreKeyboardFocus = false
            if let lane = lanes.first(where: { $0.track == focusTrack }) { focus(lane) }
        }
    }

    func currentRevision() -> UInt64? {
        guard let session = app?.session else { return nil }
        var snapshot = daw_snapshot(); snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        return daw_get_snapshot(session, &snapshot) == 0 ? snapshot.revision : nil
    }
    var editable: Bool {
        guard let app else { return false }
        return !app.isRecording && !app.midiTakeArmed && app.automationGesture == nil && app.pluginParameterGesture == nil
    }
    func message(_ text: String) { app?.setProjectMessage(text); app?.updateStorageStatus() }
    func selectTool(_ value: ArrangementTool) {
        cancelGesture(); tool = value; toolbar?.display(value)
        if let lane = lanes.first(where: { $0.track == focusTrack }) { focus(lane) }
        message("\(value.rawValue) · \(value.title) · Shift: без сетки · Esc: отменить жест")
        if let app { updateCursor(at: app.window.mouseLocationOutsideOfEventStream, modifiers: NSEvent.modifierFlags) }
    }
    func cancelGesture() { gesture = nil; overlay.rectangles = []; overlay.ramps = []; NSCursor.arrow.set() }
    func lane(at point: NSPoint) -> Lane? {
        guard let scroll = app?.timelineScroll,
              scroll.contentView.convert(scroll.contentView.bounds, to: nil).contains(point) else { return nil }
        return lanes.first { $0.view.bounds.contains($0.view.convert(point, from: nil)) }
    }
    func frame(at point: NSPoint, in lane: Lane) -> UInt64 {
        let local = lane.view.convert(point, from: nil)
        return ArrangementEditMath.frame(x: Double(local.x), width: Double(lane.view.bounds.width),
                                         total: app?.timelineRuler.projectFrames ?? 1)
    }
    func snapped(_ frame: UInt64, flags: NSEvent.ModifierFlags) -> UInt64 {
        guard let app, !flags.contains(.shift) else { return min(frame, Self.limit) }
        let grid = app.gridSnap(atFrame: Int64(min(frame, Self.limit)))
        return ArrangementEditMath.snapped(frame, anchor: grid.anchor, quantum: grid.quantum, limit: Self.limit)
    }
    func rect(_ bounds: ArrangementClipBounds, in lane: Lane) -> NSRect {
        let total = Double(max(1, app?.timelineRuler.projectFrames ?? 1)), width = lane.view.bounds.width
        return NSRect(x: width * CGFloat(Double(bounds.start) / total), y: 5,
                      width: max(2, width * CGFloat(Double(bounds.length) / total)), height: max(1, lane.view.bounds.height - 10))
    }
    func hit(at point: NSPoint, in lane: Lane) -> Item? {
        let local = lane.view.convert(point, from: nil)
        return items.last { $0.key.track == lane.track && rect($0.bounds, in: lane).contains(local) }
    }
    func focus(_ lane: Lane) {
        focusTrack = lane.track
        app?.selectedMixerID = lane.track; app?.inspectorTrackID = lane.track
        if app?.window.makeFirstResponder(lane.view) != true { _ = app?.window.makeFirstResponder(nil) }
    }
    func syncSelection() {
        guard let app else { return }
        for lane in lanes {
            let indices = selection.filter { $0.track == lane.track }.map(\.index).sorted()
            if let wave = lane.view as? WaveformView {
                wave.selectedIndices = indices; wave.selectedIndex = indices.first ?? 0
                wave.selectionActive = !indices.isEmpty; wave.needsDisplay = true
                app.clipSelection[lane.track] = indices
                app.selectedClips[lane.track] = indices.first
            } else if let midi = lane.view as? MidiArrangementView { midi.selectedIndices = Set(indices) }
        }
    }
    func select(_ item: Item, additive: Bool) {
        if additive {
            if !selection.insert(item.key).inserted { selection.remove(item.key) }
        } else if !selection.contains(item.key) { selection = [item.key] }
        app?.selectedMixerID = item.key.track; app?.inspectorTrackID = item.key.track
        if item.key.kind == .midi { app?.midiClipIndex = item.key.index; app?.updateMixerInspector(item.key.track) }
        else { app?.updateClipInspector(item.key.track, item.key.index) }
        syncSelection()
    }
    func action(at point: NSPoint, item: Item, lane: Lane) -> Action {
        if tool == .fade {
            return lane.view.convert(point, from: nil).x < rect(item.bounds, in: lane).midX ? .fadeLeft : .fadeRight
        }
        let local = lane.view.convert(point, from: nil), box = rect(item.bounds, in: lane)
        let handle = min(7, box.width / 4)
        if local.x < box.minX + handle { return .trimLeft }
        if local.x > box.maxX - handle { return .trimRight }
        return .move
    }
    func updateCursor(at point: NSPoint, modifiers: NSEvent.ModifierFlags) {
        guard let lane = lane(at: point) else { NSCursor.arrow.set(); return }
        switch tool {
        case .pointer:
            if let item = hit(at: point, in: lane) {
                if action(at: point, item: item, lane: lane) != .move { NSCursor.resizeLeftRight.set() }
                else { (modifiers.contains(.option) ? NSCursor.dragCopy : NSCursor.openHand).set() }
            } else { NSCursor.arrow.set() }
        case .hand: (gesture == nil ? NSCursor.openHand : NSCursor.closedHand).set()
        case .range, .fade: NSCursor.crosshair.set()
        default:
            if let cursor = cursors[tool] { cursor.set() }
            else if let image = NSImage(systemSymbolName: tool.symbol, accessibilityDescription: tool.title) {
                let cursor = NSCursor(image: image, hotSpot: NSPoint(x: image.size.width / 2, y: image.size.height / 2))
                cursors[tool] = cursor; cursor.set()
            } else { NSCursor.crosshair.set() }
        }
    }

    func handle(_ event: NSEvent) -> Bool {
        guard let app, app.window.attachedSheet == nil, event.window === app.window || event.windowNumber == app.window.windowNumber else { return false }
        switch event.type {
        case .keyDown: return handleKey(event)
        case .mouseMoved, .flagsChanged:
            let point = event.type == .flagsChanged ? app.window.mouseLocationOutsideOfEventStream : event.locationInWindow
            guard lane(at: point) != nil else { if gesture == nil { NSCursor.arrow.set() }; return false }
            updateCursor(at: point, modifiers: event.modifierFlags); return true
        case .rightMouseDown:
            guard let lane = lane(at: event.locationInWindow), let item = hit(at: event.locationInWindow, in: lane) else { return false }
            cancelGesture(); focus(lane); selection = [item.key]; select(item, additive: false)
            return false // Existing native clip menu remains authoritative.
        case .leftMouseDown:
            guard let lane = lane(at: event.locationInWindow) else { return false }
            mouseDown(event, lane: lane); return true
        case .leftMouseDragged:
            guard gesture != nil else { return false }
            mouseDragged(event); return true
        case .leftMouseUp:
            guard let pending = gesture else { return false }
            if pending.dragged { mouseDragged(event) }
            guard let final = gesture else { return true }
            cancelGesture(); commit(final, event: event); return true
        case .magnify:
            guard lane(at: event.locationInWindow) != nil, gesture == nil else { return false }
            zoom(by: max(0.1, 1 + event.magnification), at: event.locationInWindow); return true
        case .scrollWheel:
            guard lane(at: event.locationInWindow) != nil, gesture == nil else { return false }
            if event.modifierFlags.contains(.command) {
                zoom(by: pow(1.015, -event.scrollingDeltaY), at: event.locationInWindow); return true
            }
            if event.modifierFlags.contains(.shift), let scroll = app.timelineScroll {
                let old = scroll.contentView.bounds.origin
                let delta = abs(event.scrollingDeltaX) > abs(event.scrollingDeltaY) ? event.scrollingDeltaX : event.scrollingDeltaY
                app.restoreArrangementViewport(NSPoint(x: old.x - delta, y: old.y)); return true
            }
            return false
        default: return false
        }
    }

    func mouseDown(_ event: NSEvent, lane: Lane) {
        guard let app, let revision = currentRevision() else { return }
        if projectionRevision != revision || documentID != app.midiDocumentID {
            cancelGesture(); app.refresh(); message("Проект обновился. Повтори действие."); return
        }
        cancelGesture(); focus(lane)
        let point = event.locationInWindow, item = hit(at: point, in: lane)
        let additive = event.modifierFlags.contains(.command) || event.modifierFlags.contains(.control) ||
            (tool == .range && event.modifierFlags.contains(.shift))
        var action: Action
        switch tool {
        case .hand: action = .pan
        case .zoom: action = .zoom
        case .range: action = .marquee
        case .draw:
            guard editable, lane.kind != .audio else { message("MIDI-карандаш: выбери MIDI- или пустую дорожку."); return }
            action = .draw
        case .split, .erase, .mute:
            guard let item else { return }
            selection = [item.key]; select(item, additive: false)
            if tool == .split { split(item, at: snapped(frame(at: point, in: lane), flags: event.modifierFlags)) }
            else if tool == .erase { deleteSelection() }
            else { toggleMute(item) }
            return
        case .pointer, .fade:
            if let item {
                select(item, additive: additive)
                if additive { return }
                if event.clickCount >= 2 && item.key.kind == .midi {
                    app.workspace?.selectDock(.midi); return
                }
                if tool == .fade && item.key.kind != .audio { message("Фейды доступны для аудиоклипов."); return }
                guard editable else { message("Останови запись перед редактированием клипов."); return }
                action = self.action(at: point, item: item, lane: lane)
            } else { action = .marquee }
        }
        let chosen = items.filter { selection.contains($0.key) }
        gesture = Gesture(action: action, origin: point, viewport: app.timelineScroll.contentView.bounds.origin,
            originFrame: frame(at: point, in: lane), originDocument: overlay.convert(point, from: nil),
            lane: lane, primary: item, items: chosen, revision: revision, document: app.midiDocumentID,
            copy: event.modifierFlags.contains(.option), additive: additive, target: lane.track)
    }

    func mouseDragged(_ event: NSEvent) {
        guard let app, var g = gesture else { return }
        guard g.document == app.midiDocumentID, currentRevision() == g.revision else {
            cancelGesture(); message("Жест отменён: проект изменился."); return
        }
        let point = event.locationInWindow
        if hypot(point.x - g.origin.x, point.y - g.origin.y) >= 3 { g.dragged = true }
        guard g.dragged else { return }
        if g.action == .pan {
            app.restoreArrangementViewport(NSPoint(x: g.viewport.x - (point.x - g.origin.x),
                y: g.viewport.y + (point.y - g.origin.y)))
            gesture = g; NSCursor.closedHand.set(); return
        }
        _ = app.timelineDocument.autoscroll(with: event)
        let current = snapped(frame(at: point, in: g.lane), flags: event.modifierFlags)
        let start = snapped(g.originFrame, flags: event.modifierFlags)
        let targetLane = lane(at: point) ?? g.lane
        g.target = targetLane.track
        switch g.action {
        case .move:
            guard let primary = g.primary else { return }
            let desired = Int64(primary.bounds.start) + Int64(frame(at: point, in: g.lane)) - Int64(g.originFrame)
            let snappedStart = snapped(UInt64(max(0, desired)), flags: event.modifierFlags)
            let delta = ArrangementEditMath.moveDelta(Int64(snappedStart) - Int64(primary.bounds.start),
                clips: g.items.map(\.bounds), limit: Self.limit)
            g.proposed = g.items.map { var b = $0.bounds; b.start = UInt64(Int64(b.start) + delta); return b }
            overlay.rectangles = g.proposed.enumerated().compactMap { index, b in
                let source = g.items[index]
                let lane = g.items.count == 1 ? targetLane : lanes.first { $0.track == source.key.track }
                return lane.map { $0.view.convert(rect(b, in: $0), to: overlay) }
            }
        case .trimLeft, .trimRight, .fadeLeft, .fadeRight:
            guard let primary = g.primary else { return }
            let result: ArrangementClipBounds?
            if g.action == .fadeLeft || g.action == .fadeRight {
                result = ArrangementEditMath.fade(primary.bounds, frame: current, left: g.action == .fadeLeft)
            } else {
                result = ArrangementEditMath.trim(primary.bounds, frame: current, left: g.action == .trimLeft,
                    midi: primary.key.kind == .midi, limit: Self.limit)
            }
            g.proposed = result.map { [$0] } ?? []
            overlay.rectangles = g.proposed.map { g.lane.view.convert(rect($0, in: g.lane), to: overlay) }
            if let b = result, g.action == .fadeLeft || g.action == .fadeRight {
                let left = g.action == .fadeLeft
                overlay.ramps = [(g.lane.view.convert(rect(b, in: g.lane), to: overlay), left,
                    CGFloat(Double(left ? b.fadeIn : b.fadeOut) / Double(max(1, b.length))))]
            }
        case .draw:
            let end = max(start, current)
            let b = ArrangementClipBounds(start: min(start, current), length: max(1, end - min(start, current)))
            g.proposed = [b]; overlay.rectangles = [g.lane.view.convert(rect(b, in: g.lane), to: overlay)]
        case .marquee, .zoom:
            let a = g.originDocument, b = overlay.convert(point, from: nil)
            overlay.rectangles = [NSRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))]
        case .pan: break
        }
        gesture = g
    }
}
