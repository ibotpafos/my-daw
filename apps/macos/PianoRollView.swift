import AppKit

@MainActor
private final class PRCanvas: NSView {
    var notes:[PianoRollNote]=[]{didSet{selected=selected.filter{$0<notes.count};needsDisplay=true}}
    var enabled=true{didSet{needsDisplay=true}}
    var onChange:(([PianoRollNote])->Void)?
    var onSelection:((IndexSet)->Void)?
    var selected=IndexSet(){didSet{needsDisplay=true;onSelection?(selected)}}
    var zoom:CGFloat=1{didSet{zoom=min(8,max(0.5,zoom));needsDisplay=true}}
    var snapFrames:UInt64=12_000
    private let keyboard:CGFloat=52, ruler:CGFloat=24, velocity:CGFloat=78, row:CGFloat=13
    private var dragStart:NSPoint?, original:[PianoRollNote]=[], dragIndex:Int?
    override var isFlipped:Bool{true}
    override var acceptsFirstResponder:Bool{true}
    private var gridRect:NSRect{NSRect(x:keyboard,y:ruler,width:max(1,bounds.width-keyboard),height:max(1,bounds.height-ruler-velocity))}
    private var visibleFrames:UInt64{max(48_000,notes.map{$0.startFrames+$0.lengthFrames}.max() ?? 192_000)}
    private func x(_ frame:UInt64)->CGFloat{keyboard+CGFloat(Double(frame)/Double(visibleFrames))*gridRect.width*zoom}
    private func frame(_ x:CGFloat)->UInt64{UInt64(max(0,min(Double(visibleFrames),Double((x-keyboard)/(gridRect.width*zoom))*Double(visibleFrames))))}
    private func y(_ pitch:UInt8)->CGFloat{ruler+CGFloat(127-Int(pitch))*row}
    private func pitch(_ y:CGFloat)->UInt8{UInt8(max(0,min(127,127-Int((y-ruler)/row))))}
    private func noteRect(_ n:PianoRollNote)->NSRect{NSRect(x:x(n.startFrames),y:y(n.pitch),width:max(5,x(n.startFrames+n.lengthFrames)-x(n.startFrames)),height:row-1)}
    override func draw(_ dirty:NSRect){
        DAWDesignTokens.Color.canvas.setFill();bounds.fill();let g=gridRect
        for p in 0...127 { let yy=ruler+CGFloat(127-p)*row; (PRPitch.isBlack(p) ? NSColor.black.withAlphaComponent(0.18):NSColor.white.withAlphaComponent(0.018)).setFill();NSRect(x:g.minX,y:yy,width:g.width,height:row).fill(); if p%12==0{NSColor.white.withAlphaComponent(0.09).setStroke();let b=NSBezierPath();b.move(to:NSPoint(x:g.minX,y:yy));b.line(to:NSPoint(x:g.maxX,y:yy));b.stroke()} }
        let step=max(UInt64(1),snapFrames);var f:UInt64=0;while f<=visibleFrames{let xx=x(f);NSColor.white.withAlphaComponent(f%(step*4)==0 ? 0.16:0.06).setStroke();let b=NSBezierPath();b.move(to:NSPoint(x:xx,y:ruler));b.line(to:NSPoint(x:xx,y:g.maxY));b.stroke();f += step}
        NSColor(white:0.08,alpha:1).setFill();NSRect(x:0,y:0,width:keyboard,height:bounds.height-velocity).fill();NSRect(x:keyboard,y:0,width:bounds.width-keyboard,height:ruler).fill()
        for p in 0...127 { let yy=ruler+CGFloat(127-p)*row;if yy>g.maxY{continue};let black=PRPitch.isBlack(p);(black ? NSColor(white:0.10,alpha:1):NSColor(white:0.72,alpha:1)).setFill();NSRect(x:0,y:yy,width:black ? 34:50,height:row-1).fill();if p%12==0{PRPitch.name(UInt8(p)).draw(at:NSPoint(x:4,y:yy),withAttributes:[.font:NSFont.systemFont(ofSize:8),.foregroundColor:black ? NSColor.white:NSColor.black])}}
        for (i,n) in notes.enumerated(){let r=noteRect(n);(selected.contains(i) ? DAWDesignTokens.Color.mint:DAWDesignTokens.Color.accent).setFill();NSBezierPath(roundedRect:r,xRadius:3,yRadius:3).fill();if r.width>28{PRPitch.name(n.pitch).draw(at:NSPoint(x:r.minX+4,y:r.minY),withAttributes:[.font:NSFont.systemFont(ofSize:9,weight:.semibold),.foregroundColor:NSColor.white])}}
        let vy=bounds.height-velocity;DAWDesignTokens.Color.surface.setFill();NSRect(x:0,y:vy,width:bounds.width,height:velocity).fill();for(i,n)in notes.enumerated(){let xx=x(n.startFrames),h=CGFloat(n.velocity)/127*(velocity-18);(selected.contains(i) ? DAWDesignTokens.Color.mint:DAWDesignTokens.Color.accent.withAlphaComponent(0.75)).setFill();NSRect(x:xx,y:bounds.height-h-4,width:3,height:h).fill()}
        "VELOCITY".draw(at:NSPoint(x:8,y:vy+5),withAttributes:[.font:DAWDesignTokens.Typography.caption,.foregroundColor:DAWDesignTokens.Color.secondaryText])
    }
    override func mouseDown(with e:NSEvent){guard enabled else{return};window?.makeFirstResponder(self);let p=convert(e.locationInWindow,from:nil);if p.y>=bounds.height-velocity{return};if let i=notes.indices.reversed().first(where:{noteRect(notes[$0]).contains(p)}){if !e.modifierFlags.contains(.shift){selected=IndexSet(integer:i)}else{selected.insert(i)};dragStart=p;original=notes;dragIndex=i}else{selected.removeAll();if e.clickCount==2,p.x>keyboard,p.y>ruler{let raw=frame(p.x),s=snapFrames>0 ? (raw/snapFrames)*snapFrames:raw;var n=PianoRollNote(startFrames:s,lengthFrames:max(480,snapFrames),pitch:pitch(p.y),channel:0,velocity:100);if n.startFrames+n.lengthFrames>visibleFrames{n.lengthFrames=max(1,visibleFrames-n.startFrames)};notes.append(n);selected=IndexSet(integer:notes.count-1);onChange?(notes)}}}
    override func mouseDragged(with e:NSEvent){guard enabled,let start=dragStart,!selected.isEmpty else{return};let p=convert(e.locationInWindow,from:nil),df=Int64(frame(p.x))-Int64(frame(start.x)),dp=Int(pitch(p.y))-Int(pitch(start.y));notes=original.enumerated().map{idx,n in guard selected.contains(idx)else{return n};var c=n;let ns=max(Int64(0),min(Int64(visibleFrames-c.lengthFrames),Int64(n.startFrames)+df));c.startFrames=UInt64(ns);c.pitch=UInt8(max(0,min(127,Int(n.pitch)+dp)));return c}}
    override func mouseUp(with e:NSEvent){if dragStart != nil,notes != original{onChange?(notes)};dragStart=nil;original=[];dragIndex=nil}
    override func keyDown(with e:NSEvent){guard enabled else{return};let flags=e.modifierFlags.intersection(.deviceIndependentFlagsMask);if flags.contains(.command),e.charactersIgnoringModifiers?.lowercased()=="a"{selected=IndexSet(integersIn:0..<notes.count);return};if e.keyCode==51||e.keyCode==117{deleteSelection();return};if e.keyCode==123||e.keyCode==124||e.keyCode==125||e.keyCode==126{let semitone=e.keyCode==126 ? 1:e.keyCode==125 ? -1:0;let delta:Int64=e.keyCode==124 ? Int64(snapFrames):e.keyCode==123 ? -Int64(snapFrames):0;notes=notes.enumerated().map{i,n in guard selected.contains(i)else{return n};var c=n;c.pitch=UInt8(max(0,min(127,Int(n.pitch)+semitone)));c.startFrames=UInt64(max(0,min(Int64(visibleFrames-c.lengthFrames),Int64(n.startFrames)+delta)));return c};onChange?(notes);return};super.keyDown(with:e)}
    func deleteSelection(){guard !selected.isEmpty else{return};notes=notes.enumerated().filter{!selected.contains($0.offset)}.map(\.element);selected.removeAll();onChange?(notes)}
}

@MainActor
final class PianoRollEditorView:NSView {
    var notes:[PianoRollNote]=[]{didSet{canvas.notes=notes}}
    var clips:[PianoRollClipModel]=[]{didSet{reloadClips()}}
    var selectedClip:Int?{didSet{syncClipSelection()}}
    var editorEnabled=false{didSet{canvas.enabled=editorEnabled;applyEnabled()}}
    var onClipSelect:((Int)->Void)?,onNotesChange:(([PianoRollNote])->Void)?,onAddNote:(()->Void)?,onRemoveNote:((Int)->Void)?,onAddClip:(()->Void)?,onRemoveClip:((Int)->Void)?
    private let clipPopup=NSPopUpButton(),grid=NSPopUpButton(),canvas=PRCanvas(),zoom=NSSlider(value:1,minValue:0.5,maxValue:4,target:nil,action:nil),addClip=NSButton(title:"+ Клип",target:nil,action:nil),removeClip=NSButton(title:"− Клип",target:nil,action:nil),quantize=NSButton(title:"Quantize",target:nil,action:nil),legato=NSButton(title:"Legato",target:nil,action:nil)
    override init(frame:NSRect){super.init(frame:frame);wantsLayer=true;layer?.backgroundColor=DAWDesignTokens.Color.raisedSurface.cgColor;setup()}
    required init?(coder:NSCoder){fatalError()}
    private func setup(){clipPopup.target=self;clipPopup.action=#selector(selectClip);grid.addItems(withTitles:["1/4","1/8","1/16","1/32"]);grid.selectItem(at:2);grid.target=self;grid.action=#selector(changeGrid);zoom.target=self;zoom.action=#selector(changeZoom);zoom.isContinuous=true;addClip.target=self;addClip.action=#selector(addClipNow);removeClip.target=self;removeClip.action=#selector(removeClipNow);quantize.target=self;quantize.action=#selector(quantizeNow);legato.target=self;legato.action=#selector(legatoNow);for b in [addClip,removeClip,quantize,legato]{b.bezelStyle = .texturedRounded;b.font=DAWDesignTokens.Typography.caption};canvas.onChange={ [weak self] n in self?.notes=n;self?.onNotesChange?(n)};let top=NSStackView(views:[clipPopup,addClip,removeClip,grid,quantize,legato,NSTextField(labelWithString:"Zoom"),zoom]);top.spacing=6;top.alignment=.centerY;let stack=NSStackView(views:[top,canvas]);stack.orientation=.vertical;stack.alignment=.width;stack.spacing=6;stack.translatesAutoresizingMaskIntoConstraints=false;addSubview(stack);NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo:leadingAnchor,constant:6),stack.trailingAnchor.constraint(equalTo:trailingAnchor,constant:-6),stack.topAnchor.constraint(equalTo:topAnchor,constant:6),stack.bottomAnchor.constraint(equalTo:bottomAnchor,constant:-6),canvas.heightAnchor.constraint(greaterThanOrEqualToConstant:360)]);setAccessibilityLabel("Пианоролл MIDI-клипа");applyEnabled()}
    private func reloadClips(){clipPopup.removeAllItems();clips.forEach{clipPopup.addItem(withTitle:$0.title)};syncClipSelection();applyEnabled()}
    private func syncClipSelection(){if let s=selectedClip,let i=clips.firstIndex(where:{$0.index==s}){clipPopup.selectItem(at:i)}}
    private func applyEnabled(){let on=editorEnabled && !clips.isEmpty;clipPopup.isEnabled=on;addClip.isEnabled=editorEnabled;removeClip.isEnabled=on;quantize.isEnabled=on;legato.isEnabled=on;canvas.enabled=on}
    @objc private func selectClip(){let i=clipPopup.indexOfSelectedItem;guard clips.indices.contains(i)else{return};onClipSelect?(clips[i].index)}
    @objc private func addClipNow(){onAddClip?()};@objc private func removeClipNow(){if let i=selectedClip{onRemoveClip?(i)}}
    @objc private func changeGrid(){canvas.snapFrames=[48_000,24_000,12_000,6_000][max(0,grid.indexOfSelectedItem)]}
    @objc private func changeZoom(){canvas.zoom=CGFloat(zoom.doubleValue)}
    @objc private func quantizeNow(){let s=max(UInt64(1),canvas.snapFrames);notes=notes.map{n in var c=n;let q=Double(n.startFrames)/Double(s);c.startFrames=UInt64(q.rounded())*s;return c};canvas.notes=notes;onNotesChange?(notes)}
    @objc private func legatoNow(){let ordered=notes.indices.sorted{notes[$0].startFrames<notes[$1].startFrames};for k in 0..<ordered.count{let i=ordered[k];if let j=ordered.dropFirst(k+1).first(where:{notes[$0].pitch==notes[i].pitch && notes[$0].channel==notes[i].channel}){let end=notes[j].startFrames;if end>notes[i].startFrames{notes[i].lengthFrames=min(PRLimits.noteFrames,end-notes[i].startFrames)}}};canvas.notes=notes;onNotesChange?(notes)}
    static func noteName(_ pitch:UInt8)->String{PRPitch.name(pitch)}
    static func parsePitch(_ raw:String)->UInt8?{PRPitch.parse(raw)}
}
