import AppKit

/// Temporary track selection, not a durable VCA or an audio-routing group.
/// It contains no authoritative project values and writes only through callbacks.
@MainActor
final class MixerLinkedLevels {
    struct Plan {
        let sourceID: UInt64
        let initial: [UInt64: Double]
        let minimumDelta: Double
        let maximumDelta: Double
        var delta: Double = 0
        init?(sourceID: UInt64, models: [MixerStripModel]) {
            guard models.count >= 2, models.allSatisfy({ $0.kind == .track && $0.volumeDb.isFinite }),
                  Set(models.map(\.id)).count == models.count,
                  models.contains(where: { $0.id == sourceID }) else { return nil }
            self.sourceID = sourceID
            initial = Dictionary(uniqueKeysWithValues: models.map { ($0.id, $0.volumeDb) })
            minimumDelta = models.map { -120 - $0.volumeDb }.max()!
            maximumDelta = models.map { 24 - $0.volumeDb }.min()!
        }
        mutating func update(sourceValue: Double) -> Bool {
            guard sourceValue.isFinite, let base = initial[sourceID] else { return false }
            delta = min(maximumDelta, max(minimumDelta, sourceValue - base))
            if abs(delta) < 1e-9 { delta = 0 } // Round-tripping the nonlinear fader is a no-op.
            return true
        }
        var levels: [UInt64: Double] { initial.mapValues { min(24, max(-120, $0 + delta)) } }
    }
    private enum Gesture { case idle, single(UInt64), linked(Plan), rejected }
    private var gesture: Gesture = .idle
    private weak var workspace: MixerWorkspaceView?
    private(set) var selectedIDs = Set<UInt64>()
    private var anchor: UInt64?
    private var primary: UInt64?
    private(set) var linkEnabled = true
    var onBegin: (([UInt64]) -> Bool)?
    var onDelta: ((Double) -> Bool)?
    var onEnd: (() -> Void)?
    var onCancel: (() -> Void)?
    let bar = NSView()
    let linkButton = MixerActionButton("Link levels")
    let clearButton = MixerActionButton("Clear selection")
    let summary = NSTextField(labelWithString: "")
    var isEditing: Bool { if case .idle = gesture { return false }; return true }
    var barHeight: CGFloat { selectedIDs.count > 1 ? 32 : 0 }

    init(workspace: MixerWorkspaceView) {
        self.workspace = workspace
        bar.wantsLayer = true; bar.layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        summary.font = .systemFont(ofSize: 11, weight: .medium)
        summary.lineBreakMode = .byTruncatingTail
        summary.textColor = DAWDesignTokens.Color.secondaryText
        linkButton.setButtonType(.toggle); linkButton.state = .on
        linkButton.setAccessibilityLabel("Link selected track faders")
        linkButton.toolTip = "Move selected static track levels by one common dB offset. No bus, Master, send or automation grouping."
        linkButton.invoke = { [weak self] in guard let self else { return }; setLinked(!linkEnabled) }
        clearButton.invoke = { [weak self] in self?.clear() }
        for view in [summary, linkButton, clearButton] { bar.addSubview(view) }
    }
    func setLinked(_ value: Bool) {
        guard !isEditing else { return }
        linkEnabled = value; refreshPresentation()
    }
    func clear() {
        guard !isEditing else { return }
        selectedIDs = primary.map { selectedIDs.contains($0) ? Set([$0]) : [] } ?? []
        refreshPresentation(); workspace?.needsLayout = true
    }
    func synchronize(_ models: [MixerStripModel]) {
        guard !isEditing else { return }
        let valid = Set(models.filter { $0.kind == .track }.map(\.id))
        let incoming = models.first { $0.isSelected }?.id
        // External arranger/inspector selection replaces the temporary selection.
        // Our own click assigns primary before the synchronous onSelect/refresh.
        if incoming != primary {
            primary = incoming; anchor = incoming
            selectedIDs = incoming.map { valid.contains($0) ? Set([$0]) : [] } ?? []
        }
        selectedIDs.formIntersection(valid)
        if let anchor, !valid.contains(anchor) { self.anchor = nil }
    }
    func prune(visibleIDs: [UInt64]) {
        guard !isEditing else { return }
        selectedIDs.formIntersection(visibleIDs)
        if let anchor, !visibleIDs.contains(anchor) { self.anchor = nil }
    }
    func select(_ id: UInt64, modifiers: NSEvent.ModifierFlags = []) {
        guard !isEditing, let workspace, let model = workspace.strips.first(where: { $0.id == id }) else { return }
        let order = workspace.visibleIDs.filter { candidate in workspace.strips.contains { $0.id == candidate && $0.kind == .track } }
        if model.kind != .track { selectedIDs.removeAll(); anchor = nil; primary = id }
        else if modifiers.contains(.shift), let anchor, let start = order.firstIndex(of: anchor), let end = order.firstIndex(of: id) {
            let range = Set(order[min(start,end)...max(start,end)])
            selectedIDs = modifiers.contains(.command) ? selectedIDs.union(range) : range
            primary = id
        } else if modifiers.contains(.command) {
            if selectedIDs.contains(id) { selectedIDs.remove(id) } else { selectedIDs.insert(id) }
            primary = selectedIDs.contains(id) ? id : order.last { selectedIDs.contains($0) } ?? id
            anchor = primary
        } else { selectedIDs = [id]; primary = id; anchor = id }
        if let primary { workspace.onSelect?(primary) }
        refreshPresentation(); workspace.needsLayout = true
    }
    func refreshPresentation() {
        guard let workspace else { return }
        for view in workspace.presentedStripViews { view.isGroupSelected = selectedIDs.contains(view.model.id) && selectedIDs.count > 1 }
        let sendMode = workspace.sendTargetID != nil
        summary.stringValue = "\(selectedIDs.count) tracks selected · \(sendMode ? "Send controls stay individual" : linkEnabled ? "Linked static levels" : "Individual levels")"
        linkButton.state = linkEnabled ? .on : .off
        linkButton.isEnabled = workspace.editingEnabled && !sendMode && !isEditing
        clearButton.isEnabled = !isEditing
        bar.isHidden = barHeight == 0
        if case .linked(let plan) = gesture { show(plan.levels) }
    }
    func layoutBar(width: CGFloat, top: CGFloat) {
        bar.frame = NSRect(x: 5, y: top, width: max(0,width-10), height: barHeight)
        let controls = min(240, max(0,bar.bounds.width-120))
        summary.frame = NSRect(x: 10, y: 8, width: max(0,bar.bounds.width-controls-20), height: 18)
        linkButton.frame = NSRect(x: max(0,bar.bounds.width-controls), y: 3, width: 106, height: 25)
        clearButton.frame = NSRect(x: max(0,bar.bounds.width-126), y: 3, width: 120, height: 25)
        refreshPresentation()
    }
    func begin(_ id: UInt64) {
        guard !isEditing, let workspace, workspace.editingEnabled else { return }
        if linkEnabled && selectedIDs.count > 1 && selectedIDs.contains(id) && workspace.sendTargetID == nil {
            let models = workspace.strips.filter { selectedIDs.contains($0.id) && $0.kind == .track }
            guard let plan = Plan(sourceID: id, models: models) else { gesture = .rejected; return }
            // Never silently fall back to an individual edit on a rejected group.
            gesture = .rejected
            if onBegin?(models.map(\.id)) == true { gesture = .linked(plan) }
        } else {
            gesture = .single(id); workspace.onVolumeGestureBegin?(id)
        }
        refreshPresentation()
    }
    func change(_ id: UInt64, value: Double, source: MixerStripView) {
        guard let workspace else { return }
        switch gesture {
        case .single(let member) where member == id:
            workspace.previewLevel(id, send: nil, value: value, source: source)
            workspace.onVolume?(id,value)
        case .linked(var plan) where plan.sourceID == id:
            guard plan.update(sourceValue: value) else { return }
            guard onDelta?(plan.delta) == true else {
                gesture = .rejected; onCancel?(); restoreLevels(); return
            }
            gesture = .linked(plan); show(plan.levels)
        default: break
        }
    }
    func end(_ id: UInt64, value: Double) {
        let finished = gesture; gesture = .idle
        switch finished {
        case .single(let member) where member == id: workspace?.onVolumeGestureEnd?(id,value)
        case .linked: onEnd?()
        default: break
        }
        restoreLevels(); refreshPresentation()
    }
    func cancel() {
        let previous = gesture; gesture = .idle
        switch previous {
        case .linked: onCancel?()
        case .single: workspace?.onVolumeGestureCancel?()
        default: break
        }
        restoreLevels(); refreshPresentation()
    }
    private func show(_ levels: [UInt64:Double]) {
        for view in workspace?.presentedStripViews ?? [] where view.sendTarget == nil {
            if let value = levels[view.model.id] {
                view.fader.displayPreview(value); view.gainField.stringValue = MixerScale.label(value)
            }
        }
    }
    private func restoreLevels() {
        for view in workspace?.presentedStripViews ?? [] { view.updateFaderMode() }
    }
}
