import AppKit

@MainActor
final class RecordingRowsView: NSStackView {
    override var isFlipped: Bool { true }
}

@MainActor
final class RecordingWorkspaceView: NSView {
    let tracks = NSPopUpButton(), takes = NSPopUpButton(), preroll = NSPopUpButton()
    let record = NSButton(title: "● Запись", target: nil, action: nil)
    let arm = NSButton(title: "Дубли в эту дорожку", target: nil, action: nil)
    let monitor = NSButton(title: "Слышать сухой вход", target: nil, action: nil)
    let loop = NSButton(title: "Циклическая запись", target: nil, action: nil)
    let settings = NSButton(title: "Устройство и задержка…", target: nil, action: nil)
    let effects = NSButton(title: "Эффекты канала →", target: nil, action: nil)
    let importTake = NSButton(title: "Импортировать дубль…", target: nil, action: nil)
    let apply = NSButton(title: "В comp", target: nil, action: nil)
    let fit = NSButton(title: "Весь трек", target: nil, action: nil)
    let zoom = NSButton(title: "Выделение", target: nil, action: nil)
    let start = NSTextField(string: "0.000"), end = NSTextField(string: "0.000")
    let recordTarget = NSTextField(wrappingLabelWithString: "")
    let heading = NSTextField(labelWithString: "ЗАПИСЬ / COMPING")
    let hint = NSTextField(wrappingLabelWithString: "")
    let clock = NSTextField(labelWithString: ""), rangeLabel = NSTextField(labelWithString: "")
    let scroll = NSScrollView(), rows = RecordingRowsView(), sidebar = NSStackView()
    private(set) var lanes: [RecordTakeLaneView] = []
    private(set) var documentID = UUID()
    private(set) var trackID: UInt64?
    private(set) var revision: UInt64 = 0
    private(set) var snapshots: [RecordTakeSnapshot] = []
    var trackIDs: [UInt64] = []
    var onAction: ((NSControl) -> Void)?
    var onTake: ((UInt32) -> Void)?, onCommit: ((RecordCompRequest) -> Void)?
    var onTogglePlayback: (() -> Void)?
    var isGesturing: Bool { lanes.contains { $0.isGesturing } }
    var editable = false {
        didSet { for lane in lanes { lane.editable = editable; lane.window?.invalidateCursorRects(for: lane) } }
    }
    private var allEnd: UInt64 = 1
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.canvas.cgColor
        heading.font = .systemFont(ofSize: 18, weight: .semibold)
        hint.font = .systemFont(ofSize: 11); hint.textColor = .secondaryLabelColor
        clock.font = .monospacedDigitSystemFont(ofSize: 11, weight: .regular)
        rangeLabel.font = .monospacedDigitSystemFont(ofSize: 11, weight: .medium)
        for button in [record, arm, monitor, loop, settings, effects, importTake, apply, fit, zoom] {
            button.target = self; button.action = #selector(action(_:)); button.bezelStyle = .rounded
        }
        for button in [arm, monitor, loop] { button.setButtonType(.toggle) }
        for popup in [tracks, takes, preroll] { popup.target = self; popup.action = #selector(action(_:)) }
        for seconds in [0, 1, 2, 4, 8] { preroll.addItem(withTitle: seconds == 0 ? "Без преролла" : "Преролл: \(seconds) с"); preroll.lastItem?.tag = seconds }
        tracks.setAccessibilityLabel("Дорожка для comping")
        takes.setAccessibilityLabel("Дубль для точной сборки")
        start.setAccessibilityLabel("Начало фразы в секундах"); end.setAccessibilityLabel("Конец фразы в секундах")
        preroll.setAccessibilityLabel("Преролл записи")
        record.contentTintColor = DAWDesignTokens.Color.coral
        recordTarget.font = .systemFont(ofSize: 11, weight: .medium)
        recordTarget.textColor = DAWDesignTokens.Color.secondaryText
        monitor.toolTip = "Сухой вход слышен только во время записи и преролла. Используйте наушники."
        sidebar.orientation = .vertical; sidebar.alignment = .leading; sidebar.spacing = 7
        rows.orientation = .vertical; rows.alignment = .leading; rows.spacing = 5
        scroll.hasVerticalScroller = true; scroll.drawsBackground = false; scroll.documentView = rows
        let caption = NSTextField(labelWithString: "Фраза: начало / конец, секунды")
        caption.font = .systemFont(ofSize: 11)
        for view in [tracks, recordTarget, record, arm, monitor, loop, preroll, settings, effects, importTake,
                     takes, caption, start, end, apply, hint] {
            sidebar.addArrangedSubview(view)
            view.widthAnchor.constraint(equalToConstant: 220).isActive = true
        }
        sidebar.spacing = 5
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        rows.translatesAutoresizingMaskIntoConstraints = false
        [sidebar, heading, scroll, rangeLabel, clock, fit, zoom].forEach(addSubview)
        NSLayoutConstraint.activate([
            sidebar.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            sidebar.topAnchor.constraint(equalTo: topAnchor, constant: 16),
            rows.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
            rows.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
            rows.topAnchor.constraint(equalTo: scroll.contentView.topAnchor)
        ])
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func layout() {
        super.layout()
        heading.frame = NSRect(x: 250, y: 14, width: max(0, bounds.width - 510), height: 30)
        fit.frame = NSRect(x: max(250, bounds.width - 230), y: 15, width: 100, height: 28)
        zoom.frame = NSRect(x: max(350, bounds.width - 125), y: 15, width: 110, height: 28)
        rangeLabel.frame = NSRect(x: 250, y: 51, width: max(0, bounds.width - 268), height: 20)
        scroll.frame = NSRect(x: 248, y: 80, width: max(0, bounds.width - 260), height: max(0, bounds.height - 116))
        clock.frame = NSRect(x: 250, y: max(80, bounds.height - 28), width: max(0, bounds.width - 268), height: 20)
    }
    func requestFromFields() -> RecordCompRequest? {
        guard let trackID, let take = snapshots.first(where: { Int($0.index) == takes.selectedItem?.tag }),
              let a = RecordCompSwipe.frame(seconds: start.stringValue), let b = RecordCompSwipe.frame(seconds: end.stringValue),
              b > a, b <= take.start + take.frames,
              let swipe = RecordCompSwipe(documentID: documentID, trackID: trackID, takeIndex: take.index, revision: revision,
                  takeStart: take.start, takeFrames: take.frames, anchor: a) else { return nil }
        return swipe.request(at: b)
    }
    func selectTake(_ index: UInt32) {
        takes.selectItem(withTag: Int(index))
        for lane in lanes { lane.selected = lane.take?.index == index; lane.needsDisplay = true }
        onTake?(index)
    }
    func displayRange(_ start: UInt64, _ end: UInt64) {
        guard end > start else { return }
        for lane in lanes { lane.viewStart = start; lane.viewEnd = end; lane.needsDisplay = true }
        rangeLabel.stringValue = String(format: "%.3f — %.3f с · протяни фразу по дублю · Escape — отмена", Double(start)/48000, Double(end)/48000)
    }
    @objc private func action(_ sender: NSControl) {
        if sender === fit { displayRange(0, allEnd) }
        else if sender === zoom {
            if let request = requestFromFields() { displayRange(request.range.start, request.range.end) }
            else { hint.stringValue = "Задай диапазон внутри выбранного дубля." }
        } else if sender === takes, let item = takes.selectedItem { selectTake(UInt32(item.tag)) }
        else { onAction?(sender) }
    }
    func rebuild(trackID: UInt64?, documentID: UUID, revision: UInt64, takes newTakes: [RecordTakeSnapshot],
                 clips: [ClipGeometry], selected: UInt32) {
        guard !isGesturing else { return }
        let viewport = scroll.contentView.bounds.origin
        let changedTrack = self.trackID != trackID || self.documentID != documentID
        self.documentID = documentID
        let oldRange = lanes.first.map { ($0.viewStart, $0.viewEnd) }
        self.trackID = trackID; self.revision = revision; snapshots = newTakes
        for row in rows.arrangedSubviews { rows.removeArrangedSubview(row); row.removeFromSuperview() }
        lanes.removeAll(); takes.removeAllItems()
        for take in newTakes { takes.addItem(withTitle: take.title); takes.lastItem?.tag = Int(take.index) }
        allEnd = min(RecordCompSwipe.maximumFrame, max(1,
            max(newTakes.map { $0.start + $0.frames }.max() ?? 0, clips.map { $0.start + $0.length }.max() ?? 0)))
        let comp = RecordTakeLaneView(frame: .zero); comp.title = "COMP"; comp.clips = clips
        lanes.append(comp)
        for take in newTakes {
            let lane = RecordTakeLaneView(frame: .zero)
            lane.title = take.title; lane.take = take; lane.clips = clips
            lane.setAccessibilityLabel("Дубль: \(take.title)")
            lane.onSelect = { [weak self] in self?.selectTake($0) }
            lane.onCommit = { [weak self] in self?.onCommit?($0) }
            lane.onTogglePlayback = { [weak self] in self?.onTogglePlayback?() }
            lane.onPreview = { [weak self] range in
                guard let self else { return }
                guard let range else {
                    if let first = lanes.first { displayRange(first.viewStart, first.viewEnd) }
                    return
                }
                rangeLabel.stringValue = String(format: "Фраза %.3f — %.3f с · отпусти для сборки · Escape — отмена", Double(range.start)/48000, Double(range.end)/48000)
            }
            lanes.append(lane)
        }
        for lane in lanes {
            lane.documentID = documentID
            lane.trackID = trackID ?? 0; lane.revision = revision; lane.editable = editable
            rows.addArrangedSubview(lane)
            lane.widthAnchor.constraint(equalTo: rows.widthAnchor).isActive = true
            lane.heightAnchor.constraint(equalToConstant: lane.take == nil ? 118 : 80).isActive = true
        }
        selectTake(selected)
        if changedTrack, let take = snapshots.first(where: { $0.index == selected }) {
            start.stringValue = String(format: "%.6f", Double(take.start)/48000)
            end.stringValue = String(format: "%.6f", Double(take.start + take.frames)/48000)
        }
        if !changedTrack, let oldRange, oldRange.0 < allEnd {
            displayRange(oldRange.0, min(allEnd, oldRange.1))
        } else { displayRange(0, allEnd) }
        window?.contentView?.layoutSubtreeIfNeeded()
        let clip = scroll.contentView
        let requested = NSRect(origin: changedTrack ? .zero : viewport, size: clip.bounds.size)
        clip.scroll(to: clip.constrainBoundsRect(requested).origin)
        scroll.reflectScrolledClipView(clip)
        hint.stringValue = newTakes.isEmpty ? "Запиши или импортируй аудио. В пустом проекте Запись создаст новую дорожку." : "Зелёные отметки показывают исходные фрагменты, использованные в comp. Файл записи всегда сухой."
    }
}
