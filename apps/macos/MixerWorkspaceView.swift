import AppKit

@MainActor
private final class MixerCanvasView: NSView { override var isFlipped: Bool { true } }

/// Compact whole-console map. It is intentionally presentation-only: click selects/reveals a
/// stable channel ID while meter values come from the same snapshots as the channel strips.
@MainActor
private final class MixerMeterBridgeView: NSView {
    var models: [MixerStripModel] = [] { didSet { needsDisplay = true } }
    var snapshots: [UInt64:MixerMeterSnapshot] = [:] { didSet { needsDisplay = true } }
    var leftPinned = Set<UInt64>() { didSet { needsDisplay = true } }
    var rightPinned = Set<UInt64>() { didSet { needsDisplay = true } }
    var onSelect: ((UInt64) -> Void)?
    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Mixer overview and meter bridge")
        setAccessibilityHelp("Click a channel cell to select it and reveal it in the mixer.")
        toolTip = "Channel overview · sample peak meters · click to reveal"
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    private func itemWidth() -> CGFloat { models.isEmpty ? bounds.width : bounds.width / CGFloat(models.count) }
    override func mouseDown(with event: NSEvent) {
        guard !models.isEmpty, bounds.width > 0 else { return }
        let point = convert(event.locationInWindow, from: nil)
        let index = min(models.count - 1, max(0, Int(point.x / max(0.001, itemWidth()))))
        onSelect?(models[index].id)
    }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.surface.withAlphaComponent(0.72).setFill(); bounds.fill()
        guard !models.isEmpty else { return }
        let width = itemWidth()
        for (index, model) in models.enumerated() {
            let x = CGFloat(index) * width
            let rect = NSRect(x:x,y:0,width:max(1,width),height:bounds.height)
            let tint = model.color ?? DAWDesignTokens.Color.accent
            tint.withAlphaComponent(model.isSelected ? 0.24 : 0.09).setFill(); rect.insetBy(dx:0.5,dy:1).fill()
            let snapshot = snapshots[model.id] ?? .init()
            let peak = max(snapshot.leftPeak,snapshot.rightPeak)
            let level = CGFloat(MixerScale.meterPosition(peak))
            let meterArea = rect.insetBy(dx:max(1,min(4,width*0.18)),dy:5)
            if meterArea.height > 1, level > 0 {
                let meterRect = NSRect(x:meterArea.minX,y:meterArea.maxY-meterArea.height*level,width:max(1,meterArea.width),height:meterArea.height*level)
                DAWDataVisuals.meterColor(for: level).withAlphaComponent(0.78).setFill(); meterRect.fill()
            }
            if leftPinned.contains(model.id) {
                NSColor.systemCyan.setFill(); NSRect(x:rect.minX,y:1,width:min(3,rect.width),height:max(1,rect.height-2)).fill()
            } else if rightPinned.contains(model.id) {
                NSColor.systemPurple.setFill(); NSRect(x:max(rect.minX,rect.maxX-min(3,rect.width)),y:1,width:min(3,rect.width),height:max(1,rect.height-2)).fill()
            }
            if model.totalInsertLatencyFrames > 0 {
                NSColor.systemOrange.setFill(); NSBezierPath(ovalIn:NSRect(x:max(rect.minX,rect.maxX-5),y:2,width:min(4,rect.width),height:4)).fill()
            }
            if model.hasUnavailableInsert {
                NSColor.systemRed.setFill(); NSRect(x:rect.minX,y:rect.maxY-3,width:max(1,rect.width),height:2).fill()
            }
            if model.isSelected {
                tint.setStroke(); let path=NSBezierPath(rect:rect.insetBy(dx:1,dy:1));path.lineWidth=1.5;path.stroke()
            }
            if width >= 34 {
                let label = width >= 64 ? model.title : String(model.channelNumber > 0 ? model.channelNumber : index + 1)
                let attrs:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:8,weight:model.isSelected ? .semibold:.regular),.foregroundColor:DAWDesignTokens.Color.secondaryText]
                NSGraphicsContext.saveGraphicsState()
                NSBezierPath(rect:rect.insetBy(dx:2,dy:1)).addClip()
                (label as NSString).draw(at:NSPoint(x:rect.minX+3,y:2),withAttributes:attrs)
                NSGraphicsContext.restoreGraphicsState()
            }
        }
    }
}

/// Toolbar + selected-channel inspector + horizontally scrolling bank + fixed zones/master.
/// All project mutations are callbacks; visibility, zones and virtualization are presentation state.
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
    var onUndo: (() -> Void)?
    var onRedo: (() -> Void)?
    var historyState: (() -> (canUndo: Bool, canRedo: Bool))?
    var editingEnabled = true {
        didSet {
            for view in stripViews.values { view.updateFaderMode(); view.updateDestinations() }
            inspector?.updateFaderMode(); updateHistoryButtons()
        }
    }
    private(set) var sendTargetID: UInt64?
    private(set) var stripViews: [UInt64:MixerStripView] = [:]
    private(set) var visibleIDs: [UInt64] = []
    private(set) var scrollingIDs: [UInt64] = []
    let consoleState = MixerConsoleState()
    let search = NSSearchField()
    let filter = NSSegmentedControl(labels:["All","Tracks","Buses"],trackingMode:.selectOne,target:nil,action:nil)
    let density = NSPopUpButton(frame:.zero,pullsDown:false)
    let sendTarget = NSPopUpButton(frame:.zero,pullsDown:false)
    private let focus = MixerActionButton("Focus")
    private let inspect = MixerActionButton("Inspector")
    private let reset = MixerActionButton("Reset peaks")
    private let addBus = MixerActionButton("+ Bus")
    private let undoButton = MixerActionButton("↶")
    private let redoButton = MixerActionButton("↷")
    private let visibilityButton = MixerActionButton("Visibility")
    private let zonesButton = MixerActionButton("Zones")
    private let overviewButton = MixerActionButton("Overview")
    private let caption = NSTextField(labelWithString: "CONSOLE")
    private let inspectorCaption = NSTextField(labelWithString:"SELECTED CHANNEL")
    private let empty = NSTextField(labelWithString:"No matching channels")
    private let scroll = NSScrollView()
    private let canvas = MixerCanvasView()
    private let meterBridge = MixerMeterBridgeView(frame:.zero)
    private var inspector: MixerStripView?
    private var inspectorVisible = true
    private var overviewVisible = true
    private var focused = false
    private var latestMeters: [UInt64:MixerMeterSnapshot] = [:]
    private var lastStripWidth: CGFloat = 110
    var documentView: NSView? { scroll.documentView }
    var contentView: NSClipView { scroll.contentView }

    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame:frame)
        wantsLayer = true; layer?.backgroundColor = DAWDesignTokens.Color.canvas.cgColor
        scroll.drawsBackground = false; scroll.hasHorizontalScroller = true; scroll.hasVerticalScroller = false
        scroll.documentView = canvas
        scroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(self,selector:#selector(scrollBoundsChanged),name:NSView.boundsDidChangeNotification,object:scroll.contentView)
        for view in [caption,filter,search,density,sendTarget,undoButton,redoButton,visibilityButton,zonesButton,overviewButton,reset,addBus,inspect,focus,meterBridge,scroll,inspectorCaption,empty] { addSubview(view) }
        caption.font = .monospacedSystemFont(ofSize:11,weight:.semibold)
        inspectorCaption.font = .monospacedSystemFont(ofSize:10,weight:.medium)
        inspectorCaption.textColor = DAWDesignTokens.Color.secondaryText
        search.placeholderString = "Find channel / output"; search.delegate = self
        filter.selectedSegment = 0; filter.target = self; filter.action = #selector(viewOptionsChanged)
        for title in ["Compact","Standard","Wide"] { density.addItem(withTitle:title) }; density.selectItem(at:1)
        density.target = self; density.action = #selector(viewOptionsChanged)
        sendTarget.addItem(withTitle:"Faders: Main")
        sendTarget.target = self; sendTarget.action = #selector(sendTargetChanged)
        inspect.setButtonType(.toggle); inspect.state = .on
        inspect.invoke = { [weak self] in guard let self else{return}; inspectorVisible.toggle(); inspect.state = inspectorVisible ? .on:.off; needsLayout = true }
        focus.setButtonType(.toggle)
        focus.invoke = { [weak self] in guard let self else { return }; focused.toggle(); focus.state = focused ? .on:.off; onFocus?(focused) }
        overviewButton.setButtonType(.toggle); overviewButton.state = .on
        overviewButton.invoke = { [weak self] in guard let self else{return}; overviewVisible.toggle(); overviewButton.state = overviewVisible ? .on:.off; needsLayout = true }
        reset.invoke = { [weak self] in self?.resetPeaks() }
        addBus.invoke = { [weak self] in self?.onCreateBus?() }
        undoButton.invoke = { [weak self] in self?.onUndo?() }
        redoButton.invoke = { [weak self] in self?.onRedo?() }
        visibilityButton.invoke = { [weak self] in self?.showVisibilityMenu() }
        zonesButton.invoke = { [weak self] in self?.showZonesMenu() }
        meterBridge.onSelect = { [weak self] id in self?.selectAndReveal(id) }
        search.setAccessibilityLabel("Find mixer channel or output")
        filter.setAccessibilityLabel("Channel type filter")
        density.setAccessibilityLabel("Mixer strip width")
        sendTarget.setAccessibilityLabel("Fader target: main or send destination")
        undoButton.setAccessibilityLabel("Undo mixer change")
        redoButton.setAccessibilityLabel("Redo mixer change")
        visibilityButton.setAccessibilityLabel("Mixer visibility and channel sections")
        zonesButton.setAccessibilityLabel("Pinned mixer channels")
        overviewButton.setAccessibilityLabel("Show mixer meter bridge")
        sendTarget.toolTip = "Sends on faders: only existing sends are editable. Bus and master faders retain their main level."
        visibilityButton.toolTip = "Show/hide channels and focus inserts, sends or faders."
        zonesButton.toolTip = "Pin up to three channels outside horizontal scrolling."
        empty.textColor = DAWDesignTokens.Color.secondaryText
        for popup in [density,sendTarget] { popup.controlSize = .small; popup.font = .systemFont(ofSize:11) }
        updateHistoryButtons()
    }
    required init?(coder:NSCoder) { fatalError("init(coder:) is unavailable") }

    func controlTextDidChange(_ obj: Notification) { needsLayout = true }
    @objc private func viewOptionsChanged() { needsLayout = true }
    @objc private func sendTargetChanged() { setSendTarget((sendTarget.selectedItem?.representedObject as? NSNumber)?.uint64Value) }
    @objc private func scrollBoundsChanged() { virtualizeScrolledBank() }

    private func updateHistoryButtons() {
        if let state = historyState?() {
            undoButton.isEnabled = editingEnabled && state.canUndo
            redoButton.isEnabled = editingEnabled && state.canRedo
        } else {
            undoButton.isEnabled = false
            redoButton.isEnabled = false
        }
    }

    private func selectedStrip() -> MixerStripModel? { strips.first { $0.isSelected && $0.kind != .master } }

    private func presentationChanged() {
        meterBridge.needsDisplay = true
        needsLayout = true
    }

    func setRackMode(_ mode: MixerConsoleState.RackMode) {
        consoleState.rackMode = mode
        for view in stripViews.values { view.rackMode = mode }
        inspector?.rackMode = mode
        needsLayout = true
    }

    private func showVisibilityMenu() {
        let menu=NSMenu();menu.autoenablesItems=false
        menu.addItem(MixerMenuItem("Show all channels") { [weak self] in self?.consoleState.showAll(); self?.presentationChanged() })
        if let selected=selectedStrip() {
            menu.addItem(MixerMenuItem("Hide selected · \(selected.title)") { [weak self] in self?.consoleState.setHidden(true,id:selected.id); self?.presentationChanged() })
            let busID = selected.kind == .bus ? selected.id : selected.outputID
            if busID != 0 {
                menu.addItem(MixerMenuItem("Show route family") { [weak self] in
                    guard let self else{return}
                    var keep=Set<UInt64>([selected.id,busID])
                    for strip in strips where strip.kind == .track && (strip.outputID == busID || strip.sends.contains(where:{$0.busID == busID})) { keep.insert(strip.id) }
                    consoleState.showOnly(keep,from:strips); presentationChanged()
                })
            }
        }
        menu.addItem(.separator())
        func submenu(_ title:String,_ models:[MixerStripModel]) -> NSMenuItem {
            let parent=NSMenuItem(title:title,action:nil,keyEquivalent:"")
            let child=NSMenu();child.autoenablesItems=false
            for model in models {
                let item=MixerMenuItem(model.title) { [weak self] in self?.consoleState.toggleHidden(model.id); self?.presentationChanged() }
                item.state = consoleState.isHidden(model.id) ? .off:.on
                child.addItem(item)
            }
            parent.submenu=child;return parent
        }
        menu.addItem(submenu("Tracks",strips.filter{$0.kind == .track}))
        menu.addItem(submenu("Buses",strips.filter{$0.kind == .bus}))
        menu.addItem(.separator())
        let sections = NSMenu(); sections.autoenablesItems = false
        let sectionModes: [(String,MixerConsoleState.RackMode)] = [("Full channel",.full),("Inserts focus",.inserts),("Sends focus",.sends),("Faders focus",.faders)]
        for (title, mode) in sectionModes {
            let item=MixerMenuItem(title) { [weak self] in self?.setRackMode(mode) }
            item.state = consoleState.rackMode == mode ? .on:.off
            sections.addItem(item)
        }
        let sectionsItem=NSMenuItem(title:"Channel sections",action:nil,keyEquivalent:"");sectionsItem.submenu=sections;menu.addItem(sectionsItem)
        menu.popUp(positioning:nil,at:NSPoint(x:0,y:visibilityButton.bounds.maxY),in:visibilityButton)
    }

    private func showZonesMenu() {
        let menu=NSMenu();menu.autoenablesItems=false
        if let selected=selectedStrip() {
            menu.addItem(MixerMenuItem("Pin \(selected.title) left") { [weak self] in self?.consoleState.pin(selected.id,to:.left); self?.presentationChanged() })
            menu.addItem(MixerMenuItem("Pin \(selected.title) right") { [weak self] in self?.consoleState.pin(selected.id,to:.right); self?.presentationChanged() })
            menu.addItem(MixerMenuItem("Unpin \(selected.title)",enabled:consoleState.isPinned(selected.id)) { [weak self] in self?.consoleState.pin(selected.id,to:.scrolling); self?.presentationChanged() })
            menu.addItem(.separator())
        }
        menu.addItem(MixerMenuItem("Clear left pins",enabled:!consoleState.leftPinnedIDs.isEmpty) { [weak self] in self?.consoleState.clearPins(.left);self?.presentationChanged() })
        menu.addItem(MixerMenuItem("Clear right pins",enabled:!consoleState.rightPinnedIDs.isEmpty) { [weak self] in self?.consoleState.clearPins(.right);self?.presentationChanged() })
        if !consoleState.leftPinnedIDs.isEmpty || !consoleState.rightPinnedIDs.isEmpty {
            menu.addItem(.separator())
            let names=Dictionary(uniqueKeysWithValues:strips.map{($0.id,$0.title)})
            for id in consoleState.leftPinnedIDs { let item=NSMenuItem(title:"Left · \(names[id] ?? "Channel")",action:nil,keyEquivalent:"");item.isEnabled=false;menu.addItem(item) }
            for id in consoleState.rightPinnedIDs { let item=NSMenuItem(title:"Right · \(names[id] ?? "Channel")",action:nil,keyEquivalent:"");item.isEnabled=false;menu.addItem(item) }
        }
        menu.popUp(positioning:nil,at:NSPoint(x:0,y:zonesButton.bounds.maxY),in:zonesButton)
    }

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
        for (id,snapshot) in snapshots {
            latestMeters[id]=snapshot
            if let view=stripViews[id], !view.isHidden { view.updateMeter(snapshot) }
        }
        meterBridge.snapshots=latestMeters
        if let inspector, let snapshot=snapshots[inspector.model.id] { inspector.updateMeter(snapshot) }
    }

    private func selectAndReveal(_ id: UInt64) {
        onSelect?(id)
        revealChannel(id)
    }

    func revealChannel(_ id: UInt64) {
        guard consoleState.zone(of:id) == .scrolling, scrollingIDs.contains(id), let view=stripViews[id] else { return }
        layoutSubtreeIfNeeded()
        let maxX=max(0,canvas.frame.width-scroll.contentSize.width)
        let target=max(0,min(maxX,view.frame.midX-scroll.contentSize.width/2))
        scroll.contentView.scroll(to:NSPoint(x:target,y:0));scroll.reflectScrolledClipView(scroll.contentView);virtualizeScrolledBank()
    }

    private func reconcile() {
        let valid = Set(strips.filter{$0.kind != .master}.map(\.id))
        consoleState.cleanup(validIDs:valid)
        let allValid=Set(strips.map(\.id))
        latestMeters=latestMeters.filter{allValid.contains($0.key)}
        for id in Array(stripViews.keys) where !allValid.contains(id) { stripViews.removeValue(forKey:id)?.removeFromSuperview() }
        for (i, original) in strips.enumerated() {
            var model=original;model.channelNumber=i+1
            if let existing=stripViews[model.id] { existing.apply(model); existing.rackMode=consoleState.rackMode }
            else {
                let view=MixerStripView(model:model);view.workspace=self;view.rackMode=consoleState.rackMode;stripViews[model.id]=view
                if model.kind == .master { addSubview(view) } else { canvas.addSubview(view) }
                view.apply(model)
            }
        }
        sendTarget.removeAllItems();sendTarget.addItem(withTitle:"Faders: Main")
        for bus in strips where bus.kind == .bus { sendTarget.addItem(withTitle:"Send → \(bus.title)");sendTarget.lastItem?.representedObject=NSNumber(value:bus.id) }
        setSendTarget(sendTargetID)
        if let selected=strips.first(where:{$0.isSelected}) {
            if inspector?.model.id != selected.id {
                inspector?.removeFromSuperview();inspector=MixerStripView(model:selected);inspector?.workspace=self
                if let inspector { addSubview(inspector) }
            }
            inspector?.rackMode=consoleState.rackMode
            inspector?.apply(selected)
            if selected.totalInsertLatencyFrames > 0 {
                inspectorCaption.stringValue=String(format:"SELECTED · INSERT LAT %.2f ms",selected.totalInsertLatencyMilliseconds)
            } else { inspectorCaption.stringValue="SELECTED CHANNEL" }
            inspectorCaption.textColor=selected.hasUnavailableInsert ? .systemOrange:DAWDesignTokens.Color.secondaryText
        } else {
            inspector?.removeFromSuperview();inspector=nil;inspectorCaption.stringValue="SELECTED CHANNEL"
        }
        meterBridge.snapshots=latestMeters
        updateHistoryButtons();needsLayout=true
    }

    private func attach(_ view:MixerStripView,to parent:NSView) {
        if view.superview !== parent { view.removeFromSuperview();parent.addSubview(view) }
    }

    private func virtualizeScrolledBank() {
        guard scroll.frame.width > 0 else{return}
        let scrollSet=Set(scrollingIDs)
        let overscan=max(100,lastStripWidth*2.2)
        let visibleRect=scroll.contentView.bounds.insetBy(dx:-overscan,dy:0)
        for (id,view) in stripViews where view.model.kind != .master {
            if consoleState.isPinned(id) { view.isHidden=false; if let meter=latestMeters[id]{view.updateMeter(meter)};continue }
            guard scrollSet.contains(id), view.superview === canvas else { view.isHidden=true;continue }
            let show=view.frame.intersects(visibleRect)
            view.isHidden = !show
            if show, let meter=latestMeters[id] { view.updateMeter(meter) }
        }
    }

    private func layoutToolbar(width w:CGFloat) -> CGFloat {
        let rows = w >= 1320 ? 1 : (w >= 820 ? 2 : 3)
        if rows == 1 {
            caption.frame=NSRect(x:10,y:14,width:64,height:18);filter.frame=NSRect(x:80,y:8,width:176,height:28);search.frame=NSRect(x:265,y:9,width:150,height:26);density.frame=NSRect(x:425,y:9,width:92,height:26);sendTarget.frame=NSRect(x:527,y:9,width:178,height:26)
            undoButton.frame=NSRect(x:713,y:9,width:32,height:26);redoButton.frame=NSRect(x:749,y:9,width:32,height:26);visibilityButton.frame=NSRect(x:789,y:9,width:68,height:26);zonesButton.frame=NSRect(x:863,y:9,width:54,height:26);overviewButton.frame=NSRect(x:923,y:9,width:70,height:26);focus.frame=NSRect(x:999,y:9,width:58,height:26);inspect.frame=NSRect(x:1063,y:9,width:72,height:26);addBus.frame=NSRect(x:1141,y:9,width:58,height:26);reset.frame=NSRect(x:1205,y:9,width:88,height:26)
            return 46
        }
        if rows == 2 {
            caption.frame=NSRect(x:10,y:14,width:64,height:18);filter.frame=NSRect(x:80,y:8,width:176,height:28);search.frame=NSRect(x:265,y:9,width:160,height:26);density.frame=NSRect(x:435,y:9,width:92,height:26);sendTarget.frame=NSRect(x:537,y:9,width:180,height:26)
            undoButton.frame=NSRect(x:10,y:43,width:32,height:26);redoButton.frame=NSRect(x:46,y:43,width:32,height:26);visibilityButton.frame=NSRect(x:84,y:43,width:72,height:26);zonesButton.frame=NSRect(x:162,y:43,width:58,height:26);overviewButton.frame=NSRect(x:226,y:43,width:72,height:26);focus.frame=NSRect(x:304,y:43,width:58,height:26);inspect.frame=NSRect(x:368,y:43,width:76,height:26);addBus.frame=NSRect(x:450,y:43,width:62,height:26);reset.frame=NSRect(x:518,y:43,width:90,height:26)
            return 78
        }
        caption.frame=NSRect(x:10,y:14,width:64,height:18);filter.frame=NSRect(x:80,y:8,width:176,height:28);search.frame=NSRect(x:265,y:9,width:min(150,max(80,w-275)),height:26)
        density.frame=NSRect(x:10,y:43,width:94,height:26);sendTarget.frame=NSRect(x:110,y:43,width:180,height:26);undoButton.frame=NSRect(x:296,y:43,width:32,height:26);redoButton.frame=NSRect(x:332,y:43,width:32,height:26);visibilityButton.frame=NSRect(x:370,y:43,width:72,height:26);zonesButton.frame=NSRect(x:448,y:43,width:58,height:26)
        overviewButton.frame=NSRect(x:10,y:78,width:72,height:26);focus.frame=NSRect(x:88,y:78,width:58,height:26);inspect.frame=NSRect(x:152,y:78,width:76,height:26);addBus.frame=NSRect(x:234,y:78,width:62,height:26);reset.frame=NSRect(x:302,y:78,width:90,height:26)
        return 110
    }

    override func layout() {
        super.layout()
        let w=bounds.width,h=bounds.height
        let top=layoutToolbar(width:w),bottom:CGFloat=5
        consoleState.density=MixerConsoleState.Density(rawValue:density.indexOfSelectedItem) ?? .regular
        consoleState.filter=MixerConsoleState.Filter(rawValue:filter.selectedSegment) ?? .all
        consoleState.search=search.stringValue
        let stripWidth=consoleState.density.width;lastStripWidth=stripWidth
        let bridgeHeight:CGFloat=overviewVisible && h-top >= 280 ? 44:0
        meterBridge.isHidden=bridgeHeight==0
        let channelTop=top+bridgeHeight
        let channelHeight=max(0,h-channelTop-bottom)
        let useInspector=inspectorVisible && inspector != nil && w >= 1180 && channelHeight >= 360
        var left:CGFloat=useInspector ? 210:5
        inspector?.isHidden = !useInspector;inspectorCaption.isHidden = !useInspector
        inspectorCaption.frame=NSRect(x:14,y:channelTop+4,width:184,height:18)
        inspector?.frame=NSRect(x:10,y:channelTop+27,width:184,height:max(0,h-channelTop-27-bottom))

        let master=strips.first{$0.kind == .master}.flatMap{stripViews[$0.id]}
        let masterWidth:CGFloat=master == nil ? 0:max(112,stripWidth)+10
        let leftPinned=consoleState.pinned(strips,in:.left),rightPinned=consoleState.pinned(strips,in:.right)
        let rightSpan=rightPinned.isEmpty ? 0:CGFloat(rightPinned.count)*(stripWidth+3)
        let masterX=w-masterWidth
        let rightStart=masterX-rightSpan

        for view in stripViews.values where view.model.kind != .master { view.isHidden=true }
        for model in leftPinned {
            guard let view=stripViews[model.id] else{continue};attach(view,to:self);view.isHidden=false
            view.frame=NSRect(x:left,y:channelTop,width:stripWidth,height:channelHeight);if let meter=latestMeters[model.id]{view.updateMeter(meter)};left += stripWidth+3
        }
        var rightX=rightStart
        for model in rightPinned {
            guard let view=stripViews[model.id] else{continue};attach(view,to:self);view.isHidden=false
            view.frame=NSRect(x:rightX,y:channelTop,width:stripWidth,height:channelHeight);if let meter=latestMeters[model.id]{view.updateMeter(meter)};rightX += stripWidth+3
        }
        if let master {
            attach(master,to:self);master.isHidden=false;master.frame=NSRect(x:masterX+5,y:channelTop,width:max(1,masterWidth-10),height:channelHeight)
            if let meter=latestMeters[master.model.id]{master.updateMeter(meter)}
        }

        let scrollRight=rightStart-4
        scroll.frame=NSRect(x:left,y:channelTop,width:max(60,scrollRight-left),height:channelHeight)
        meterBridge.frame=NSRect(x:useInspector ? 210:5,y:top,width:max(60,w-(useInspector ? 210:5)-masterWidth-4),height:max(0,bridgeHeight-4))
        meterBridge.models=consoleState.overview(strips);meterBridge.leftPinned=Set(consoleState.leftPinnedIDs);meterBridge.rightPinned=Set(consoleState.rightPinnedIDs)

        scrollingIDs=consoleState.visible(strips).map(\.id)
        visibleIDs=leftPinned.map(\.id)+scrollingIDs+rightPinned.map(\.id)
        var x:CGFloat=0;var previous:MixerStripKind?
        for id in scrollingIDs {
            guard let view=stripViews[id] else{continue};attach(view,to:canvas)
            if let previous,previous != view.model.kind{x += 10}
            view.frame=NSRect(x:x,y:0,width:stripWidth,height:max(210,scroll.contentSize.height));x += stripWidth+3;previous=view.model.kind
        }
        canvas.frame.size=NSSize(width:max(scroll.contentSize.width,x),height:max(210,scroll.contentSize.height))
        virtualizeScrolledBank()
        empty.isHidden = !visibleIDs.isEmpty
        empty.frame=NSRect(x:left+20,y:channelTop+45,width:max(30,scroll.frame.width-40),height:24)
    }
}
