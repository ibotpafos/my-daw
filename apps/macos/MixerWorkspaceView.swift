import AppKit

@MainActor
private final class MixerCanvasView: NSView { override var isFlipped: Bool { true } }

/// Toolbar + selected-channel inspector + horizontally scrolling bank + fixed master.
/// All project mutations are callbacks; the view owns only visibility and metering.
@MainActor
final class MixerWorkspaceView: NSView, NSSearchFieldDelegate {
    var strips: [MixerStripModel] = [] { didSet { reconcile() } }
    var onSelect: ((UInt64) -> Void)?
    var onArm: ((UInt64,Bool) -> Void)?
    var onMute: ((UInt64,Bool) -> Void)?
    var onSolo: ((UInt64,Bool) -> Void)?
    var onVolumeGestureBegin: ((UInt64) -> Void)?
    var onVolume: ((UInt64,Double) -> Void)?
    var onVolumeGestureEnd: ((UInt64,Double) -> Void)?
    var onPan: ((UInt64,Double) -> Void)?
    var onDeleteBus: ((UInt64) -> Void)?
    var onOutput: ((UInt64,UInt64) -> Void)?
    var onInsert: ((UInt64,MixerInsertAction) -> Void)?
    var onSend: ((UInt64,MixerSendAction) -> Void)?
    var onSendGainBegin: ((UInt64,UInt64) -> Void)?
    var onSendGain: ((UInt64,UInt64,Double) -> Void)?
    var onSendGainEnd: ((UInt64,UInt64,Double) -> Void)?
    var onCreateBus: (() -> Void)?
    var onFocus: ((Bool) -> Void)?
    var onResetPeaks: (() -> Void)?
    var onClearSolo: (() -> Void)?
    var onRename: ((UInt64,String) -> Void)?
    var editingEnabled = true { didSet { for view in stripViews.values { view.updateFaderMode(); view.updateDestinations() }; inspector?.updateFaderMode() } }
    private(set) var sendTargetID: UInt64?
    private(set) var stripViews: [UInt64:MixerStripView] = [:]
    private(set) var visibleIDs: [UInt64] = []
    let consoleState = MixerConsoleState()
    let search = NSSearchField()
    let filter = NSSegmentedControl(labels:["All","Tracks","Buses"],trackingMode:.selectOne,target:nil,action:nil)
    let density = NSPopUpButton(frame:.zero,pullsDown:false)
    let sendTarget = NSPopUpButton(frame:.zero,pullsDown:false)
    private let focus = MixerActionButton("Focus")
    private var focused = false
    private let reset = MixerActionButton("Reset peaks"), addBus = MixerActionButton("+ Bus"), inspect = MixerActionButton("Inspector")
    private let caption = NSTextField(labelWithString: "CONSOLE")
    private let inspectorCaption = NSTextField(labelWithString:"SELECTED CHANNEL")
    private let empty = NSTextField(labelWithString:"No matching channels")
    private let scroll = NSScrollView()
    private let canvas = MixerCanvasView()
    private var inspector: MixerStripView?
    private var inspectorVisible = true
    // Read-only compatibility for existing layout diagnostics.
    var documentView: NSView? { scroll.documentView }
    var contentView: NSClipView { scroll.contentView }
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame:frame)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.canvas.cgColor
        scroll.drawsBackground = false; scroll.hasHorizontalScroller = true; scroll.hasVerticalScroller = false
        scroll.documentView = canvas
        for view in [caption,filter,search,density,sendTarget,reset,addBus,inspect,focus,scroll,inspectorCaption,empty] { addSubview(view) }
        caption.font = .monospacedSystemFont(ofSize:11,weight:.semibold)
        inspectorCaption.font = .monospacedSystemFont(ofSize:10,weight:.medium)
        inspectorCaption.textColor = DAWDesignTokens.Color.secondaryText
        search.placeholderString = "Find channel"; search.delegate = self
        filter.selectedSegment = 0; filter.target = self; filter.action = #selector(viewOptionsChanged)
        for title in ["Compact","Standard","Wide"] { density.addItem(withTitle:title) }; density.selectItem(at:1)
        density.target = self; density.action = #selector(viewOptionsChanged)
        sendTarget.addItem(withTitle:"Faders: Main")
        sendTarget.target = self; sendTarget.action = #selector(sendTargetChanged)
        inspect.setButtonType(.toggle); inspect.state = .on
        inspect.invoke = { [weak self] in guard let self else {return}; inspectorVisible.toggle(); needsLayout = true }
        focus.setButtonType(.toggle)
        focus.invoke = { [weak self] in guard let self else { return }; focused.toggle(); onFocus?(focused) }
        reset.invoke = { [weak self] in self?.resetPeaks() }
        addBus.invoke = { [weak self] in self?.onCreateBus?() }
        search.setAccessibilityLabel("Find mixer channel")
        filter.setAccessibilityLabel("Channel type filter")
        density.setAccessibilityLabel("Mixer strip width")
        sendTarget.setAccessibilityLabel("Fader target: main or send destination")
        sendTarget.toolTip = "Sends on faders: only existing sends are editable. Bus and master faders retain their main level."
        empty.textColor = DAWDesignTokens.Color.secondaryText
        for popup in [density,sendTarget] { popup.controlSize = .small; popup.font = .systemFont(ofSize:11) }
    }
    required init?(coder:NSCoder) { fatalError("init(coder:) is unavailable") }
    func controlTextDidChange(_ obj: Notification) { needsLayout = true }
    @objc private func viewOptionsChanged() { needsLayout = true }
    @objc private func sendTargetChanged() { setSendTarget((sendTarget.selectedItem?.representedObject as? NSNumber)?.uint64Value) }
    func setSendTarget(_ id: UInt64?) {
        sendTargetID = strips.contains { $0.kind == .bus && $0.id == id } ? id : nil
        sendTarget.select(sendTarget.itemArray.first { ($0.representedObject as? NSNumber)?.uint64Value == sendTargetID } ?? sendTarget.item(at:0))
        for view in stripViews.values { view.updateFaderMode() }
        inspector?.updateFaderMode()
        caption.stringValue = sendTargetID == nil ? "CONSOLE" : "SEND MIX"
        caption.textColor = sendTargetID == nil ? .labelColor : .systemMint
    }
    func previewLevel(_ id:UInt64,send:UInt64?,value:Double,source:MixerStripView) {
        for view in [stripViews[id],inspector].compactMap({$0}) where view !== source && view.model.id == id && view.sendTarget == send {
            view.fader.valueDb = value
            view.gainField.stringValue = MixerScale.label(value)
        }
    }
    func resetFocus() { focused = false; focus.state = .off }
    func resetPeaks() { stripViews.values.forEach { $0.meter.resetClip() }; inspector?.meter.resetClip(); onResetPeaks?() }
    func updateMeters(_ snapshots: [UInt64:MixerMeterSnapshot]) {
        for (id,snapshot) in snapshots { stripViews[id]?.updateMeter(snapshot) }
        if let inspector, let snapshot = snapshots[inspector.model.id] { inspector.updateMeter(snapshot) }
    }
    private func reconcile() {
        let valid = Set(strips.map(\.id))
        for id in Array(stripViews.keys) where !valid.contains(id) { stripViews.removeValue(forKey:id)?.removeFromSuperview() }
        for (i, original) in strips.enumerated() {
            var model = original; model.channelNumber = i+1
            if let existing = stripViews[model.id] { existing.apply(model) }
            else {
                let view = MixerStripView(model:model); view.workspace = self
                stripViews[model.id] = view
                if model.kind == .master { addSubview(view) } else { canvas.addSubview(view) }
                view.apply(model)
            }
        }
        sendTarget.removeAllItems(); sendTarget.addItem(withTitle:"Faders: Main")
        for bus in strips where bus.kind == .bus { sendTarget.addItem(withTitle:"Send → \(bus.title)"); sendTarget.lastItem?.representedObject = NSNumber(value:bus.id) }
        setSendTarget(sendTargetID)
        if let selected = strips.first(where:{ $0.isSelected }) {
            if inspector?.model.id != selected.id {
                inspector?.removeFromSuperview(); inspector = MixerStripView(model:selected); inspector?.workspace = self
                if let inspector { addSubview(inspector) }
            }
            inspector?.apply(selected)
        } else { inspector?.removeFromSuperview(); inspector = nil }
        needsLayout = true
    }
    override func layout() {
        super.layout()
        let w = bounds.width, h = bounds.height
        caption.frame = NSRect(x:10,y:14,width:70,height:18)
        filter.frame = NSRect(x:88,y:8,width:176,height:28)
        search.frame = NSRect(x:275,y:9,width:150,height:26)
        density.frame = NSRect(x:435,y:9,width:95,height:26)
        sendTarget.frame = NSRect(x:540,y:9,width:180,height:26)
        focus.frame = NSRect(x:w-297,y:9,width:62,height:26)
        inspect.frame = NSRect(x:w-230,y:9,width:75,height:26)
        addBus.frame = NSRect(x:w-150,y:9,width:60,height:26)
        reset.frame = NSRect(x:w-85,y:9,width:80,height:26)
        // Narrow windows use two toolbar rows instead of overlapping controls.
        let narrow = w < 1040
        if narrow {
            sendTarget.frame = NSRect(x:10,y:43,width:180,height:26)
            inspect.frame = NSRect(x:202,y:43,width:80,height:26)
            addBus.frame = NSRect(x:290,y:43,width:70,height:26)
            reset.frame = NSRect(x:370,y:43,width:95,height:26)
            focus.frame = NSRect(x:477,y:43,width:70,height:26)
        }
        let top: CGFloat = narrow ? 78 : 46
        let bottom: CGFloat = 5
        consoleState.density = MixerConsoleState.Density(rawValue:density.indexOfSelectedItem) ?? .regular
        consoleState.filter = MixerConsoleState.Filter(rawValue:filter.selectedSegment) ?? .all
        consoleState.search = search.stringValue
        let stripWidth = consoleState.density.width
        let useInspector = inspectorVisible && inspector != nil && w >= 1180 && h >= 480
        let left: CGFloat = useInspector ? 210 : 5
        inspector?.isHidden = !useInspector; inspectorCaption.isHidden = !useInspector
        inspectorCaption.frame = NSRect(x:14,y:top+4,width:184,height:18)
        inspector?.frame = NSRect(x:10,y:top+27,width:184,height:max(210,h-top-27-bottom))
        let master = strips.first { $0.kind == .master }.flatMap { stripViews[$0.id] }
        let masterWidth: CGFloat = master == nil ? 0 : max(112,stripWidth)+10
        master?.frame = NSRect(x:w-masterWidth+5,y:top,width:masterWidth-10,height:max(0,h-top-bottom))
        scroll.frame = NSRect(x:left,y:top,width:max(60,w-left-masterWidth-4),height:max(0,h-top-bottom))
        visibleIDs = consoleState.visible(strips).map(\.id)
        let visible = Set(visibleIDs)
        for view in stripViews.values where view.model.kind != .master { view.isHidden = !visible.contains(view.model.id) }
        var x: CGFloat = 0
        var previous: MixerStripKind?
        for id in visibleIDs {
            guard let view = stripViews[id] else { continue }
            if let previous, previous != view.model.kind { x += 10 }
            view.frame = NSRect(x:x,y:0,width:stripWidth,height:max(210,scroll.contentSize.height))
            x += stripWidth+3; previous = view.model.kind
        }
        canvas.frame.size = NSSize(width:max(scroll.contentSize.width,x),height:max(210,scroll.contentSize.height))
        empty.isHidden = !visibleIDs.isEmpty
        empty.frame = NSRect(x:left+20,y:top+45,width:max(30,scroll.frame.width-40),height:24)
    }
}
