import AppKit

@MainActor
private final class PRMiniPreviewView: NSView {
    let state: PRProState
    var onOpen: (() -> Void)?
    override var isFlipped: Bool { true }

    init(state: PRProState) {
        self.state = state
        super.init(frame: .zero)
        wantsLayer = true
        layer?.cornerRadius = DAWDesignTokens.Radius.control
        setAccessibilityElement(true)
        setAccessibilityRole(.button)
        setAccessibilityLabel("Миниатюра Piano Roll")
        setAccessibilityHelp("Двойной клик открывает полноценный редактор MIDI-нот.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func draw(_ dirtyRect: NSRect) {
        PRProDrawing.canvas.setFill(); bounds.fill()
        guard let map = state.timeMap else {
            PRProDrawing.label("Выберите MIDI-клип", in: bounds.insetBy(dx: 10, dy: 10),
                               color: PRProDrawing.secondary, size: 10)
            return
        }
        let columns = max(1, min(64, Int(bounds.width / 18)))
        for column in 0...columns {
            let x = CGFloat(column) / CGFloat(columns) * bounds.width
            PRProDrawing.line(NSPoint(x: x, y: 0), NSPoint(x: x, y: bounds.height),
                              color: PRProDrawing.border.withAlphaComponent(column % 4 == 0 ? 0.36 : 0.13))
        }
        let notes = state.entities
        let low = Int(notes.map(\.note.pitch).min() ?? 48)
        let high = Int(notes.map(\.note.pitch).max() ?? 72)
        let pitchSpan = max(12, high - low + 4)
        let bottom = max(0, low - 2)
        for entity in notes {
            let note = entity.note
            let start = map.start(note) / map.durationBeats
            let length = map.length(note) / map.durationBeats
            let normalizedPitch = Double(Int(note.pitch) - bottom) / Double(pitchSpan)
            let x = CGFloat(start) * bounds.width
            let width = max(CGFloat(2), CGFloat(length) * bounds.width)
            let y = bounds.height - CGFloat(normalizedPitch) * max(1, bounds.height - 8) - 5
            let rect = NSRect(x: x, y: min(bounds.height - 6, max(2, y)), width: width, height: 5)
            let color = state.selection.contains(entity.id) ? PRProDrawing.mint : PRProDrawing.noteColor(note)
            color.withAlphaComponent(0.9).setFill()
            NSBezierPath(roundedRect: rect, xRadius: 2, yRadius: 2).fill()
        }
        if state.playheadFrame >= map.clipStart, state.playheadFrame <= map.clipStart + map.clipLength {
            let beat = map.beat(at: state.playheadFrame - map.clipStart)
            let x = CGFloat(beat / map.durationBeats) * bounds.width
            PRProDrawing.line(NSPoint(x: x, y: 0), NSPoint(x: x, y: bounds.height),
                              color: PRProDrawing.mint, width: 1.2)
        }
        PRProDrawing.label("\(notes.count) notes · Double-click to open",
                           in: NSRect(x: 8, y: 5, width: max(0, bounds.width - 16), height: 16),
                           color: PRProDrawing.secondary, size: 9)
    }

    override func mouseDown(with event: NSEvent) {
        if event.clickCount >= 2 { onOpen?() }
    }
}

@MainActor
final class PianoRollEditorView: NSView {
    private struct GhostCacheKey: Equatable {
        var documentID: UUID?
        var revision: UInt64
        var trackID: UInt64
        var clipIndex: Int
        var start: UInt64
        var length: UInt64
    }

    var notes: [PianoRollNote] = [] { didSet { syncState() } }
    var clips: [PianoRollClipModel] = [] { didSet { reloadClips(); syncState() } }
    var selectedClip: Int? { didSet { syncClipSelection(); syncState() } }
    var editorEnabled = false { didSet { applyEnabled(); syncState() } }
    /// Optional explicit adapter for isolated tests/embedders. In the real app,
    /// the selected clip is mapped through DraftApp.tempoMap automatically.
    var timeMap: PRTimeMap? { didSet { syncState() } }
    var playheadFrame: UInt64 = 0 {
        didSet {
            state.playheadFrame = playheadFrame
            preview.needsDisplay = true
            windowController?.workspace.updatePlayhead(playheadFrame)
        }
    }

    var onClipSelect: ((Int) -> Void)?
    var onNotesChange: (([PianoRollNote]) -> Void)?
    var onCommitRequest: ((PRCommitRequest) -> Void)?
    private var context: PRClipContext?
    var onAddNote: (() -> Void)?
    var onRemoveNote: ((Int) -> Void)?
    var onAddClip: (() -> Void)?
    var onRemoveClip: ((Int) -> Void)?
    var onSeek: ((UInt64) -> Void)?
    var onUndo: (() -> Void)?
    var onRedo: (() -> Void)?
    var onPlayToggle: (() -> Void)?

    private let state = PRProState()
    private lazy var preview = PRMiniPreviewView(state: state)
    private let clipPopup = NSPopUpButton()
    private let gridPopup = NSPopUpButton()
    private let openButton = NSButton(title: "Open Piano Roll", target: nil, action: nil)
    private let addClipButton = NSButton(title: "+ Clip", target: nil, action: nil)
    private let removeClipButton = NSButton(title: "− Clip", target: nil, action: nil)
    private let addNoteButton = NSButton(title: "+ Note", target: nil, action: nil)
    private let quantizeButton = NSButton(title: "Quantize all", target: nil, action: nil)
    private let legatoButton = NSButton(title: "Legato all", target: nil, action: nil)
    private let statusLabel = NSTextField(wrappingLabelWithString: "")
    private var windowController: PRProWindowController?
    private var syncing = false
    private var ghostCacheKey: GhostCacheKey?
    private var ghostCache = PRGhostLoadResult(notes: [], sourceTracks: 0, limited: false)

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = DAWDesignTokens.Color.raisedSurface.cgColor
        setup()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    private var hostApp: DraftApp? { NSApp.delegate as? DraftApp }

    private func resolvedTimeMap() -> PRTimeMap? {
        if let timeMap { return timeMap }
        guard let selectedClip,
              let clip = clips.first(where: { $0.index == selectedClip }),
              clip.lengthFrames > 0,
              let hostApp else { return nil }
        let projectMap = hostApp.tempoMap
        return try? PRTimeMap(clipStart: clip.startFrames,
                              clipLength: clip.lengthFrames,
                              toBeat: { projectMap.beats(atFrame: $0) },
                              toFrame: { projectMap.frame(atBeats: $0) })
    }

    private func setup() {
        clipPopup.target = self; clipPopup.action = #selector(selectClip)
        clipPopup.font = DAWDesignTokens.Typography.caption
        gridPopup.addItems(withTitles: PRGridDivision.allCases.map(\.title))
        gridPopup.selectItem(at: PRGridDivision.sixteenth.rawValue)
        gridPopup.target = self; gridPopup.action = #selector(changeGrid)
        for button in [openButton, addClipButton, removeClipButton, addNoteButton, quantizeButton, legatoButton] {
            button.bezelStyle = .texturedRounded
            button.font = DAWDesignTokens.Typography.caption
        }
        openButton.target = self; openButton.action = #selector(openPianoRoll)
        addClipButton.target = self; addClipButton.action = #selector(addClipNow)
        removeClipButton.target = self; removeClipButton.action = #selector(removeClipNow)
        addNoteButton.target = self; addNoteButton.action = #selector(addNoteNow)
        quantizeButton.target = self; quantizeButton.action = #selector(quantizeAll)
        legatoButton.target = self; legatoButton.action = #selector(legatoAll)
        openButton.contentTintColor = DAWDesignTokens.Color.accent
        preview.onOpen = { [weak self] in self?.openPianoRoll() }
        preview.translatesAutoresizingMaskIntoConstraints = false
        preview.heightAnchor.constraint(equalToConstant: 150).isActive = true
        statusLabel.font = DAWDesignTokens.Typography.caption
        statusLabel.textColor = DAWDesignTokens.Color.secondaryText
        statusLabel.maximumNumberOfLines = 2

        let clipRow = NSStackView(views: [clipPopup, addClipButton, removeClipButton])
        clipRow.spacing = 5; clipRow.alignment = .centerY
        let quickRow = NSStackView(views: [openButton, addNoteButton, quantizeButton, legatoButton])
        quickRow.spacing = 5; quickRow.alignment = .centerY
        let gridRow = NSStackView(views: [NSTextField(labelWithString: "Grid"), gridPopup])
        gridRow.spacing = 5; gridRow.alignment = .centerY
        let stack = NSStackView(views: [clipRow, gridRow, quickRow, preview, statusLabel])
        stack.orientation = .vertical
        stack.alignment = .width
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 6),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -6),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 6),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6)
        ])

        state.onCommitRequest = { [weak self] request in self?.onCommitRequest?(request) }
        state.onCommit = { [weak self] committed in self?.onNotesChange?(committed) }
        state.onChange = { [weak self] in self?.refreshFromState() }
        setAccessibilityLabel("MIDI Piano Roll")
        openButton.setAccessibilityLabel("Открыть полноразмерный Piano Roll")
        preview.setAccessibilityLabel("Предпросмотр выбранного MIDI-клипа")
        applyEnabled()
        refreshFromState()
    }

    /// The host must replace binding, metadata and notes in ONE receive. Property
    /// didSet callbacks alone would acknowledge a pending commit with old notes.
    func apply(model: InspectorMidiModel?) {
        let previousContext = context
        syncing = true
        context = model?.context
        notes = model?.notes ?? []
        clips = model?.clips ?? []
        selectedClip = model?.selectedClip
        editorEnabled = model?.editable ?? false
        syncing = false
        syncState()
        if let next = context, previousContext?.sameClip(as: next) != true,
           let controller = windowController, controller.window?.isVisible == true {
            controller.workspace.layoutSubtreeIfNeeded()
            controller.workspace.fit(selectionOnly: false)
            controller.workspace.scrollInspectorToTop()
        }
    }

    private func syncState() {
        guard !syncing else { return }
        syncing = true
        state.onCommitRequest = context == nil ? nil : { [weak self] request in
            self?.onCommitRequest?(request)
        }
        let currentPlayhead = hostApp?.playheadFrame ?? playheadFrame
        state.playheadFrame = currentPlayhead
        state.receive(notes: notes, map: resolvedTimeMap(),
                      editable: editorEnabled && selectedClip != nil && !clips.isEmpty, context: context)
        syncing = false
        applyEnabled()
        if windowController != nil { refreshGhostsIfNeeded() }
    }

    private func refreshFromState() {
        applyEnabled()
        statusLabel.stringValue = state.status
        preview.needsDisplay = true
        windowController?.workspace.refreshFromState()
    }

    private func reloadClips() {
        clipPopup.removeAllItems()
        clips.forEach { clipPopup.addItem(withTitle: $0.title) }
        syncClipSelection()
        applyEnabled()
    }

    private func syncClipSelection() {
        guard let selectedClip,
              let position = clips.firstIndex(where: { $0.index == selectedClip }) else {
            if !clips.isEmpty { clipPopup.selectItem(at: 0) }
            return
        }
        clipPopup.selectItem(at: position)
    }

    private func applyEnabled() {
        let hasClip = selectedClip != nil && !clips.isEmpty
        let canEdit = editorEnabled && hasClip && !state.awaitingCommit && !state.isGesturing
        clipPopup.isEnabled = canEdit
        addClipButton.isEnabled = editorEnabled && !state.awaitingCommit && !state.isGesturing
        removeClipButton.isEnabled = canEdit
        openButton.isEnabled = canEdit && resolvedTimeMap() != nil
        addNoteButton.isEnabled = canEdit
        quantizeButton.isEnabled = canEdit
        legatoButton.isEnabled = canEdit
        gridPopup.isEnabled = canEdit
    }

    private func refreshGhostsIfNeeded(force: Bool = false) {
        guard let workspace = windowController?.workspace,
              let app = hostApp,
              let trackID = app.inspectorTrackID,
              let selectedClip,
              let clip = clips.first(where: { $0.index == selectedClip }) else {
            ghostCacheKey = nil
            ghostCache = PRGhostLoadResult(notes: [], sourceTracks: 0, limited: false)
            windowController?.workspace.setGhostNotes(ghostCache)
            return
        }
        let key = GhostCacheKey(documentID: context?.documentID, revision: context?.revision ?? app.revision, trackID: trackID,
                                clipIndex: selectedClip, start: clip.startFrames,
                                length: clip.lengthFrames)
        if !force, key == ghostCacheKey {
            workspace.setGhostNotes(ghostCache)
            return
        }
        ghostCache = PRGhostLoader.load(app: app, activeTrackID: trackID, activeClip: clip)
        ghostCacheKey = key
        workspace.setGhostNotes(ghostCache)
    }

    @objc private func selectClip() {
        let position = clipPopup.indexOfSelectedItem
        guard clips.indices.contains(position) else { return }
        onClipSelect?(clips[position].index)
    }

    @objc private func addClipNow() { onAddClip?() }
    @objc private func removeClipNow() { if let selectedClip { onRemoveClip?(selectedClip) } }
    @objc private func addNoteNow() { onAddNote?() }

    @objc private func changeGrid() {
        state.grid.division = PRGridDivision(rawValue: gridPopup.indexOfSelectedItem) ?? .sixteenth
        state.changed()
    }

    @objc private func quantizeAll() {
        state.selectAll(); state.quantize()
    }

    @objc private func legatoAll() {
        state.selectAll(); state.legato()
    }

    @objc private func openPianoRoll() {
        guard editorEnabled, selectedClip != nil, resolvedTimeMap() != nil else {
            state.fail(PREditError.unavailable)
            return
        }
        syncState()
        let controller: PRProWindowController
        if let existing = windowController {
            controller = existing
        } else {
            let created = PRProWindowController(state: state)
            created.onClose = { [weak self] in self?.preview.needsDisplay = true }
            windowController = created
            controller = created
        }
        controller.workspace.onSeek = { [weak self] frame in
            guard let self else { return }
            if let onSeek = self.onSeek { onSeek(frame) }
            else { self.hostApp?.seekAudio(frame) }
        }
        controller.workspace.onUndo = { [weak self] in
            guard let self else { return }
            if let onUndo = self.onUndo { onUndo() }
            else { self.hostApp?.undo() }
        }
        controller.workspace.onRedo = { [weak self] in
            guard let self else { return }
            if let onRedo = self.onRedo { onRedo() }
            else { self.hostApp?.redo() }
        }
        controller.workspace.onPlayToggle = { [weak self] in
            guard let self else { return }
            if let onPlayToggle = self.onPlayToggle { onPlayToggle() }
            else { self.hostApp?.togglePlayStop() }
        }
        refreshGhostsIfNeeded(force: true)
        let title = clips.first(where: { $0.index == selectedClip })?.title
        controller.present(relativeTo: window, title: title)
    }

    static func noteName(_ pitch: UInt8) -> String { PRPitch.name(pitch) }
    static func parsePitch(_ raw: String) -> UInt8? { PRPitch.parse(raw) }
}
