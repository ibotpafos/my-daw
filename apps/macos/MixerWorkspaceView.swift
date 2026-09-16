import AppKit

enum MixerStripKind: Sendable { case track, bus, master }

struct MixerMeterSnapshot: Sendable, Equatable {
    var leftPeak: Float = 0
    var rightPeak: Float = 0
    var leftHold: Float = 0
    var rightHold: Float = 0
    /// BS.1770 momentary/short-term LUFS, только для мастера; nil — нет живого рендерера.
    var momentaryLufs: Float?
    var shortTermLufs: Float?
}

struct MixerInsertSummary: Sendable, Equatable {
    var name: String
    var bypassed: Bool = false
}

struct MixerSendSummary: Sendable, Equatable {
    var destination: String
    var gainDb: Double = -12
    var preFader: Bool = false
}

struct MixerStripModel: Identifiable, Sendable, Equatable {
    var id: UInt64
    var kind: MixerStripKind
    var title: String
    var channelNumber: Int = 0
    var color: NSColor? = nil
    var volumeDb: Double = 0
    var pan: Double = 0
    var meter = MixerMeterSnapshot()
    var outputName: String = "Main"
    var inserts: [MixerInsertSummary] = []
    var sends: [MixerSendSummary] = []
    var isSelected = false
    var isArmed = false
    var isMuted = false
    var isSolo = false
    var isAutomationRead = true
}

@MainActor
final class MixerMeterView: NSView {
    var snapshot = MixerMeterSnapshot() { didSet { needsDisplay = true } }
    override var isFlipped: Bool { true }
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setAccessibilityElement(true)
        setAccessibilityRole(.levelIndicator)
        setAccessibilityHelp("Пиковый стереоуровень канала")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func accessibilityValue() -> Any? {
        let peak = max(snapshot.leftPeak, snapshot.rightPeak)
        guard peak > 0 else { return "−∞ dBFS" }
        return String(format: "%.1f dBFS", 20 * log10(Double(peak)))
    }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.canvas.withAlphaComponent(0.86).setFill(); bounds.fill()
        let channels = [snapshot.leftPeak, snapshot.rightPeak]
        let holds = [snapshot.leftHold, snapshot.rightHold]
        for index in 0..<2 {
            let lane = NSRect(x: CGFloat(index) * (bounds.width + 1) / 2, y: 0, width: max(1, (bounds.width - 1) / 2), height: bounds.height)
            let level = CGFloat(min(1, max(0, channels[index])))
            let filled = NSRect(x: lane.minX, y: lane.maxY - lane.height * level, width: lane.width, height: lane.height * level)
            let color = DAWDataVisuals.meterColor(for: level)
            color.withAlphaComponent(0.9).setFill(); filled.fill()
            let hold = lane.maxY - lane.height * CGFloat(min(1, max(0, holds[index])))
            color.setFill(); NSRect(x: lane.minX, y: hold, width: lane.width, height: 1).fill()
        }
    }
}

@MainActor
final class MixerFaderView: NSView {
    var valueDb: Double = 0 { didSet { needsDisplay = true } }
    var onBegin: (() -> Void)?
    var onChange: ((Double) -> Void)?
    var onEnd: ((Double) -> Void)?
    private var dragging = false
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setAccessibilityElement(true)
        setAccessibilityRole(.slider)
        setAccessibilityHelp("Стрелки вверх и вниз изменяют уровень на 0,5 dB")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    override func accessibilityValue() -> Any? { String(format: "%+.1f dB", valueDb) }
    override func accessibilityPerformIncrement() -> Bool { nudge(0.5); return true }
    override func accessibilityPerformDecrement() -> Bool { nudge(-0.5); return true }
    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 125: nudge(-0.5)
        case 126: nudge(0.5)
        default: super.keyDown(with: event)
        }
    }
    private func nudge(_ delta: Double) {
        let next = min(24, max(-120, valueDb + delta))
        guard next != valueDb else { return }
        onBegin?(); valueDb = next; onChange?(next); onEnd?(next)
        NSAccessibility.post(element: self, notification: .valueChanged)
    }
    private func value(at point: NSPoint) -> Double {
        let fraction = min(1, max(0, 1 - Double(point.y / max(1, bounds.height))))
        return -120 + fraction * 144
    }
    override func mouseDown(with event: NSEvent) { window?.makeFirstResponder(self); dragging = true; onBegin?(); let v=value(at:convert(event.locationInWindow,from:nil)); valueDb=v; onChange?(v) }
    override func mouseDragged(with event: NSEvent) { guard dragging else{return}; let v=value(at:convert(event.locationInWindow,from:nil)); valueDb=v; onChange?(v) }
    override func mouseUp(with event: NSEvent) { guard dragging else{return}; dragging=false; onEnd?(valueDb) }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.canvas.withAlphaComponent(0.85).setFill(); NSBezierPath(roundedRect: bounds, xRadius: DAWDesignTokens.Radius.control, yRadius: DAWDesignTokens.Radius.control).fill()
        let fraction=CGFloat((min(24,max(-120,valueDb))+120)/144); let y=bounds.height*(1-fraction)
        DAWDesignTokens.Color.accent.withAlphaComponent(0.45).setFill(); NSRect(x: bounds.midX - 1, y: y, width: 2, height: bounds.maxY-y).fill()
        DAWDesignTokens.Color.text.withAlphaComponent(0.9).setFill(); NSBezierPath(roundedRect:NSRect(x:2,y:y-4,width:bounds.width-4,height:8),xRadius:3,yRadius:3).fill()
    }
}

@MainActor
private final class MixerCanvasView: NSView {
    override var isFlipped: Bool { true }
}

@MainActor
private final class MixerStripView: NSView {
    var model: MixerStripModel { didSet { refresh() } }
    var onSelect: ((UInt64) -> Void)?
    var onArm: ((UInt64, Bool) -> Void)?
    var onMute: ((UInt64, Bool) -> Void)?
    var onSolo: ((UInt64, Bool) -> Void)?
    var onVolumeBegin: ((UInt64) -> Void)?
    var onVolume: ((UInt64, Double) -> Void)?
    var onVolumeEnd: ((UInt64, Double) -> Void)?
    var onPan: ((UInt64, Double) -> Void)?
    var onDeleteBus: ((UInt64) -> Void)?
    private let title = NSTextField(labelWithString: "")
    private let insertHeading = NSTextField(labelWithString: "INSERTS")
    private let sendHeading = NSTextField(labelWithString: "SENDS")
    private let routing = NSTextField(labelWithString: "")
    private let loudness = NSTextField(labelWithString: "")
    private let automation = NSTextField(labelWithString: "")
    private let value = NSTextField(labelWithString: "")
    private let footer = NSTextField(labelWithString: "")
    private let meter=MixerMeterView()
    private let fader=MixerFaderView()
    private let pan=NSSlider(value:0,minValue:-1,maxValue:1,target:nil,action:nil)
    private let arm=NSButton(title:"R",target:nil,action:nil), mute=NSButton(title:"M",target:nil,action:nil), solo=NSButton(title:"S",target:nil,action:nil)
    private let deleteBusButton=NSButton(title:"✕",target:nil,action:nil)
    private let inserts=NSTextField(wrappingLabelWithString:""), sends=NSTextField(wrappingLabelWithString:"")
    init(model: MixerStripModel) { self.model=model; super.init(frame:.zero); setup(); refresh() }
    required init?(coder:NSCoder) { fatalError("init(coder:) is unavailable") }
    private func setup() {
        wantsLayer = true
        for button in [arm, mute, solo] {
            button.bezelStyle = .texturedRounded
            button.setButtonType(.toggle)
            button.font = .monospacedSystemFont(ofSize: 9, weight: .semibold)
            addSubview(button)
        }
        arm.target = self; arm.action = #selector(toggleArm)
        mute.target = self; mute.action = #selector(toggleMute)
        solo.target = self; solo.action = #selector(toggleSolo)
        pan.target = self; pan.action = #selector(changePan)
        deleteBusButton.bezelStyle = .texturedRounded
        deleteBusButton.font = .monospacedSystemFont(ofSize: 9, weight: .semibold)
        deleteBusButton.target = self; deleteBusButton.action = #selector(deleteBus)
        deleteBusButton.isHidden = true  // shown only for bus kind

        title.alignment = .center
        title.font = .systemFont(ofSize: 10, weight: .semibold)
        title.lineBreakMode = .byTruncatingTail
        footer.alignment = .center
        footer.font = .monospacedSystemFont(ofSize: 9, weight: .bold)
        footer.textColor = .white
        footer.lineBreakMode = .byTruncatingTail
        routing.alignment = .center
        routing.font = .monospacedSystemFont(ofSize: 8, weight: .medium)
        routing.lineBreakMode = .byTruncatingTail
        loudness.alignment = .center
        loudness.font = .monospacedSystemFont(ofSize: 8, weight: .semibold)
        loudness.textColor = DAWDesignTokens.Color.mint
        loudness.lineBreakMode = .byTruncatingTail
        loudness.isHidden = true
        automation.alignment = .center
        automation.font = .monospacedSystemFont(ofSize: 8, weight: .medium)
        value.alignment = .center
        value.font = .monospacedDigitSystemFont(ofSize: 9, weight: .medium)
        for heading in [insertHeading, sendHeading] {
            heading.font = .monospacedSystemFont(ofSize: 8, weight: .semibold)
            heading.textColor = .tertiaryLabelColor
        }
        for detail in [inserts, sends] {
            detail.font = .systemFont(ofSize: 8)
            detail.textColor = .secondaryLabelColor
            detail.lineBreakMode = .byTruncatingTail
        }
        [title, insertHeading, inserts, sendHeading, sends, routing, loudness, automation, value, meter, fader, pan, footer, deleteBusButton].forEach(addSubview)
        fader.onBegin={ [weak self] in self.map { $0.onVolumeBegin?($0.model.id) } };fader.onChange={ [weak self] value in self.map { $0.onVolume?($0.model.id,value) } };fader.onEnd={ [weak self] value in self.map { $0.onVolumeEnd?($0.model.id,value) } }
    }
    private func refresh() {
        title.stringValue = model.kind == .bus ? "BUS · \(model.title)" : (model.kind == .master ? "MASTER" : model.title)
        let footerPrefix = model.kind == .master ? "M" : "\(model.channelNumber)"
        footer.stringValue = "\(footerPrefix)  \(model.title.uppercased())"
        meter.snapshot = model.meter
        fader.valueDb = model.volumeDb
        pan.doubleValue = model.pan
        arm.state = model.isArmed ? .on : .off
        mute.state = model.isMuted ? .on : .off
        solo.state = model.isSolo ? .on : .off
        routing.stringValue = model.kind == .master ? "MAIN  \(model.outputName)" : "OUT  \(model.outputName)"
        if model.kind == .master, let momentary = model.meter.momentaryLufs {
            loudness.isHidden = false
            loudness.stringValue = momentary < -99 ? "LUFS · тишина" : String(format: "M %.1f · S %.1f LUFS", momentary, model.meter.shortTermLufs ?? momentary)
            loudness.setAccessibilityLabel("Громкость мастера: \(loudness.stringValue)")
        } else { loudness.isHidden = true; loudness.stringValue = "" }
        automation.stringValue = model.isAutomationRead ? "AUTO: READ" : "AUTO: OFF"
        value.stringValue = String(format: "%+.1f dB", model.volumeDb)
        inserts.stringValue = model.inserts.isEmpty ? "—" : model.inserts.prefix(4).map { $0.bypassed ? "⊘ \($0.name)" : "◉ \($0.name)" }.joined(separator: "\n")
        sends.stringValue = model.sends.prefix(3).map {
            "→ \($0.destination) \(String(format: "%+.0f", $0.gainDb)) \($0.preFader ? "PRE" : "POST")"
        }.joined(separator: "\n")
        if sends.stringValue.isEmpty { sends.stringValue = "—" }
        meter.setAccessibilityLabel("Пиковый уровень \(model.title)")
        fader.setAccessibilityLabel("Громкость \(model.title)")
        pan.setAccessibilityLabel("Панорама \(model.title)")
        arm.setAccessibilityLabel("Запись \(model.title)")
        mute.setAccessibilityLabel("Mute \(model.title)")
        solo.setAccessibilityLabel("Solo \(model.title)")
        inserts.setAccessibilityLabel("Inserts \(model.title): \(inserts.stringValue)")
        sends.setAccessibilityLabel("Sends \(model.title): \(sends.stringValue)")
        routing.setAccessibilityLabel("Выход \(model.title): \(routing.stringValue)")
        value.setAccessibilityLabel("Текущий уровень \(model.title): \(value.stringValue)")
        automation.setAccessibilityLabel("Автоматизация \(model.title): \(automation.stringValue)")
        let channelName = model.kind == .master ? "Master" : "Канал \(model.channelNumber) \(model.title)"
        setAccessibilityLabel("\(channelName) микшера")
        title.toolTip = model.title
        footer.toolTip = model.title
        title.setAccessibilityLabel(channelName)
        footer.setAccessibilityLabel(channelName)
        let canArm = model.kind == .track
        arm.isHidden = !canArm
        arm.isEnabled = canArm
        let canMute = model.kind != .master
        mute.isHidden = !canMute
        mute.isEnabled = canMute
        let canSolo = model.kind == .track
        solo.isHidden = !canSolo
        solo.isEnabled = canSolo
        let canPan = model.kind != .master
        pan.isHidden = !canPan
        pan.isEnabled = canPan
        deleteBusButton.isHidden = model.kind != .bus
        needsDisplay = true
    }
    override var isFlipped: Bool { true }
    override func layout() {
        let width = bounds.width
        let height = bounds.height
        let padding: CGFloat = 5
        let footerHeight: CGFloat = 20
        let titleHeight: CGFloat = 15
        let showDetails = height >= 286
        let detailTop = padding + titleHeight + 2
        let detailHeight = showDetails ? min(176, max(104, height * 0.34)) : 0
        let channelControlsY = showDetails ? detailTop + detailHeight + 5 : detailTop + 2
        let meterTop = channelControlsY + 74
        let meterBottom = max(meterTop + 30, height - footerHeight - 22)

        insertHeading.isHidden = !showDetails
        inserts.isHidden = !showDetails
        sendHeading.isHidden = !showDetails
        sends.isHidden = !showDetails
        title.isHidden = false
        title.frame = NSRect(x: padding, y: padding, width: width - 2 * padding, height: titleHeight)

        if showDetails {
            insertHeading.frame = NSRect(x: padding, y: detailTop, width: width - 2 * padding, height: 13)
            inserts.frame = NSRect(x: padding, y: detailTop + 14, width: width - 2 * padding, height: max(24, detailHeight * 0.52 - 15))
            let sendY = detailTop + max(48, detailHeight * 0.52)
            sendHeading.frame = NSRect(x: padding, y: sendY, width: width - 2 * padding, height: 13)
            sends.frame = NSRect(x: padding, y: sendY + 14, width: width - 2 * padding, height: max(24, detailTop + detailHeight - sendY - 15))
        }

        routing.frame = NSRect(x: padding, y: channelControlsY, width: width - 2 * padding, height: 15)
        loudness.frame = NSRect(x: padding, y: max(channelControlsY + 20, height - 34), width: width - 2 * padding, height: 11)
        if !pan.isHidden {
            pan.frame = NSRect(x: padding, y: channelControlsY + 17, width: width - 2 * padding, height: 17)
        }
        let activeButtons: [NSButton] = [arm, mute, solo].filter { !$0.isHidden }
        let buttonWidth = max(20, (width - padding * CGFloat(activeButtons.count + 1)) / CGFloat(max(1, activeButtons.count)))
        for (index, button) in activeButtons.enumerated() {
            button.frame = NSRect(x: padding + CGFloat(index) * (buttonWidth + padding), y: channelControlsY + 37, width: buttonWidth, height: 19)
        }
        // Delete button for bus strips - positioned at top-right
        if !deleteBusButton.isHidden {
            deleteBusButton.frame = NSRect(x: width - padding - 20, y: padding, width: 20, height: 16)
        }
        value.frame = NSRect(x: padding, y: channelControlsY + 58, width: width - 2 * padding, height: 13)
        let meterHeight = max(26, meterBottom - meterTop)
        meter.frame = NSRect(x: padding + 9, y: meterTop, width: 12, height: meterHeight)
        fader.frame = NSRect(x: padding + 28, y: meterTop, width: max(22, width - 42), height: meterHeight)
        automation.frame = NSRect(x: padding, y: height - footerHeight - 19, width: width - 2 * padding, height: 14)
        footer.frame = NSRect(x: 2, y: height - footerHeight, width: width - 4, height: footerHeight - 2)
    }
    override func mouseDown(with event:NSEvent) { onSelect?(model.id) }
    override func draw(_ dirtyRect:NSRect) {
        let color = model.color ?? (model.kind == .master ? DAWDesignTokens.Color.warning : (model.kind == .bus ? DAWDesignTokens.Color.accent : DAWDesignTokens.Color.mint))
        let card = bounds.insetBy(dx: 1, dy: 1)
        (model.isSelected ? color.withAlphaComponent(0.18) : DAWDesignTokens.Color.surface).setFill()
        NSBezierPath(roundedRect: card, xRadius: DAWDesignTokens.Radius.card, yRadius: DAWDesignTokens.Radius.card).fill()
        color.withAlphaComponent(model.isSelected ? 0.85 : 0.28).setStroke()
        NSBezierPath(roundedRect: card, xRadius: DAWDesignTokens.Radius.card, yRadius: DAWDesignTokens.Radius.card).stroke()
        color.withAlphaComponent(0.92).setFill()
        NSRect(x: 2, y: max(2, bounds.height - 20), width: max(0, bounds.width - 4), height: 17).fill()
        if model.kind == .bus {
            color.withAlphaComponent(0.72).setFill()
            NSRect(x: 2, y: 2, width: max(0, bounds.width - 4), height: 2).fill()
        }
        if model.kind == .master {
            color.withAlphaComponent(0.95).setFill()
            NSRect(x: 2, y: 2, width: 3, height: max(0, bounds.height - 24)).fill()
        }
        DAWDesignTokens.Color.canvas.withAlphaComponent(0.55).setStroke()
        NSBezierPath(rect: NSRect(x: 4, y: 0, width: max(0, bounds.width - 8), height: 0.5)).stroke()
    }
    @objc private func toggleArm(){onArm?(model.id,arm.state == .on)}
    @objc private func toggleMute(){onMute?(model.id,mute.state == .on)}
    @objc private func toggleSolo(){onSolo?(model.id,solo.state == .on)}
    @objc private func changePan(){onPan?(model.id,pan.doubleValue)}
    @objc private func deleteBus(){onDeleteBus?(model.id)}
}

@MainActor
final class MixerWorkspaceView: NSScrollView {
    var strips: [MixerStripModel] = [] { didSet { rebuild() } }
    var onSelect: ((UInt64) -> Void)?
    var onArm: ((UInt64, Bool) -> Void)?
    var onMute: ((UInt64, Bool) -> Void)?
    var onSolo: ((UInt64, Bool) -> Void)?
    var onVolumeGestureBegin: ((UInt64) -> Void)?
    var onVolume: ((UInt64, Double) -> Void)?
    var onVolumeGestureEnd: ((UInt64, Double) -> Void)?
    var onPan: ((UInt64, Double) -> Void)?
    var onDeleteBus: ((UInt64) -> Void)?
    private let canvas=MixerCanvasView()
    private var stripViews:[UInt64:MixerStripView]=[:]
    override init(frame: NSRect) { super.init(frame:frame); drawsBackground=false; hasHorizontalScroller=true; hasVerticalScroller=false; documentView=canvas }
    required init?(coder:NSCoder) { fatalError("init(coder:) is unavailable") }
    func updateMeters(_ snapshots:[UInt64:MixerMeterSnapshot]) { for(id,snapshot) in snapshots { stripViews[id]?.model.meter=snapshot } }
    private func rebuild() {
        canvas.subviews.forEach { $0.removeFromSuperview() }
        stripViews.removeAll()
        for (index, model) in strips.enumerated() {
            var displayModel=model;displayModel.channelNumber=index + 1
            let strip = MixerStripView(model: displayModel)
            stripViews[model.id] = strip
            strip.onSelect = { [weak self] in self?.onSelect?($0) }
            strip.onArm = { [weak self] in self?.onArm?($0, $1) }
            strip.onMute = { [weak self] in self?.onMute?($0, $1) }
            strip.onSolo = { [weak self] in self?.onSolo?($0, $1) }
            strip.onVolumeBegin = { [weak self] in self?.onVolumeGestureBegin?($0) }
            strip.onVolume = { [weak self] in self?.onVolume?($0, $1) }
            strip.onVolumeEnd = { [weak self] in self?.onVolumeGestureEnd?($0, $1) }
            strip.onPan = { [weak self] in self?.onPan?($0, $1) }
            strip.onDeleteBus = { [weak self] id in
                guard let self else { return }
                self.onDeleteBus?(id)
            }
            canvas.addSubview(strip)
        }
        layoutStrips()
    }
    private func layoutStrips() {
        let stripWidth: CGFloat = bounds.height >= 300 ? 80 : 74
        let gap: CGFloat = 4
        let groupGap: CGFloat = 10
        let extraGaps = strips.enumerated().reduce(0) { partial, item in
            guard item.offset > 0 else { return partial }
            return partial + (item.element.kind != strips[item.offset - 1].kind ? 1 : 0)
        }
        let canvasSize = NSSize(
            width: max(bounds.width, CGFloat(strips.count) * (stripWidth + gap) + gap + CGFloat(extraGaps) * groupGap),
            height: max(150, bounds.height)
        )
        if canvas.frame.size != canvasSize { canvas.frame.size = canvasSize }
        var x = gap
        for (index, model) in strips.enumerated() {
            if index > 0, model.kind != strips[index - 1].kind { x += groupGap }
            stripViews[model.id]?.frame = NSRect(
                x: x,
                y: 0,
                width: stripWidth,
                height: canvasSize.height
            )
            x += stripWidth + gap
        }
    }
    override func layout() { super.layout(); layoutStrips() }
}
