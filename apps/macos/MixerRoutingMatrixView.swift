import AppKit

/// Large-session routing overview. Project mutations still go through the existing
/// routing/send callbacks, so Session remains the only graph authority.
@MainActor
final class MixerRoutingMatrixView: NSView, NSSearchFieldDelegate {
    enum Mode: Int { case main, sends }

    var strips: [MixerStripModel] = [] { didSet { rebuild() } }
    var onOutput: ((UInt64, UInt64) -> Void)?
    var onSend: ((UInt64, MixerSendAction) -> Void)?

    private let mode = NSSegmentedControl(labels:["Main outputs","Sends"],trackingMode:.selectOne,target:nil,action:nil)
    private let search = NSSearchField()
    private let scroll = NSScrollView()
    private lazy var grid = GridView(owner:self)

    private(set) var activeMode: Mode = .main
    private(set) var rows: [MixerStripModel] = []
    private(set) var destinations: [MixerStripModel] = []

    override var isFlipped: Bool { true }
    override init(frame: NSRect) {
        super.init(frame:frame)
        wantsLayer=true;layer?.backgroundColor=DAWDesignTokens.Color.canvas.cgColor
        mode.selectedSegment=0;mode.target=self;mode.action=#selector(modeChanged)
        search.placeholderString="Filter channels";search.delegate=self
        scroll.drawsBackground=false;scroll.hasVerticalScroller=true;scroll.hasHorizontalScroller=true;scroll.documentView=grid
        addSubview(mode);addSubview(search);addSubview(scroll)
        mode.setAccessibilityLabel("Routing matrix mode")
        search.setAccessibilityLabel("Filter routing matrix channels")
        setAccessibilityElement(true);setAccessibilityRole(.group);setAccessibilityLabel("Mixer routing matrix")
    }
    required init?(coder:NSCoder){fatalError("init(coder:) is unavailable")}

    func controlTextDidChange(_ obj: Notification) { rebuild() }
    @objc private func modeChanged(){ setMode(Mode(rawValue:mode.selectedSegment) ?? .main) }
    func setMode(_ value:Mode){activeMode=value;mode.selectedSegment=value.rawValue;rebuild()}

    private func rebuild(){
        let q=search.stringValue.trimmingCharacters(in:.whitespacesAndNewlines)
        let allRows=strips.filter{activeMode == .sends ? $0.kind == .track : $0.kind != .master}
        rows=q.isEmpty ? allRows : allRows.filter{$0.title.localizedCaseInsensitiveContains(q)||$0.outputName.localizedCaseInsensitiveContains(q)}
        let buses=strips.filter{$0.kind == .bus}
        if activeMode == .main {
            destinations=[MixerStripModel(id:0,kind:.master,title:"Master")]+buses
        } else { destinations=buses }
        grid.rebuildGeometry();grid.needsDisplay=true
    }

    func activate(rowID:UInt64,destinationID:UInt64){
        guard let row=rows.first(where:{$0.id == rowID}) else{return}
        guard destinations.contains(where:{$0.id == destinationID}) else{return}
        if activeMode == .main {
            guard row.kind != .master, row.id != destinationID else{return}
            if row.outputID != destinationID { onOutput?(row.id,destinationID) }
        } else {
            guard row.kind == .track, destinationID != 0 else{return}
            if row.sends.contains(where:{$0.busID == destinationID}) { onSend?(row.id,.remove(destinationID)) }
            else { onSend?(row.id,.add(destinationID)) }
        }
    }

    fileprivate func sendMenu(rowID:UInt64,destinationID:UInt64) -> NSMenu? {
        guard activeMode == .sends,
              let row=rows.first(where:{$0.id == rowID}),row.kind == .track,
              let send=row.sends.first(where:{$0.busID == destinationID}) else{return nil}
        let menu=NSMenu();menu.autoenablesItems=false
        menu.addItem(MixerMenuItem("Edit level…") { [weak self] in self?.onSend?(rowID,.edit(destinationID)) })
        menu.addItem(MixerMenuItem(send.preFader ? "Switch to POST" : "Switch to PRE") { [weak self] in self?.onSend?(rowID,.tap(destinationID,!send.preFader)) })
        menu.addItem(.separator())
        menu.addItem(MixerMenuItem("Remove send") { [weak self] in self?.onSend?(rowID,.remove(destinationID)) })
        return menu
    }

    override func layout(){
        super.layout();let w=bounds.width
        mode.frame=NSRect(x:10,y:8,width:min(230,max(120,w*0.34)),height:28)
        search.frame=NSRect(x:mode.frame.maxX+10,y:9,width:max(100,min(260,w-mode.frame.maxX-20)),height:26)
        scroll.frame=NSRect(x:0,y:44,width:w,height:max(0,bounds.height-44))
        grid.rebuildGeometry()
    }

    @MainActor
    private final class GridView:NSView {
        weak var owner:MixerRoutingMatrixView?
        let rowLabelWidth:CGFloat=150,columnWidth:CGFloat=86,rowHeight:CGFloat=25,headerHeight:CGFloat=32
        override var isFlipped:Bool{true}
        init(owner:MixerRoutingMatrixView){self.owner=owner;super.init(frame:.zero);setAccessibilityElement(true);setAccessibilityRole(.group);setAccessibilityLabel("Routing cells")}
        required init?(coder:NSCoder){fatalError("init(coder:) is unavailable")}
        func rebuildGeometry(){
            guard let owner else{return}
            frame.size=NSSize(width:max(owner.scroll.contentSize.width,rowLabelWidth+CGFloat(owner.destinations.count)*columnWidth),height:max(owner.scroll.contentSize.height,headerHeight+CGFloat(owner.rows.count)*rowHeight))
        }
        private func hit(_ point:NSPoint)->(MixerStripModel,MixerStripModel)?{
            guard let owner,point.x>=rowLabelWidth,point.y>=headerHeight else{return nil}
            let rowIndex=Int((point.y-headerHeight)/rowHeight),columnIndex=Int((point.x-rowLabelWidth)/columnWidth)
            guard owner.rows.indices.contains(rowIndex),owner.destinations.indices.contains(columnIndex) else{return nil}
            return(owner.rows[rowIndex],owner.destinations[columnIndex])
        }
        override func mouseDown(with event:NSEvent){guard let owner,let pair=hit(convert(event.locationInWindow,from:nil)) else{return};owner.activate(rowID:pair.0.id,destinationID:pair.1.id)}
        override func rightMouseDown(with event:NSEvent){guard let owner,let pair=hit(convert(event.locationInWindow,from:nil)),let menu=owner.sendMenu(rowID:pair.0.id,destinationID:pair.1.id) else{return};NSMenu.popUpContextMenu(menu,with:event,for:self)}
        override func draw(_ dirtyRect:NSRect){
            guard let owner else{return}
            DAWDesignTokens.Color.canvas.setFill();bounds.fill()
            DAWDesignTokens.Color.surface.setFill();NSRect(x:0,y:0,width:bounds.width,height:headerHeight).fill()
            let headerAttrs:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:9,weight:.semibold),.foregroundColor:DAWDesignTokens.Color.secondaryText]
            let header=(owner.activeMode == .main ? "CHANNEL → OUTPUT" : "CHANNEL → SEND") as NSString
            header.draw(at:NSPoint(x:8,y:9),withAttributes:headerAttrs)
            for (column,destination) in owner.destinations.enumerated(){
                let x=rowLabelWidth+CGFloat(column)*columnWidth
                let title=destination.title as NSString
                title.draw(in:NSRect(x:x+4,y:5,width:columnWidth-8,height:22),withAttributes:headerAttrs)
                DAWDesignTokens.Color.border.withAlphaComponent(0.35).setFill();NSRect(x:x,y:0,width:1,height:bounds.height).fill()
            }
            for (rowIndex,row) in owner.rows.enumerated(){
                let y=headerHeight+CGFloat(rowIndex)*rowHeight
                if rowIndex%2 == 1 { DAWDesignTokens.Color.surface.withAlphaComponent(0.32).setFill();NSRect(x:0,y:y,width:bounds.width,height:rowHeight).fill() }
                let tint=row.color ?? DAWDesignTokens.Color.accent
                tint.withAlphaComponent(0.7).setFill();NSRect(x:2,y:y+4,width:3,height:rowHeight-8).fill()
                let nameAttrs:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:10,weight:row.isSelected ? .semibold:.regular),.foregroundColor:row.isSelected ? DAWDesignTokens.Color.text:DAWDesignTokens.Color.secondaryText]
                (row.title as NSString).draw(in:NSRect(x:10,y:y+5,width:rowLabelWidth-15,height:18),withAttributes:nameAttrs)
                for (column,destination) in owner.destinations.enumerated(){
                    let x=rowLabelWidth+CGFloat(column)*columnWidth
                    let cell=NSRect(x:x+1,y:y+1,width:columnWidth-2,height:rowHeight-2)
                    if owner.activeMode == .main {
                        let invalid=row.id == destination.id && row.kind == .bus
                        if invalid { NSColor.systemRed.withAlphaComponent(0.08).setFill();cell.fill();continue }
                        if row.outputID == destination.id {
                            tint.withAlphaComponent(0.3).setFill();NSBezierPath(roundedRect:cell.insetBy(dx:5,dy:4),xRadius:5,yRadius:5).fill()
                            tint.setFill();NSBezierPath(ovalIn:NSRect(x:cell.midX-3,y:cell.midY-3,width:6,height:6)).fill()
                        }
                    } else if row.kind == .track,let send=row.sends.first(where:{$0.busID == destination.id}) {
                        NSColor.systemMint.withAlphaComponent(0.24).setFill();NSBezierPath(roundedRect:cell.insetBy(dx:5,dy:4),xRadius:5,yRadius:5).fill()
                        let text=String(format:"%@ %.0f",send.preFader ? "PRE":"POST",send.gainDb) as NSString
                        let attrs:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedDigitSystemFont(ofSize:8,weight:.medium),.foregroundColor:NSColor.systemMint]
                        text.draw(in:cell.insetBy(dx:7,dy:5),withAttributes:attrs)
                    }
                }
                DAWDesignTokens.Color.border.withAlphaComponent(0.22).setFill();NSRect(x:0,y:y+rowHeight-1,width:bounds.width,height:1).fill()
            }
        }
    }
}
