import AppKit

@MainActor
final class DraftCanvas: NSView { override var isFlipped: Bool { true } }

@MainActor
final class AutomationSlider: NSSlider {
    var automationBegin: (() -> Void)?
    var automationEnd: (() -> Void)?
    override func mouseDown(with event: NSEvent) {
        automationBegin?()
        super.mouseDown(with: event)
        automationEnd?()
    }
}

@MainActor
/// Маркер-локатор проекта (POD daw_marker): позиция на общей шкале 48 кГц и имя.
struct ProjectMarker {
    let frame: UInt64
    let name: String
}

final class TimelineRulerView: NSView {
    var projectFrames: UInt64 = 48000 * 12 { didSet { needsDisplay = true } }
    var playhead: UInt64 = 0 { didSet { needsDisplay = true } }
    /// Тактовые метки из темпо-карты проекта; пустой массив — прежняя секундная шкала без второго слоя.
    var barMarks: [ProjectBarStart] = [] { didSet { needsDisplay = true } }
    /// Маркеры-локаторы: отдельная верхняя полоса флажков (домен v18, мост v1).
    var markers: [ProjectMarker] = [] { didSet { needsDisplay = true } }
    var onMarkerSeek: ((UInt64) -> Void)?
    var onMarkerAdd: ((UInt64) -> Void)?
    var onMarkerMenu: ((ProjectMarker) -> NSMenu?)?
    /// Высота верхней полосы под номера тактов; секундная шкала остаётся ниже.
    static let barBand: CGFloat = 14
    /// Полоса флажков маркеров над тактовой полосой.
    static let markerBand: CGFloat = 16
    override var isFlipped: Bool { true }
    override func draw(_ dirtyRect: NSRect) {
        NSColor(white: 0.07, alpha: 1).setFill();bounds.fill();let frames=max(UInt64(1),projectFrames);let seconds=Double(frames)/48000;let raw=max(1,seconds/Double(max(1,Int(bounds.width/110))));let step:Double=raw <= 1 ? 1:(raw <= 2 ? 2:(raw <= 5 ? 5:10));let attributes:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedDigitSystemFont(ofSize:10,weight:.regular),.foregroundColor:NSColor.secondaryLabelColor]
        // Второй слой: номера тактов из темпо-карты в верхней полосе. Горизонтальный
        // маппинг кадр↔пиксель тот же, что у секундной шкалы; меняется только то,
        // что метки приходят из карты, а не из локального темпа.
        // Верхняя полоса: флажки маркеров с именами и вертикальная линия на всю линейку.
        if !markers.isEmpty {
            NSColor(white:1,alpha:0.05).setFill();NSBezierPath(rect:NSRect(x:0,y:0,width:bounds.width,height:Self.markerBand)).fill()
            let markerAttributes:[NSAttributedString.Key:Any]=[.font:NSFont.systemFont(ofSize:9,weight:.medium),.foregroundColor:NSColor.systemYellow.withAlphaComponent(0.92)]
            for marker in markers {
                if marker.frame > frames { continue }
                let x=CGFloat(Double(marker.frame)/Double(frames))*bounds.width
                NSColor.systemYellow.withAlphaComponent(0.45).setStroke();let drop=NSBezierPath();drop.move(to:NSPoint(x:x,y:7));drop.line(to:NSPoint(x:x,y:bounds.height));drop.lineWidth=1;drop.stroke()
                NSColor.systemYellow.setFill();let flag=NSBezierPath();flag.move(to:NSPoint(x:x-4,y:0));flag.line(to:NSPoint(x:x+4,y:0));flag.line(to:NSPoint(x:x,y:7));flag.close();flag.fill()
                marker.name.draw(at:NSPoint(x:x+6,y:6),withAttributes:markerAttributes)
            }
        }
        if !barMarks.isEmpty {
            let barAttributes:[NSAttributedString.Key:Any]=[.font:NSFont.monospacedDigitSystemFont(ofSize:9,weight:.medium),.foregroundColor:NSColor.systemMint.withAlphaComponent(0.86)]
            NSColor(white:1,alpha:0.08).setFill();NSBezierPath(rect:NSRect(x:0,y:Self.markerBand,width:bounds.width,height:Self.barBand)).fill()
            NSColor(white:1,alpha:0.14).setStroke();let divider=NSBezierPath();divider.move(to:NSPoint(x:0,y:Self.markerBand+Self.barBand));divider.line(to:NSPoint(x:bounds.width,y:Self.markerBand+Self.barBand));divider.stroke()
            var lastLabelX:CGFloat = -60
            for mark in barMarks {
                if mark.frame > frames { break }
                let x=CGFloat(Double(mark.frame)/Double(frames))*bounds.width
                NSColor(white:1,alpha:0.26).setStroke();let tick=NSBezierPath();tick.move(to:NSPoint(x:x,y:Self.markerBand+Self.barBand-9));tick.line(to:NSPoint(x:x,y:Self.markerBand+Self.barBand));tick.stroke()
                guard x-lastLabelX >= 20 else { continue }
                lastLabelX=x
                String(mark.number).draw(at:NSPoint(x:x+3,y:Self.markerBand+1),withAttributes:barAttributes)
            }
        }
        let baseline=Self.markerBand+Self.barBand
        var second:Double=0;while second<=seconds {let x=CGFloat(second/seconds)*bounds.width;NSColor(white:1,alpha:0.16).setStroke();let line=NSBezierPath();line.move(to:NSPoint(x:x,y:bounds.height-8));line.line(to:NSPoint(x:x,y:bounds.height));line.stroke();String(format:"%.0f",second).draw(at:NSPoint(x:x+3,y:baseline+2),withAttributes:attributes);second+=step};let cursor=CGFloat(Double(min(playhead,frames))/Double(frames))*bounds.width;NSColor.systemMint.setStroke();let cursorLine=NSBezierPath();cursorLine.move(to:NSPoint(x:cursor,y:0));cursorLine.line(to:NSPoint(x:cursor,y:bounds.height));cursorLine.stroke()
    }
}

