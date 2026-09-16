import AppKit

struct ClipGeometry {
    var start: UInt64
    var sourceOffset: UInt64
    var length: UInt64
    var fadeIn: UInt64
    var fadeOut: UInt64
    var takeIndex: UInt32 = 0
    var sourceFramesForTake: UInt64 = 0
    var sourcePeaks: [Float] = []
    var color: UInt32 = 0
    var gainDb: Double = 0
    var muted: Bool = false
    var looped: Bool = false
    var pan: Double = 0
}

/// Durable 24-bit RGB из домена (0xRRGGBB); 0 означает «цвета нет» и
/// оставляет акцент дорожки.
func dawColorFromHex(_ hex: UInt32) -> NSColor {
    NSColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255.0, green: CGFloat((hex >> 8) & 0xFF) / 255.0, blue: CGFloat(hex & 0xFF) / 255.0, alpha: 1)
}

@MainActor
final class TakeLaneView:NSView {
    var title="";var peaks:[Float]=[];var takeStart:UInt64=0;var takeFrames:UInt64=0;var projectFrames:UInt64=1;var selected=false;var onSelect:(()->Void)?
    override var isFlipped:Bool{true}
    override init(frame:NSRect){super.init(frame:frame);setAccessibilityElement(true);setAccessibilityRole(.button)}
    required init?(coder:NSCoder){fatalError("init(coder:) is unavailable")}
    override func mouseDown(with event:NSEvent){onSelect?()}
    override func draw(_ dirtyRect:NSRect){super.draw(dirtyRect);(selected ? NSColor.systemPurple.withAlphaComponent(0.18):NSColor(white:1,alpha:0.025)).setFill();NSBezierPath(roundedRect:bounds,xRadius:6,yRadius:6).fill();let attrs:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:10,weight:.medium),.foregroundColor:NSColor.secondaryLabelColor];(title as NSString).draw(at:NSPoint(x:10,y:5),withAttributes:attrs);guard !peaks.isEmpty,projectFrames>0 else{return};let lane=NSRect(x:105,y:6,width:max(1,bounds.width-115),height:max(1,bounds.height-12));let x0=lane.minX+lane.width*CGFloat(Double(takeStart)/Double(projectFrames));let width=lane.width*CGFloat(Double(takeFrames)/Double(projectFrames));let maximum=max(0.000001,peaks.max() ?? 0);let wave=NSBezierPath();for(i,peak)in peaks.enumerated(){let x=x0+width*CGFloat(Double(i)/Double(peaks.count));let h=CGFloat(peak/maximum)*lane.height*0.42;wave.move(to:NSPoint(x:x,y:lane.midY-h));wave.line(to:NSPoint(x:x,y:lane.midY+h))};(selected ? NSColor.systemPurple:NSColor.systemPurple.withAlphaComponent(0.55)).setStroke();wave.stroke()}
}

@MainActor
final class WaveformView: NSView {
    var peaks: [Float] = []
    var clips: [ClipGeometry] = []
    var automationPoints: [(frame: UInt64, gain: Double)] = []
    var panAutomationPoints: [(frame: UInt64, value: Double)] = []
    var selectedIndex: Int = 0
    /// Мультиселект: все индексы группы (primary остаётся selectedIndex).
    var selectedIndices: [Int] = []
    var sourceFrames: UInt64 = 0
    var playableFrames: UInt64 = 0
    var snapFrames: UInt64 = 12000
    /// Beat-grid квант для произвольного кадра: (опора — кадр ближайшей темпо-точки назад от
    /// позиции, размер деления в кадрах). Пиксель↔кадр маппинг не тронут — заменён только
    /// источник кванта; nil оставляет прежнюю сетку от нуля таймлайна.
    var snapGrid: ((Int64) -> (anchor: UInt64, quantum: UInt64))?
    var rangeStart: UInt64? { didSet { needsDisplay = true } }
    var rangeEnd: UInt64? { didSet { needsDisplay = true } }
    var loopEnabled = false { didSet { needsDisplay = true } }
    var trackAccent: NSColor = .systemBlue { didSet { needsDisplay = true } }
    var showsEmbeddedRuler = false { didSet { needsDisplay = true } }
    var onEditBegin: (() -> Void)?
    var onEdit: ((Int, UInt64, UInt64, UInt64) -> Void)?
    var onFadeEdit: ((Int, UInt64, UInt64) -> Void)?
    /// Контекстное меню клипа по правой кнопке: контроллер собирает действия
    /// (цвет, громкость, дубль, split, delete) для выбранного индекса.
    var onClipMenu: ((Int) -> NSMenu?)?
    /// Горячие клавиши редактора клипов в фокусе волны: "s", "d", "delete".
    var onClipHotkey: ((String) -> Void)?
    var onSelect: ((Int, Bool) -> Void)?
    /// Option+стрелки: сдвинуть всю группу на шаг сетки (-1/ +1).
    var onNudge: ((Int) -> Void)?
    private var gesture: (kind: Int, x: CGFloat, start: UInt64, offset: UInt64, length: UInt64, fadeIn: UInt64, fadeOut: UInt64)?
    var projectFrames: UInt64 = 0
    var playhead: UInt64 = 0 { didSet { needsDisplay = true } }
    var onSeek: ((UInt64) -> Void)?
    var onToggle: (() -> Void)?
    private enum HitZone: Int { case none, body, trimStart, trimEnd, fadeIn, fadeOut }
    private var hoveredIndex: Int?
    private var hoveredZone: HitZone = .none
    private var tracking: NSTrackingArea?
    /// Drag-and-drop of audio files from Finder onto the lane: the controller
    /// receives the file plus the drop-frame and returns whether it accepted it.
    var onDropFile: ((URL, UInt64) -> Bool)?
    private func audioFileURLs(_ sender: NSDraggingInfo) -> [URL] {
        guard let listed=sender.draggingPasteboard.readObjects(forClasses:[NSURL.self],options:nil) as? [URL] else { return [] }
        return listed.filter{ ["wav","aif","aiff","aifc"].contains($0.pathExtension.lowercased()) }
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation { audioFileURLs(sender).isEmpty ? [] : .copy }
    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation { draggingEntered(sender) }
    override func prepareForDragOperation(_ sender: NSDraggingInfo) -> Bool { !audioFileURLs(sender).isEmpty }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let url=audioFileURLs(sender).first else { return false }
        let point=convert(sender.draggingLocation,from:nil)
        let ratio=min(1.0,max(0.0,Double((point.x-lane.minX)/lane.width)))
        return onDropFile?(url,UInt64(Double(projectFrames)*ratio)) ?? false
    }
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
        setAccessibilityElement(true)
        setAccessibilityRole(.slider)
        setAccessibilityLabel("Позиция на аудиоволне")
        setAccessibilityHelp("Клик — выбрать позицию. Стрелки — одна секунда. Пробел — воспроизведение или стоп. S — разделить, D — дублировать, C — копировать, V — вставить у курсора, M — мьют клипа, L — луп клипа, Delete — удалить выбранный клип или группу. Ctrl-клик добавляет и убирает клип из группы, Option+стрелки сдвигают группу на шаг сетки. Правая кнопка — меню клипа. Перетаскивание WAV/AIFF из Finder — импорт клипа в дорожку по месту отпускания.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }
    private var lane: NSRect { NSRect(x: showsEmbeddedRuler ? 14:0, y: showsEmbeddedRuler ? 25:4, width: max(1, bounds.width - (showsEmbeddedRuler ? 28:0)), height: max(1, bounds.height - (showsEmbeddedRuler ? 40:8))) }
    private func clipRect(_ clip: ClipGeometry) -> NSRect {
        let x = lane.minX + lane.width * CGFloat(Double(clip.start) / Double(max(1, projectFrames)))
        let width = max(2, lane.width * CGFloat(Double(clip.length) / Double(max(1, projectFrames))))
        return NSRect(x: x, y: lane.minY + 7, width: width, height: max(1, lane.height - 14))
    }
    private func hitZone(for index: Int, at point: NSPoint) -> HitZone {
        let clip = clips[index], rect = clipRect(clip)
        guard rect.insetBy(dx: -8, dy: -6).contains(point) else { return .none }
        let fadeWidth = max(10, min(28, rect.width * 0.22))
        if point.x <= rect.minX + 7 { return .trimStart }
        if point.x >= rect.maxX - 7 { return .trimEnd }
        if point.y >= rect.maxY - 13 && point.x <= rect.minX + fadeWidth { return .fadeIn }
        if point.y >= rect.maxY - 13 && point.x >= rect.maxX - fadeWidth { return .fadeOut }
        return rect.contains(point) ? .body : .none
    }
    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let area = NSTrackingArea(rect: bounds, options: [.activeInKeyWindow, .inVisibleRect, .mouseMoved], owner: self, userInfo: nil)
        addTrackingArea(area); tracking = area
    }
    override func mouseMoved(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        let index = clips.indices.last { clipRect(clips[$0]).insetBy(dx: -8, dy: -6).contains(point) }
        let zone = index.map { hitZone(for: $0, at: point) } ?? .none
        if index != hoveredIndex || zone != hoveredZone { hoveredIndex = index; hoveredZone = zone; needsDisplay = true }
        switch zone { case .trimStart, .trimEnd: NSCursor.resizeLeftRight.set(); case .fadeIn, .fadeOut: NSCursor.crosshair.set(); case .body: NSCursor.openHand.set(); case .none: NSCursor.arrow.set() }
    }
    private func snap(_ value:Int64,_ event:NSEvent)->Int64 {
        guard !event.modifierFlags.contains(.shift), snapFrames > 0 else { return value }
        if let snapGrid {
            // Сетка считается от темпо-точки, которой принадлежит позиция (tempo map last-holds),
            // а не от нуля таймлайна: при смене темпа деления едут вместе с точкой.
            let (anchor, quantum) = snapGrid(value)
            guard quantum > 0 else { return value }
            let base = Int64(anchor)
            let steps = (Double(max(0, value - base))/Double(quantum)).rounded()
            return base + Int64(steps*Double(quantum))
        }
        return Int64((Double(value)/Double(snapFrames)).rounded())*Int64(snapFrames)
    }
    private func seek(_ frame: UInt64) { playhead = min(frame, playableFrames); onSeek?(playhead) }
    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let point = convert(event.locationInWindow, from: nil)
        guard !clips.isEmpty else { return }
        let additive = event.modifierFlags.contains(.control)
        if let hit = clips.indices.last(where: { hitZone(for: $0, at: point) != .none }) {
            if additive { onSelect?(hit, true); needsDisplay = true; return } // toggle остаётся контроллеру, жест не стартует
            selectedIndex = hit; onSelect?(hit, false); needsDisplay = true
        }
        let clip=clips[selectedIndex]
        let zone = hitZone(for: selectedIndex, at: point)
        var kind = 0
        if zone == .trimStart { kind = 2 }
        else if zone == .trimEnd { kind = 3 }
        else if zone == .body { kind = 1 }
        else if zone == .fadeIn { kind = 4 }
        else if zone == .fadeOut { kind = 5 }
        if kind != 0 {
            gesture = (kind, point.x, clip.start, clip.sourceOffset, clip.length, clip.fadeIn, clip.fadeOut)
            onEditBegin?()
        } else {
            let fraction = min(1, max(0, (point.x - lane.minX) / lane.width))
            seek(UInt64((fraction * Double(projectFrames)).rounded()))
        }
    }
    override func mouseDragged(with event: NSEvent) {
        guard let g = gesture else {
            let point = convert(event.locationInWindow, from: nil)
            seek(UInt64((min(1, max(0, (point.x - lane.minX) / lane.width)) * Double(projectFrames)).rounded()))
            return
        }
        let point = convert(event.locationInWindow, from: nil)
        let delta = Int64(((point.x - g.x) / lane.width * Double(projectFrames)).rounded())
        var clip=clips[selectedIndex]
        switch g.kind {
        case 1:
            clip.start = UInt64(max(0, min(Int64(48000 * 600 - g.length), snap(Int64(g.start)+delta,event))))
        case 2:
            let shift = max(-Int64(min(g.start, g.offset)), min(Int64(g.length) - 1, snap(Int64(g.start)+delta,event)-Int64(g.start)))
            clip.start = UInt64(Int64(g.start) + shift); clip.sourceOffset = UInt64(Int64(g.offset) + shift); clip.length = UInt64(Int64(g.length) - shift)
        case 3:
            let snappedLength=snap(Int64(g.start+g.length)+delta,event)-Int64(g.start)
            let available=(clip.sourceFramesForTake > 0 ? clip.sourceFramesForTake:sourceFrames)-g.offset
            clip.length = UInt64(max(1, min(Int64(min(available, 48000 * 600 - g.start)), snappedLength)))
            clip.fadeIn = min(g.fadeIn, clip.length)
            clip.fadeOut = min(g.fadeOut, clip.length - clip.fadeIn)
        case 4:
            let frame = snap(Int64(g.start + g.fadeIn) + delta, event) - Int64(g.start)
            clip.fadeIn = UInt64(max(0, min(Int64(g.length - g.fadeOut), frame)))
        default:
            let frame = Int64(g.start + g.length) - snap(Int64(g.start + g.length - g.fadeOut) + delta, event)
            clip.fadeOut = UInt64(max(0, min(Int64(g.length - g.fadeIn), frame)))
        }
        clips[selectedIndex]=clip
        needsDisplay = true
    }
    override func mouseUp(with event: NSEvent) {
        guard let g = gesture else { return }; gesture = nil
        let clip=clips[selectedIndex]
        if g.kind == 4 || g.kind == 5 {
            if clip.fadeIn != g.fadeIn || clip.fadeOut != g.fadeOut { onFadeEdit?(selectedIndex, clip.fadeIn, clip.fadeOut) }
        } else if clip.start != g.start || clip.sourceOffset != g.offset || clip.length != g.length { onEdit?(selectedIndex,clip.start,clip.sourceOffset,clip.length) }
    }
    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53, let g = gesture {
            gesture = nil; let current=clips[selectedIndex]; clips[selectedIndex]=ClipGeometry(start:g.start,sourceOffset:g.offset,length:g.length,fadeIn:g.fadeIn,fadeOut:g.fadeOut,takeIndex:current.takeIndex,sourceFramesForTake:current.sourceFramesForTake,sourcePeaks:current.sourcePeaks,color:current.color,gainDb:current.gainDb,muted:current.muted,looped:current.looped,pan:current.pan); needsDisplay = true; return
        }
        // Редакторские горячие клавиши работают только без модификаторов и при
        // пустом тексте ввода: Cmd+S остаётся «Сохранить» в главном меню,
        // а ⌥S/⌥M/⌥A принадлежат меню дорожек.
        if gesture == nil, !event.modifierFlags.contains(.command),
           !event.modifierFlags.contains(.option), !event.modifierFlags.contains(.control),
           !clips.isEmpty {
            if event.keyCode == 51 || event.keyCode == 117 { onClipHotkey?("delete"); return }
            switch event.charactersIgnoringModifiers?.lowercased() {
            case "s": onClipHotkey?("s"); return
            case "d": onClipHotkey?("d"); return
            case "c": onClipHotkey?("c"); return
            case "v": onClipHotkey?("v"); return
            case "m": onClipHotkey?("m"); return
            case "l": onClipHotkey?("l"); return
            default: break
            }
        }
        if gesture == nil, event.modifierFlags.contains(.option), (event.keyCode == 123 || event.keyCode == 124), !clips.isEmpty { onNudge?(event.keyCode == 124 ? 1 : -1); return }
        switch event.keyCode {
        case 123: seek(playhead > 48000 ? playhead - 48000 : 0)
        case 124: seek(min(projectFrames, playhead + 48000))
        case 115: seek(0)
        case 119: seek(playableFrames)
        case 49: onToggle?()
        default: super.keyDown(with: event)
        }
    }
    override func rightMouseDown(with event: NSEvent) {
        let point = convert(event.locationInWindow, from: nil)
        guard let hit = clips.indices.last(where: { hitZone(for: $0, at: point) != .none }) else { return }
        if hit != selectedIndex { selectedIndex = hit; onSelect?(hit,false); needsDisplay = true }
        guard let menu = onClipMenu?(hit) else { return }
        NSMenu.popUpContextMenu(menu, with: event, for: self)
    }
    override func accessibilityValue() -> Any? { String(format: "%.2f секунд", Double(playhead) / 48000) }
    override func accessibilityPerformIncrement() -> Bool { seek(min(projectFrames, playhead + 48000)); return true }
    override func accessibilityPerformDecrement() -> Bool { seek(playhead > 48000 ? playhead - 48000 : 0); return true }
    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        NSColor(calibratedWhite: 0.055, alpha: 1).setFill();bounds.fill()
        guard projectFrames > 0 else { return }
        if showsEmbeddedRuler {let ruler = NSRect(x: 0, y: 0, width: bounds.width, height: lane.minY - 1);NSColor(calibratedWhite: 0.095, alpha: 1).setFill(); ruler.fill();NSColor(white: 1, alpha: 0.10).setStroke(); let separator = NSBezierPath(); separator.move(to: NSPoint(x: 0, y: lane.minY - 0.5)); separator.line(to: NSPoint(x: bounds.maxX, y: lane.minY - 0.5)); separator.stroke()}
        let laneGradient = NSGradient(colors: [NSColor(calibratedWhite: 0.075, alpha: 1), NSColor(calibratedWhite: 0.045, alpha: 1)])
        laneGradient?.draw(in: lane, angle: 90)
        let duration = Double(projectFrames) / 48000
        let tickStep = duration > 30 ? 10.0 : (duration > 10 ? 5.0 : (duration > 3 ? 1.0 : 0.5))
        if snapFrames > 0 {
            var gridStep=snapFrames
            while projectFrames/gridStep > 256 { gridStep *= 2 }
            NSColor(white:1,alpha:0.025).setStroke()
            var frame:UInt64=0
            while frame<=projectFrames {
                let x=lane.minX+lane.width*CGFloat(Double(frame)/Double(projectFrames));let line=NSBezierPath();line.move(to:NSPoint(x:x,y:lane.minY));line.line(to:NSPoint(x:x,y:lane.maxY));line.stroke()
                if projectFrames-frame<gridStep { break };frame+=gridStep
            }
        }
        for time in stride(from: 0.0, through: duration, by: tickStep) {
            let x = lane.minX + CGFloat(time / duration) * lane.width
            NSColor(white: 1, alpha: 0.12).setStroke()
            let line = NSBezierPath(); line.move(to: NSPoint(x: x, y: lane.minY)); line.line(to: NSPoint(x: x, y: lane.maxY)); line.stroke()
            if showsEmbeddedRuler {let notch = NSBezierPath(); notch.move(to: NSPoint(x: x, y: lane.minY - 1)); notch.line(to: NSPoint(x: x, y: lane.minY - 6)); notch.stroke();let attributes:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedDigitSystemFont(ofSize:10,weight:.regular),.foregroundColor:NSColor.secondaryLabelColor];(String(format:"%05.1f",time) as NSString).draw(at:NSPoint(x:min(x+4,bounds.width-42),y:6),withAttributes:attributes)}
        }
        if let rangeStart, let rangeEnd, rangeEnd > rangeStart {
            let x = lane.minX + lane.width * CGFloat(Double(rangeStart) / Double(projectFrames))
            let width = lane.width * CGFloat(Double(rangeEnd - rangeStart) / Double(projectFrames))
            let rangeColor=loopEnabled ? NSColor.systemOrange:NSColor.systemBlue
            rangeColor.withAlphaComponent(loopEnabled ? 0.20:0.15).setFill(); NSRect(x: x, y: lane.minY, width: width, height: lane.height).fill()
            rangeColor.withAlphaComponent(0.85).setStroke(); let outline=NSBezierPath(rect:NSRect(x:x,y:lane.minY,width:width,height:lane.height)); outline.lineWidth=1; outline.stroke()
        }
        for (index,clip) in clips.enumerated() {
            let rect = clipRect(clip); let clipX=rect.minX; let clipWidth=rect.width
            let selected = index == selectedIndex || selectedIndices.contains(index); let hovered = index == hoveredIndex
            let body = clip.color != 0 ? dawColorFromHex(clip.color) : trackAccent
            if selected { NSColor.systemMint.withAlphaComponent(0.10).setFill(); NSBezierPath(roundedRect: rect.insetBy(dx: -3, dy: -3), xRadius: 7, yRadius: 7).fill() }
            (selected ? body.withAlphaComponent(clip.muted ? 0.30 : 0.82) : body.withAlphaComponent(clip.muted ? (hovered ? 0.20 : 0.12) : (hovered ? 0.48 : 0.30))).setFill()
            NSBezierPath(roundedRect:rect,xRadius:6,yRadius:6).fill()
            (selected ? NSColor.systemMint : NSColor(white:1,alpha:0.22)).withAlphaComponent(selected ? 0.95 : (hovered ? 0.55 : 0.25)).setStroke()
            let border = NSBezierPath(roundedRect: rect, xRadius: 6, yRadius: 6); border.lineWidth = selected ? 1.5 : 1; border.stroke()
            let clipPeaks=clip.sourcePeaks.isEmpty ? peaks:clip.sourcePeaks;let clipSourceFrames=clip.sourceFramesForTake>0 ? clip.sourceFramesForTake:sourceFrames;let maximum=max(0.000001,clipPeaks.max() ?? 0)
            let waveform=NSBezierPath(); waveform.lineWidth=1
            for (i,peak) in clipPeaks.enumerated() {
                let begin=Double(i)/Double(clipPeaks.count)*Double(clipSourceFrames); let end=Double(i+1)/Double(clipPeaks.count)*Double(clipSourceFrames)
                let visibleBegin=max(begin,Double(clip.sourceOffset)), visibleEnd=min(end,Double(clip.sourceOffset+clip.length)); guard visibleEnd>visibleBegin else { continue }
                let x=clipX+CGFloat((visibleBegin+visibleEnd)/2-Double(clip.sourceOffset))/CGFloat(clip.length)*clipWidth; let height=CGFloat(peak/maximum)*rect.height*0.43
                waveform.move(to:NSPoint(x:x,y:rect.midY-height)); waveform.line(to:NSPoint(x:x,y:rect.midY+height))
            }
            NSColor.systemMint.withAlphaComponent(selected ? 0.92 : 0.50).setStroke(); waveform.stroke()
            let handleColor = selected ? NSColor.systemMint : NSColor(white: 1, alpha: hovered ? 0.55 : 0.26)
            handleColor.setFill(); NSBezierPath(roundedRect: NSRect(x:rect.minX-2,y:rect.midY-12,width:4,height:24),xRadius:2,yRadius:2).fill(); NSBezierPath(roundedRect: NSRect(x:rect.maxX-2,y:rect.midY-12,width:4,height:24),xRadius:2,yRadius:2).fill()
            let fadeColor = selected ? NSColor.white.withAlphaComponent(0.76) : NSColor.white.withAlphaComponent(0.36); fadeColor.setStroke()
            let fadeInX = clip.fadeIn > 0 ? clipX+clipWidth*CGFloat(Double(clip.fadeIn)/Double(clip.length)) : clipX+min(20,clipWidth*0.18)
            let fadeOutX = clip.fadeOut > 0 ? clipX+clipWidth*CGFloat(Double(clip.length-clip.fadeOut)/Double(clip.length)) : clipX+clipWidth-min(20,clipWidth*0.18)
            let fadeInPath=NSBezierPath(); fadeInPath.move(to:NSPoint(x:clipX,y:rect.maxY)); fadeInPath.line(to:NSPoint(x:fadeInX,y:rect.minY)); fadeInPath.stroke()
            let fadeOutPath=NSBezierPath(); fadeOutPath.move(to:NSPoint(x:fadeOutX,y:rect.minY)); fadeOutPath.line(to:NSPoint(x:rect.maxX,y:rect.maxY)); fadeOutPath.stroke()
            if selected || hovered { let attrs:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:10,weight:.semibold),.foregroundColor:NSColor.white.withAlphaComponent(0.72)]; let tag="Клип \(index + 1)\(clip.looped ? " · ↻" : "")\(clip.muted ? " · MUTE" : "")\(clip.gainDb == 0 ? "" : String(format:" · %+.1f dB",clip.gainDb))\(clip.pan == 0 ? "" : String(format:" · P %@%d%%", clip.pan < 0 ? "L": "R", Int(abs(clip.pan) * 100 + 0.5)))"; (tag as NSString).draw(at:NSPoint(x:rect.minX+8,y:rect.minY+7),withAttributes:attrs) }
        }
        if !automationPoints.isEmpty {
            func automationY(_ gain:Double)->CGFloat{let visible=min(12,max(-60,gain));return lane.maxY-CGFloat((visible+60)/72)*lane.height}
            let curve=NSBezierPath();curve.lineWidth=1.5
            for(index,point)in automationPoints.enumerated(){let x=lane.minX+lane.width*CGFloat(Double(point.frame)/Double(projectFrames));let y=automationY(point.gain);if index==0{curve.move(to:NSPoint(x:lane.minX,y:y))}else{curve.line(to:NSPoint(x:x,y:y))};if index+1==automationPoints.count{curve.line(to:NSPoint(x:lane.maxX,y:y))}}
            NSColor.systemCyan.setStroke();curve.stroke();for point in automationPoints{let x=lane.minX+lane.width*CGFloat(Double(point.frame)/Double(projectFrames));let y=automationY(point.gain);NSColor.systemCyan.setFill();NSBezierPath(ovalIn:NSRect(x:x-3,y:y-3,width:6,height:6)).fill()}
        }
        if !panAutomationPoints.isEmpty {
            let curve=NSBezierPath();curve.lineWidth=1.25
            for(index,point)in panAutomationPoints.enumerated(){let x=lane.minX+lane.width*CGFloat(Double(point.frame)/Double(projectFrames));let y=lane.midY-CGFloat(min(1,max(-1,point.value)))*lane.height*0.42;if index==0{curve.move(to:NSPoint(x:lane.minX,y:y))}else{curve.line(to:NSPoint(x:x,y:y))};if index+1==panAutomationPoints.count{curve.line(to:NSPoint(x:lane.maxX,y:y))}}
            NSColor.systemPurple.setStroke();curve.stroke();for point in panAutomationPoints{let x=lane.minX+lane.width*CGFloat(Double(point.frame)/Double(projectFrames));let y=lane.midY-CGFloat(min(1,max(-1,point.value)))*lane.height*0.42;NSColor.systemPurple.setFill();NSBezierPath(ovalIn:NSRect(x:x-2.5,y:y-2.5,width:5,height:5)).fill()}
        }
        let cursorX = lane.minX + CGFloat(Double(min(playhead, projectFrames)) / Double(projectFrames)) * lane.width
        NSColor.white.setStroke()
        let cursor = NSBezierPath(); cursor.lineWidth = 1.5; cursor.move(to: NSPoint(x: cursorX, y: lane.minY)); cursor.line(to: NSPoint(x: cursorX, y: lane.maxY)); cursor.stroke()
        if window?.firstResponder === self {
            NSColor.keyboardFocusIndicatorColor.setStroke()
            let focus = NSBezierPath(roundedRect: bounds.insetBy(dx: 2, dy: 2), xRadius: 6, yRadius: 6); focus.lineWidth = 2; focus.stroke()
        }
    }
}
