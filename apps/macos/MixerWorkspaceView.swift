import AppKit

enum MixerStripKind: Sendable { case track, bus, master }

struct MixerMeterSnapshot: Sendable, Equatable {
    var leftPeak: Float = 0
    var rightPeak: Float = 0
    var leftHold: Float = 0
    var rightHold: Float = 0
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
    var color: NSColor? = nil
    var volumeDb: Double = 0
    var pan: Double = 0
    var meter = MixerMeterSnapshot()
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
    private let title=NSTextField(labelWithString: "")
    private let meter=MixerMeterView()
    private let fader=MixerFaderView()
    private let pan=NSSlider(value:0,minValue:-1,maxValue:1,target:nil,action:nil)
    private let arm=NSButton(title:"R",target:nil,action:nil), mute=NSButton(title:"M",target:nil,action:nil), solo=NSButton(title:"S",target:nil,action:nil)
    private let inserts=NSTextField(wrappingLabelWithString:""), sends=NSTextField(wrappingLabelWithString:"")
    init(model: MixerStripModel) { self.model=model; super.init(frame:.zero); setup(); refresh() }
    required init?(coder:NSCoder) { fatalError("init(coder:) is unavailable") }
    private func setup() {
        wantsLayer = true; for button in [arm,mute,solo] { button.bezelStyle = .texturedRounded; button.setButtonType(.toggle); addSubview(button) }
        arm.target = self;arm.action = #selector(toggleArm);mute.target = self;mute.action = #selector(toggleMute);solo.target = self;solo.action = #selector(toggleSolo)
        pan.target = self;pan.action = #selector(changePan); title.alignment = .center;title.font = .systemFont(ofSize:11,weight:.semibold);inserts.font = .systemFont(ofSize:9);sends.font = .systemFont(ofSize:9);inserts.textColor = .secondaryLabelColor;sends.textColor = .secondaryLabelColor
        [title,meter,fader,pan,inserts,sends].forEach(addSubview)
        fader.onBegin={ [weak self] in self.map { $0.onVolumeBegin?($0.model.id) } };fader.onChange={ [weak self] value in self.map { $0.onVolume?($0.model.id,value) } };fader.onEnd={ [weak self] value in self.map { $0.onVolumeEnd?($0.model.id,value) } }
    }
    private func refresh() { title.stringValue=model.title;meter.snapshot=model.meter;fader.valueDb=model.volumeDb;pan.doubleValue=model.pan;arm.state=model.isArmed ? .on:.off;mute.state=model.isMuted ? .on:.off;solo.state=model.isSolo ? .on:.off;inserts.stringValue=model.inserts.prefix(3).map { $0.bypassed ? "⊘ \($0.name)" : $0.name }.joined(separator:"\n");sends.stringValue=model.sends.prefix(2).map { "→ \($0.destination) \(String(format:"%.1f",$0.gainDb))" }.joined(separator:"\n");needsDisplay=true }
    override var isFlipped: Bool { true }
    override func layout() {
        let w=bounds.width,h=bounds.height
        title.frame=NSRect(x:5,y:5,width:w-10,height:17)
        arm.frame=NSRect(x:7,y:25,width:25,height:21);mute.frame=NSRect(x:34,y:25,width:25,height:21);solo.frame=NSRect(x:61,y:25,width:25,height:21)
        let controlBottom:CGFloat=48;let panY=max(controlBottom+28,h-26);let faderHeight=max(24,panY-controlBottom-7)
        meter.frame=NSRect(x:14,y:controlBottom,width:18,height:faderHeight);fader.frame=NSRect(x:42,y:controlBottom,width:28,height:faderHeight);pan.frame=NSRect(x:7,y:panY,width:w-14,height:18)
        let showDetails=h>=235;inserts.isHidden = !showDetails;sends.isHidden = !showDetails
        if showDetails { inserts.frame=NSRect(x:5,y:h-92,width:w-10,height:38);sends.frame=NSRect(x:5,y:h-50,width:w-10,height:36) }
    }
    override func mouseDown(with event:NSEvent) { onSelect?(model.id) }
    override func draw(_ dirtyRect:NSRect) { let color=model.color ?? (model.kind == .master ? DAWDesignTokens.Color.warning : (model.kind == .bus ? DAWDesignTokens.Color.accent:DAWDesignTokens.Color.mint));(model.isSelected ? color.withAlphaComponent(0.18):DAWDesignTokens.Color.surface).setFill();NSBezierPath(roundedRect:bounds.insetBy(dx:1,dy:1),xRadius:DAWDesignTokens.Radius.card,yRadius:DAWDesignTokens.Radius.card).fill();color.withAlphaComponent(model.isSelected ? 0.85:0.24).setStroke();NSBezierPath(roundedRect:bounds.insetBy(dx:1,dy:1),xRadius:DAWDesignTokens.Radius.card,yRadius:DAWDesignTokens.Radius.card).stroke() }
    @objc private func toggleArm(){onArm?(model.id,arm.state == .on)}
    @objc private func toggleMute(){onMute?(model.id,mute.state == .on)}
    @objc private func toggleSolo(){onSolo?(model.id,solo.state == .on)}
    @objc private func changePan(){onPan?(model.id,pan.doubleValue)}
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
    private let canvas=MixerCanvasView()
    private var stripViews:[UInt64:MixerStripView]=[:]
    override init(frame: NSRect) { super.init(frame:frame); drawsBackground=false; hasHorizontalScroller=true; hasVerticalScroller=false; documentView=canvas }
    required init?(coder:NSCoder) { fatalError("init(coder:) is unavailable") }
    func updateMeters(_ snapshots:[UInt64:MixerMeterSnapshot]) { for(id,snapshot) in snapshots { stripViews[id]?.model.meter=snapshot } }
    private func rebuild() { canvas.subviews.forEach { $0.removeFromSuperview() };stripViews.removeAll(); let width=max(bounds.width,CGFloat(strips.count)*112+14);canvas.frame=NSRect(x:0,y:0,width:width,height:max(150,bounds.height));for(index,model) in strips.enumerated(){let strip=MixerStripView(model:model);stripViews[model.id]=strip;strip.frame=NSRect(x:7+CGFloat(index)*112,y:0,width:104,height:canvas.bounds.height);strip.onSelect={ [weak self] in self?.onSelect?($0) };strip.onArm={ [weak self] in self?.onArm?($0,$1) };strip.onMute={ [weak self] in self?.onMute?($0,$1) };strip.onSolo={ [weak self] in self?.onSolo?($0,$1) };strip.onVolumeBegin={ [weak self] in self?.onVolumeGestureBegin?($0) };strip.onVolume={ [weak self] in self?.onVolume?($0,$1) };strip.onVolumeEnd={ [weak self] in self?.onVolumeGestureEnd?($0,$1) };strip.onPan={ [weak self] in self?.onPan?($0,$1) };canvas.addSubview(strip)} }
    override func layout() { super.layout(); rebuild() }
}
