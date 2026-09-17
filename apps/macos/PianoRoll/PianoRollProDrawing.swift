import AppKit

@MainActor
enum PRProDrawing {
    static let canvas = DAWDesignTokens.Color.canvas
    static let surface = DAWDesignTokens.Color.surface
    static let raised = DAWDesignTokens.Color.raisedSurface
    static let border = DAWDesignTokens.Color.border
    static let text = DAWDesignTokens.Color.text
    static let secondary = DAWDesignTokens.Color.secondaryText
    static let accent = DAWDesignTokens.Color.accent
    static let mint = DAWDesignTokens.Color.mint
    static let coral = DAWDesignTokens.Color.coral

    static func line(_ from: NSPoint, _ to: NSPoint,
                     color: NSColor, width: CGFloat = 1) {
        color.setStroke()
        let path = NSBezierPath()
        path.move(to: from); path.line(to: to)
        path.lineWidth = width
        path.stroke()
    }

    static func label(_ string: String, in rect: NSRect,
                      color: NSColor = text, size: CGFloat = 10,
                      weight: NSFont.Weight = .medium,
                      alignment: NSTextAlignment = .left) {
        guard rect.width > 1, rect.height > 1 else { return }
        let paragraph = NSMutableParagraphStyle()
        paragraph.alignment = alignment
        paragraph.lineBreakMode = .byTruncatingTail
        (string as NSString).draw(in: rect, withAttributes: [
            .font: NSFont.monospacedDigitSystemFont(ofSize: size, weight: weight),
            .foregroundColor: color,
            .paragraphStyle: paragraph
        ])
    }

    static func noteColor(_ note: PianoRollNote) -> NSColor {
        let base: NSColor
        switch note.channel % 6 {
        case 0: base = accent
        case 1: base = mint
        case 2: base = .systemBlue
        case 3: base = .systemOrange
        case 4: base = .systemPink
        default: base = .systemTeal
        }
        let darken = CGFloat(127 - Int(note.velocity)) / 310
        return base.blended(withFraction: darken, of: canvas) ?? base
    }

    static func noteRect(_ note: PianoRollNote, state: PRProState,
                         rows: PRPitchRows) -> NSRect? {
        guard let map = state.timeMap, let row = rows.row(for: note.pitch) else { return nil }
        let x = map.start(note) * state.pixelsPerBeat
        let width = max(3, map.length(note) * state.pixelsPerBeat)
        let y = Double(row) * state.rowHeight + 1
        return NSRect(x: x, y: y, width: width, height: max(4, state.rowHeight - 2))
    }

    static func drawGrid(in dirtyRect: NSRect, height: CGFloat,
                         state: PRProState, offsetX: CGFloat = 0) {
        guard let map = state.timeMap else { return }
        let nominal = state.grid.step(state.pixelsPerBeat)
        let visualStep = nominal * max(1, ceil(8 / max(0.001, nominal * state.pixelsPerBeat)))
        let leftBeat = max(0, Double(dirtyRect.minX + offsetX) / state.pixelsPerBeat)
        let rightBeat = min(map.durationBeats, Double(dirtyRect.maxX + offsetX) / state.pixelsPerBeat)
        var tick = floor((leftBeat + map.originBeat) / visualStep) * visualStep
        let limit = rightBeat + map.originBeat + visualStep
        var guardCount = 0
        while tick <= limit, guardCount < 20_000 {
            let relative = tick - map.originBeat
            let x = CGFloat(relative * state.pixelsPerBeat) - offsetX
            let wholeBeat = abs(tick.rounded() - tick) < 1e-7
            let color = border.withAlphaComponent(wholeBeat ? 0.55 : 0.22)
            line(NSPoint(x: x, y: dirtyRect.minY), NSPoint(x: x, y: min(height, dirtyRect.maxY)), color: color)
            tick += visualStep
            guardCount += 1
        }
    }

    static func drawPlayhead(state: PRProState, offsetX: CGFloat,
                             height: CGFloat) {
        guard let map = state.timeMap,
              state.playheadFrame >= map.clipStart,
              state.playheadFrame <= map.clipStart + map.clipLength else { return }
        let relativeFrame = state.playheadFrame - map.clipStart
        let x = CGFloat(map.beat(at: relativeFrame) * state.pixelsPerBeat) - offsetX
        line(NSPoint(x: x, y: 0), NSPoint(x: x, y: height), color: mint, width: 1.25)
    }
}
