import AppKit

@MainActor
final class PRProWorkspaceView: NSView {
    let state: PRProState
    let canvas: PRProCanvas
    private let scrollView = NSScrollView()
    private let ruler: PRProRulerView
    private let keyboard: PRProKeyboardView
    private let velocityLane: PRProVelocityLane
    private let laneGrip = PRProLaneGrip()
    private let toolbar = NSStackView()
    private let inspector = NSStackView()
    private let statusField = NSTextField(labelWithString: "")

    private let toolControl = NSSegmentedControl(labels: PRTool.allCases.map { $0.shortcut },
                                                 trackingMode: .selectOne, target: nil, action: nil)
    private let gridPopup = NSPopUpButton()
    private let swingSlider = NSSlider(value: 0, minValue: 0, maxValue: 0.75, target: nil, action: nil)
    private let rootPopup = NSPopUpButton()
    private let scalePopup = NSPopUpButton()
    private let scaleLock = NSButton(title: "Lock", target: nil, action: nil)
    private let foldButton = NSButton(title: "Fold", target: nil, action: nil)
    private let followButton = NSButton(title: "Follow", target: nil, action: nil)
    private let selectionLabel = NSTextField(labelWithString: "Нет выделения")
    private let chordLabel = NSTextField(wrappingLabelWithString: "")
    private let pitchField = NSTextField(string: "")
    private let startField = NSTextField(string: "")
    private let lengthField = NSTextField(string: "")
    private let velocityField = NSTextField(string: "")
    private let channelField = NSTextField(string: "")
    private let ratchetCount = NSPopUpButton()
    private let ratchetGate = NSSlider(value: 1, minValue: 0.05, maxValue: 1, target: nil, action: nil)
    private let strumAmount = NSSlider(value: 0.12, minValue: 0, maxValue: 1, target: nil, action: nil)
    private let strumDirection = NSSegmentedControl(labels: ["↑", "↓"], trackingMode: .selectOne, target: nil, action: nil)
    private let rampFrom = NSTextField(string: "60")
    private let rampTo = NSTextField(string: "110")
    private let transformApplyButton = NSButton(title: "Apply preview", target: nil, action: nil)
    private let transformCancelButton = NSButton(title: "Cancel", target: nil, action: nil)
    private let chordRoot = NSPopUpButton()
    private let chordKind = NSPopUpButton()
    private let chordInversion = NSPopUpButton()

    private var laneHeight: CGFloat = 125
    private(set) var rows = PRPitchRows()
    var onSeek: ((UInt64) -> Void)?
    var onUndo: (() -> Void)? { didSet { canvas.onUndo = onUndo } }
    var onRedo: (() -> Void)? { didSet { canvas.onRedo = onRedo } }
    var onPlayToggle: (() -> Void)? { didSet { canvas.onPlayToggle = onPlayToggle } }

    override var isFlipped: Bool { true }
    var scrollOrigin: NSPoint { scrollView.contentView.bounds.origin }

    init(state: PRProState) {
        self.state = state
        canvas = PRProCanvas(state: state)
        ruler = PRProRulerView(state: state)
        keyboard = PRProKeyboardView(state: state)
        velocityLane = PRProVelocityLane(state: state)
        super.init(frame: .zero)
        canvas.workspace = self
        ruler.workspace = self
        keyboard.workspace = self
        velocityLane.workspace = self
        setupToolbar()
        setupInspector()
        setupViewport()
        statusField.font = DAWDesignTokens.Typography.caption
        statusField.textColor = DAWDesignTokens.Color.secondaryText
        statusField.lineBreakMode = .byTruncatingTail
        statusField.setAccessibilityLabel("Статус Piano Roll")
        addSubview(statusField)
        laneGrip.onDelta = { [weak self] delta in
            guard let self else { return }
            self.laneHeight = min(260, max(70, self.laneHeight - delta))
            self.needsLayout = true
        }
        refreshGeometry()
        refreshFromState()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    deinit { NotificationCenter.default.removeObserver(self) }

    private func setupViewport() {
        scrollView.documentView = canvas
        scrollView.hasHorizontalScroller = true
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = false
        scrollView.drawsBackground = false
        scrollView.borderType = .noBorder
        scrollView.horizontalScrollElasticity = .none
        scrollView.verticalScrollElasticity = .none
        scrollView.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(self, selector: #selector(scrolled(_:)),
                                               name: NSView.boundsDidChangeNotification,
                                               object: scrollView.contentView)
        [toolbar, ruler, keyboard, scrollView, laneGrip, velocityLane, inspector].forEach(addSubview)
    }

    private func setupToolbar() {
        toolbar.orientation = .horizontal
        toolbar.alignment = .centerY
        toolbar.spacing = 6
        toolControl.target = self; toolControl.action = #selector(changeTool)
        toolControl.selectedSegment = PRTool.select.rawValue
        for (index, tool) in PRTool.allCases.enumerated() {
            toolControl.setToolTip("\(tool.title) · \(tool.shortcut)", forSegment: index)
        }
        gridPopup.addItems(withTitles: PRGridDivision.allCases.map(\.title))
        gridPopup.selectItem(at: PRGridDivision.sixteenth.rawValue)
        gridPopup.target = self; gridPopup.action = #selector(changeGrid)
        swingSlider.target = self; swingSlider.action = #selector(changeSwing); swingSlider.isContinuous = true
        swingSlider.widthAnchor.constraint(equalToConstant: 90).isActive = true
        rootPopup.addItems(withTitles: PRPitch.names)
        rootPopup.target = self; rootPopup.action = #selector(changeScale)
        scalePopup.addItems(withTitles: PRScaleKind.allCases.map(\.title))
        scalePopup.target = self; scalePopup.action = #selector(changeScale)
        for button in [scaleLock, foldButton, followButton] {
            button.setButtonType(.toggle); button.bezelStyle = .texturedRounded
            button.font = DAWDesignTokens.Typography.caption
        }
        scaleLock.target = self; scaleLock.action = #selector(toggleScaleLock)
        foldButton.target = self; foldButton.action = #selector(toggleFold)
        followButton.target = self; followButton.action = #selector(toggleFollow)
        let quantize = button("Q Quantize", #selector(quantizeNow))
        let legato = button("Legato", #selector(legatoNow))
        let humanize = button("Humanize", #selector(humanizeNow))
        let reverse = button("Reverse", #selector(reverseNow))
        let zoomOut = button("−", #selector(zoomOutNow))
        let zoomIn = button("+", #selector(zoomInNow))
        let fit = button("Fit", #selector(fitNow))
        [toolControl, separator(), caption("Grid"), gridPopup, caption("Swing"), swingSlider,
         separator(), rootPopup, scalePopup, scaleLock, foldButton, followButton,
         separator(), quantize, legato, humanize, reverse, flexible(), fit, zoomOut, zoomIn].forEach {
            toolbar.addArrangedSubview($0)
        }
    }

    private func setupInspector() {
        inspector.orientation = .vertical
        inspector.alignment = .leading
        inspector.spacing = 6
        inspector.edgeInsets = NSEdgeInsets(top: 10, left: 10, bottom: 10, right: 10)
        inspector.wantsLayer = true
        inspector.layer?.backgroundColor = DAWDesignTokens.Color.surface.cgColor
        selectionLabel.font = DAWDesignTokens.Typography.label
        chordLabel.font = DAWDesignTokens.Typography.caption
        chordLabel.textColor = DAWDesignTokens.Color.secondaryText
        inspector.addArrangedSubview(caption("SELECTION"))
        inspector.addArrangedSubview(selectionLabel)
        inspector.addArrangedSubview(chordLabel)
        inspector.addArrangedSubview(fieldRow("Pitch", pitchField, #selector(editPitch)))
        inspector.addArrangedSubview(fieldRow("Start · beat", startField, #selector(editTiming)))
        inspector.addArrangedSubview(fieldRow("Length · beat", lengthField, #selector(editTiming)))
        inspector.addArrangedSubview(fieldRow("Velocity", velocityField, #selector(editVelocity)))
        inspector.addArrangedSubview(fieldRow("Channel 1–16", channelField, #selector(editChannel)))
        inspector.addArrangedSubview(divider())
        inspector.addArrangedSubview(caption("RATCHET"))
        ratchetCount.addItems(withTitles: ["2", "3", "4", "6", "8", "12", "16", "32"])
        ratchetCount.selectItem(withTitle: "4")
        ratchetCount.target = self; ratchetCount.action = #selector(previewRatchetNow)
        ratchetGate.isContinuous = true
        ratchetGate.target = self; ratchetGate.action = #selector(previewRatchetNow)
        inspector.addArrangedSubview(row("Repeats", ratchetCount))
        inspector.addArrangedSubview(row("Gate", ratchetGate))
        inspector.addArrangedSubview(button("Preview Ratchet", #selector(previewRatchetNow)))
        inspector.addArrangedSubview(divider())
        inspector.addArrangedSubview(caption("STRUM"))
        strumDirection.selectedSegment = 0
        strumDirection.target = self; strumDirection.action = #selector(previewStrumNow)
        strumAmount.isContinuous = true
        strumAmount.target = self; strumAmount.action = #selector(previewStrumNow)
        inspector.addArrangedSubview(row("Direction", strumDirection))
        inspector.addArrangedSubview(row("Spread · beat", strumAmount))
        inspector.addArrangedSubview(button("Preview Strum", #selector(previewStrumNow)))
        inspector.addArrangedSubview(divider())
        inspector.addArrangedSubview(caption("VELOCITY RAMP"))
        rampFrom.target = self; rampFrom.action = #selector(previewRampNow)
        rampTo.target = self; rampTo.action = #selector(previewRampNow)
        let ramp = NSStackView(views: [rampFrom, NSTextField(labelWithString: "→"), rampTo])
        ramp.spacing = 4; ramp.alignment = .centerY
        inspector.addArrangedSubview(ramp)
        inspector.addArrangedSubview(button("Preview Ramp", #selector(previewRampNow)))
        inspector.addArrangedSubview(divider())
        inspector.addArrangedSubview(caption("TRANSFORM PREVIEW"))
        transformApplyButton.target = self; transformApplyButton.action = #selector(applyTransformPreview)
        transformCancelButton.target = self; transformCancelButton.action = #selector(cancelTransformPreview)
        for button in [transformApplyButton, transformCancelButton] {
            button.bezelStyle = .texturedRounded
            button.font = DAWDesignTokens.Typography.caption
        }
        transformApplyButton.contentTintColor = DAWDesignTokens.Color.mint
        let previewActions = NSStackView(views: [transformApplyButton, transformCancelButton])
        previewActions.spacing = 5; previewActions.alignment = .centerY
        inspector.addArrangedSubview(previewActions)
        inspector.addArrangedSubview(divider())
        inspector.addArrangedSubview(caption("CHORD STAMP"))
        chordRoot.addItems(withTitles: PRPitch.names)
        chordKind.addItems(withTitles: PRChordKind.allCases.map(\.title))
        chordKind.target = self; chordKind.action = #selector(chordKindChanged)
        inspector.addArrangedSubview(row("Root", chordRoot))
        inspector.addArrangedSubview(row("Type", chordKind))
        inspector.addArrangedSubview(row("Inversion", chordInversion))
        inspector.addArrangedSubview(button("Stamp chord at cursor", #selector(stampChord)))
        chordKindChanged()
        for view in inspector.arrangedSubviews {
            view.widthAnchor.constraint(equalTo: inspector.widthAnchor, constant: -20).isActive = true
        }
    }

    private func button(_ title: String, _ action: Selector) -> NSButton {
        let result = NSButton(title: title, target: self, action: action)
        result.bezelStyle = .texturedRounded
        result.font = DAWDesignTokens.Typography.caption
        return result
    }

    private func caption(_ value: String) -> NSTextField {
        let label = NSTextField(labelWithString: value)
        label.font = .systemFont(ofSize: 9, weight: .semibold)
        label.textColor = DAWDesignTokens.Color.secondaryText
        return label
    }

    private func separator() -> NSView {
        let view = NSView(); view.wantsLayer = true
        view.layer?.backgroundColor = DAWDesignTokens.Color.border.withAlphaComponent(0.6).cgColor
        view.widthAnchor.constraint(equalToConstant: 1).isActive = true
        view.heightAnchor.constraint(equalToConstant: 22).isActive = true
        return view
    }

    private func divider() -> NSView {
        let view = NSView(); view.wantsLayer = true
        view.layer?.backgroundColor = DAWDesignTokens.Color.border.withAlphaComponent(0.55).cgColor
        view.heightAnchor.constraint(equalToConstant: 1).isActive = true
        return view
    }

    private func flexible() -> NSView {
        let view = NSView()
        view.setContentHuggingPriority(.defaultLow, for: .horizontal)
        view.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return view
    }

    private func row(_ title: String, _ control: NSView) -> NSStackView {
        let stack = NSStackView(views: [caption(title), control])
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 3
        control.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        return stack
    }

    private func fieldRow(_ title: String, _ field: NSTextField, _ action: Selector) -> NSStackView {
        field.target = self; field.action = action
        field.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        return row(title, field)
    }

    override func layout() {
        super.layout()
        let toolbarHeight: CGFloat = 40
        let statusHeight: CGFloat = 24
        let inspectorWidth: CGFloat = min(250, max(210, bounds.width * 0.21))
        let editorWidth = max(400, bounds.width - inspectorWidth)
        let rulerHeight: CGFloat = 30
        let keyboardWidth: CGFloat = 64
        let gripHeight: CGFloat = 8
        let available = max(220, bounds.height - toolbarHeight - statusHeight)
        let actualLaneHeight = state.showVelocity ? min(laneHeight, max(70, available * 0.38)) : 0
        let notesHeight = max(120, available - rulerHeight - gripHeight - actualLaneHeight)
        toolbar.frame = NSRect(x: 8, y: 4, width: max(0, bounds.width - 16), height: toolbarHeight - 8)
        ruler.frame = NSRect(x: keyboardWidth, y: toolbarHeight,
                             width: max(0, editorWidth - keyboardWidth), height: rulerHeight)
        keyboard.frame = NSRect(x: 0, y: toolbarHeight + rulerHeight,
                                width: keyboardWidth, height: notesHeight)
        scrollView.frame = NSRect(x: keyboardWidth, y: toolbarHeight + rulerHeight,
                                  width: max(0, editorWidth - keyboardWidth), height: notesHeight)
        laneGrip.frame = NSRect(x: 0, y: toolbarHeight + rulerHeight + notesHeight,
                                width: editorWidth, height: state.showVelocity ? gripHeight : 0)
        velocityLane.frame = NSRect(x: keyboardWidth,
                                    y: toolbarHeight + rulerHeight + notesHeight + (state.showVelocity ? gripHeight : 0),
                                    width: max(0, editorWidth - keyboardWidth - 15),
                                    height: actualLaneHeight)
        velocityLane.isHidden = !state.showVelocity
        laneGrip.isHidden = !state.showVelocity
        inspector.frame = NSRect(x: editorWidth, y: toolbarHeight,
                                 width: inspectorWidth,
                                 height: max(0, bounds.height - toolbarHeight - statusHeight))
        statusField.frame = NSRect(x: 10, y: bounds.height - statusHeight + 3,
                                   width: max(0, bounds.width - 20), height: statusHeight - 5)
        refreshGeometry()
    }

    func refreshGeometry() {
        rows = state.pitchRows
        let duration = state.timeMap?.durationBeats ?? 16
        let maxPixels = 16_000_000.0
        state.pixelsPerBeat = min(640, max(4, min(state.pixelsPerBeat, maxPixels / max(1, duration))))
        let width = max(Double(scrollView.contentSize.width), duration * state.pixelsPerBeat + 8)
        let height = max(Double(scrollView.contentSize.height), Double(rows.pitches.count) * state.rowHeight)
        canvas.setFrameSize(NSSize(width: width, height: height))
        scroll(to: scrollOrigin)
        syncSurfaces()
    }

    func refreshFromState() {
        toolControl.selectedSegment = state.tool.rawValue
        gridPopup.selectItem(at: state.grid.division.rawValue)
        swingSlider.doubleValue = state.grid.swing
        rootPopup.selectItem(at: state.scale.root)
        scalePopup.selectItem(at: state.scale.kind.rawValue)
        scaleLock.state = state.lockScale ? .on : .off
        foldButton.state = state.fold ? .on : .off
        followButton.state = state.followPlayhead ? .on : .off
        transformApplyButton.isEnabled = state.hasTransformPreview
        transformCancelButton.isEnabled = state.hasTransformPreview
        statusField.stringValue = state.status
        updateInspector()
        refreshGeometry()
        canvas.needsDisplay = true
    }

    private func updateInspector() {
        let selected = state.selectedEntities
        selectionLabel.stringValue = selected.isEmpty ? "Нет выделения" : "Нот: \(selected.count)"
        let candidates = state.chordCandidates()
        chordLabel.stringValue = candidates.prefix(3).joined(separator: " · ")
        guard let map = state.timeMap, let first = selected.first else {
            [pitchField, startField, lengthField, velocityField, channelField].forEach { $0.stringValue = ""; $0.isEnabled = false }
            return
        }
        let single = selected.count == 1
        pitchField.isEnabled = single
        startField.isEnabled = single
        lengthField.isEnabled = single
        velocityField.isEnabled = true
        channelField.isEnabled = true
        pitchField.stringValue = single ? PRPitch.name(first.note.pitch) : "—"
        startField.stringValue = single ? String(format: "%.4f", map.start(first.note)) : "—"
        lengthField.stringValue = single ? String(format: "%.4f", map.length(first.note)) : "—"
        velocityField.stringValue = selected.map(\.note.velocity).allSatisfy { $0 == first.note.velocity } ? String(first.note.velocity) : "—"
        channelField.stringValue = selected.map(\.note.channel).allSatisfy { $0 == first.note.channel } ? String(Int(first.note.channel) + 1) : "—"
    }

    @objc private func scrolled(_ note: Notification) { syncSurfaces() }

    private func syncSurfaces() {
        ruler.needsDisplay = true
        keyboard.needsDisplay = true
        velocityLane.needsDisplay = true
        canvas.needsDisplay = true
    }

    func focusCanvas() { window?.makeFirstResponder(canvas) }

    func scroll(to point: NSPoint) {
        let maxX = max(0, canvas.bounds.width - scrollView.contentSize.width)
        let maxY = max(0, canvas.bounds.height - scrollView.contentSize.height)
        let target = NSPoint(x: min(maxX, max(0, point.x)), y: min(maxY, max(0, point.y)))
        scrollView.contentView.scroll(to: target)
        scrollView.reflectScrolledClipView(scrollView.contentView)
        syncSurfaces()
    }

    func zoomHorizontal(factor: Double, anchorInCanvas: NSPoint?) {
        guard factor.isFinite, factor > 0, !state.isGesturing else { return }
        let anchor = anchorInCanvas ?? NSPoint(x: scrollOrigin.x + scrollView.contentSize.width * 0.5,
                                               y: scrollOrigin.y)
        let beat = Double(anchor.x) / state.pixelsPerBeat
        let screenX = anchor.x - scrollOrigin.x
        state.pixelsPerBeat = min(640, max(4, state.pixelsPerBeat * factor))
        refreshGeometry()
        scroll(to: NSPoint(x: beat * state.pixelsPerBeat - screenX, y: scrollOrigin.y))
    }

    func zoomVertical(factor: Double, anchorInCanvas: NSPoint?) {
        guard factor.isFinite, factor > 0, !state.isGesturing else { return }
        let anchor = anchorInCanvas ?? NSPoint(x: scrollOrigin.x,
                                               y: scrollOrigin.y + scrollView.contentSize.height * 0.5)
        let row = Double(anchor.y) / state.rowHeight
        let screenY = anchor.y - scrollOrigin.y
        state.rowHeight = min(42, max(9, state.rowHeight * factor))
        refreshGeometry()
        scroll(to: NSPoint(x: scrollOrigin.x, y: row * state.rowHeight - screenY))
    }

    func fit(selectionOnly: Bool) {
        guard let map = state.timeMap, !state.isGesturing else { return }
        let selected = state.selectedEntities
        let source = selectionOnly && !selected.isEmpty ? selected : state.entities
        let left = selectionOnly ? source.map { map.start($0.note) }.min() ?? 0 : 0
        let right = selectionOnly ? source.map { map.end($0.note) }.max() ?? map.durationBeats : map.durationBeats
        let width = max(100, scrollView.contentSize.width)
        state.pixelsPerBeat = min(320, max(4, Double(width - 50) / max(0.25, right - left)))
        if let low = source.map(\.note.pitch).min(), let high = source.map(\.note.pitch).max() {
            let lowRow = rows.row(for: low) ?? 0
            let highRow = rows.row(for: high) ?? 0
            let span = max(1, lowRow - highRow)
            state.rowHeight = min(30, max(10, Double(scrollView.contentSize.height - 30) / Double(span + 2)))
        }
        refreshGeometry()
        let middlePitch = UInt8((Int(source.map(\.note.pitch).min() ?? 48) + Int(source.map(\.note.pitch).max() ?? 72)) / 2)
        let middleRow = rows.row(for: middlePitch) ?? rows.pitches.count / 2
        scroll(to: NSPoint(x: max(0, left * state.pixelsPerBeat - 20),
                           y: max(0, Double(middleRow) * state.rowHeight - scrollView.contentSize.height * 0.5)))
    }

    func seek(relativeBeat: Double) {
        guard let map = state.timeMap else { return }
        let beat = min(map.durationBeats, max(0, relativeBeat))
        state.insertionBeat = beat
        onSeek?(map.clipStart + map.frame(at: beat))
        state.changed()
    }

    func updatePlayhead(_ frame: UInt64) {
        state.playheadFrame = frame
        guard let map = state.timeMap else { syncSurfaces(); return }
        if state.followPlayhead, !state.isGesturing,
           frame >= map.clipStart, frame <= map.clipStart + map.clipLength {
            let x = map.beat(at: frame - map.clipStart) * state.pixelsPerBeat
            let left = Double(scrollOrigin.x), right = left + Double(scrollView.contentSize.width)
            if x < left || x > left + (right - left) * 0.88 {
                scroll(to: NSPoint(x: max(0, x - Double(scrollView.contentSize.width) * 0.15), y: scrollOrigin.y))
            }
        }
        syncSurfaces()
    }

    @objc private func changeTool() {
        guard let tool = PRTool(rawValue: toolControl.selectedSegment) else { return }
        state.tool = tool; state.setStatus("Инструмент: \(tool.title) · \(tool.shortcut)")
        focusCanvas()
    }

    @objc private func changeGrid() {
        state.grid.division = PRGridDivision(rawValue: gridPopup.indexOfSelectedItem) ?? .sixteenth
        state.changed(); focusCanvas()
    }

    @objc private func changeSwing() {
        state.grid.swing = min(0.75, max(0, swingSlider.doubleValue))
        state.changed()
    }

    @objc private func changeScale() {
        state.scale.root = max(0, rootPopup.indexOfSelectedItem)
        state.scale.kind = PRScaleKind(rawValue: scalePopup.indexOfSelectedItem) ?? .chromatic
        state.changed()
    }

    @objc private func toggleScaleLock() { state.lockScale = scaleLock.state == .on; state.changed() }
    @objc private func toggleFold() { state.fold = foldButton.state == .on; state.changed(); refreshGeometry() }
    @objc private func toggleFollow() { state.followPlayhead = followButton.state == .on; state.changed() }
    @objc private func quantizeNow() { state.quantize(); focusCanvas() }
    @objc private func legatoNow() { state.legato(); focusCanvas() }
    @objc private func humanizeNow() { state.humanize(); focusCanvas() }
    @objc private func reverseNow() { state.reverseSelection(); focusCanvas() }
    @objc private func zoomOutNow() { zoomHorizontal(factor: 0.8, anchorInCanvas: nil) }
    @objc private func zoomInNow() { zoomHorizontal(factor: 1.25, anchorInCanvas: nil) }
    @objc private func fitNow() { fit(selectionOnly: false) }

    @objc private func editPitch() {
        guard state.selectedEntities.count == 1, let pitch = PRPitch.parse(pitchField.stringValue) else { updateInspector(); return }
        let id = state.selectedEntities[0].id
        state.perform { items, _ in items.map { entity in
            guard entity.id == id else { return entity }
            var changed = entity; changed.note.pitch = pitch; return changed
        } }
        focusCanvas()
    }

    @objc private func editTiming() {
        guard state.selectedEntities.count == 1, state.timeMap != nil,
              let start = Double(startField.stringValue.replacingOccurrences(of: ",", with: ".")),
              let length = Double(lengthField.stringValue.replacingOccurrences(of: ",", with: ".")),
              length > 0 else { updateInspector(); return }
        let selected = state.selectedEntities[0]
        state.perform { items, map in try items.map { entity in
            guard entity.id == selected.id else { return entity }
            return PRNoteEntity(id: entity.id, note: try map.note(start: start, end: start + length,
                pitch: entity.note.pitch, channel: entity.note.channel, velocity: entity.note.velocity))
        } }
        focusCanvas()
    }

    @objc private func editVelocity() {
        guard let value = Int(velocityField.stringValue), (1...127).contains(value) else { updateInspector(); return }
        state.setVelocity(value); focusCanvas()
    }

    @objc private func editChannel() {
        guard let value = Int(channelField.stringValue), (1...16).contains(value) else { updateInspector(); return }
        state.setChannel(value - 1); focusCanvas()
    }

    @objc private func previewRatchetNow() {
        guard !state.selection.isEmpty else { state.setStatus("Выберите ноты для Ratchet"); return }
        let count = Int(ratchetCount.titleOfSelectedItem ?? "4") ?? 4
        state.previewRatchet(count: count, gate: ratchetGate.doubleValue)
        focusCanvas()
    }

    @objc private func previewStrumNow() {
        guard !state.selection.isEmpty else { state.setStatus("Выберите аккорд для Strum"); return }
        state.previewStrum(spreadBeats: strumAmount.doubleValue,
                           descending: strumDirection.selectedSegment == 1)
        focusCanvas()
    }

    @objc private func previewRampNow() {
        guard !state.selection.isEmpty else { state.setStatus("Выберите ноты для Velocity Ramp"); return }
        guard let from = Int(rampFrom.stringValue), let to = Int(rampTo.stringValue) else {
            state.setStatus("Velocity Ramp: значения должны быть 1–127")
            return
        }
        state.previewVelocityRamp(from: from, to: to)
        focusCanvas()
    }

    @objc private func applyTransformPreview() {
        state.applyTransformPreview()
        focusCanvas()
    }

    @objc private func cancelTransformPreview() {
        state.cancelTransformPreview()
        focusCanvas()
    }

    @objc private func chordKindChanged() {
        let kind = PRChordKind(rawValue: chordKind.indexOfSelectedItem) ?? .major
        chordInversion.removeAllItems()
        chordInversion.addItems(withTitles: kind.intervals.indices.map { $0 == 0 ? "Root" : "Inv \($0)" })
        chordInversion.selectItem(at: 0)
    }

    @objc private func stampChord() {
        let kind = PRChordKind(rawValue: chordKind.indexOfSelectedItem) ?? .major
        let root = 60 + chordRoot.indexOfSelectedItem
        state.insertChord(root: root, kind: kind,
                          inversion: max(0, chordInversion.indexOfSelectedItem))
        focusCanvas()
    }
}
