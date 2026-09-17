import AppKit

struct MidiArrangementClip {
    let index: Int
    let start: UInt64
    let length: UInt64
    let color: NSColor
    let notes: [PianoRollNote]
}

/// Read-only arrangement projection of the existing MIDI clips, not a second
/// editable note model. Selection opens the existing dock editor and C ABI path.
@MainActor
final class MidiArrangementView: NSView {
    var trackID: UInt64 = 0
    var clips: [MidiArrangementClip] = []
    var projectFrames: UInt64 = 1
    var barFrames: [UInt64] = []
    var playhead: UInt64 = 0 { didSet { needsDisplay = true } }
    var cycleSelection: TimelineFrameRange? { didSet { needsDisplay = true } }
    var cycleEnabled = false { didSet { needsDisplay = true } }
    var title = "MIDI"
    var onSelect: ((Int, Bool) -> Void)?
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    private func x(_ frame: UInt64) -> CGFloat { bounds.width * CGFloat(Double(frame) / Double(max(1, projectFrames))) }
    func clipRect(_ clip: MidiArrangementClip) -> NSRect {
        NSRect(x: x(clip.start), y: 6, width: max(2, x(clip.length)), height: max(1, bounds.height - 12))
    }
    override func draw(_ dirtyRect: NSRect) {
        DAWDesignTokens.Color.canvas.setFill(); bounds.fill()
        DAWDesignTokens.Color.border.withAlphaComponent(0.5).setStroke()
        let grid = NSBezierPath()
        for frame in barFrames { let xx = x(frame); grid.move(to: NSPoint(x: xx, y: 0)); grid.line(to: NSPoint(x: xx, y: bounds.height)) }
        grid.stroke()
        if let range = cycleSelection {
            let tint = cycleEnabled ? NSColor.systemOrange : NSColor.systemBlue
            let rect = NSRect(x: x(range.start), y: 0, width: x(range.length), height: bounds.height)
            tint.withAlphaComponent(cycleEnabled ? 0.20 : 0.15).setFill(); rect.fill()
            tint.withAlphaComponent(0.85).setStroke(); NSBezierPath(rect: rect).stroke()
        }
        for clip in clips {
            let rect = clipRect(clip)
            guard rect.intersects(dirtyRect) else { continue }
            clip.color.withAlphaComponent(0.22).setFill(); NSBezierPath(roundedRect: rect, xRadius: 4, yRadius: 4).fill()
            clip.color.withAlphaComponent(0.8).setStroke(); NSBezierPath(roundedRect: rect.insetBy(dx: 0.5, dy: 0.5), xRadius: 4, yRadius: 4).stroke()
            NSGraphicsContext.saveGraphicsState(); NSBezierPath(rect: rect.insetBy(dx: 2, dy: 1)).addClip()
            ("\(title) · \(clip.index + 1)" as NSString).draw(at: NSPoint(x: rect.minX + 6, y: rect.minY + 4), withAttributes: [.font: NSFont.systemFont(ofSize: 10, weight: .medium), .foregroundColor: DAWDesignTokens.Color.text])
            let lowest = Int(clip.notes.map(\.pitch).min() ?? 48), highest = Int(clip.notes.map(\.pitch).max() ?? 72)
            let pitchRows = max(12, highest - lowest + 3)
            let height = max(1, rect.height - 23) / CGFloat(pitchRows)
            clip.color.withAlphaComponent(0.95).setFill()
            for note in clip.notes {
                let nx = rect.minX + x(note.startFrames)
                let ny = rect.maxY - 5 - CGFloat(Int(note.pitch) - lowest + 1) * height
                NSRect(x: nx, y: ny, width: max(2, x(note.lengthFrames)), height: max(1.5, min(3, height))).fill()
            }
            NSGraphicsContext.restoreGraphicsState()
        }
        DAWDesignTokens.Color.accent.setStroke()
        let cursor = NSBezierPath(); cursor.move(to: NSPoint(x: x(playhead), y: 0)); cursor.line(to: NSPoint(x: x(playhead), y: bounds.height)); cursor.stroke()
    }
    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        let point = convert(event.locationInWindow, from: nil)
        if let clip = clips.last(where: { clipRect($0).contains(point) }) { onSelect?(clip.index, event.clickCount >= 2) }
    }
}

extension DraftApp {
    func makeMidiArrangement(trackID: UInt64, title: String, color: NSColor, count: UInt32, duration: UInt64) -> MidiArrangementView {
        let view = MidiArrangementView(); view.title = title; view.trackID = trackID
        view.projectFrames = min(48000 * 600, max(48000 * 12, duration + 48000 * 2))
        view.barFrames = tempoBars.map(\.frame); view.playhead = playheadFrame
        for index in 0..<count {
            var meta = daw_midi_clip(); meta.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
            guard daw_get_midi_clip(session, trackID, index, &meta, 0, nil, 0, nil) == 0 else { continue }
            var notes: [PianoRollNote] = [], offset: UInt32 = 0
            while offset < meta.note_count {
                var page = [daw_midi_note](repeating: daw_midi_note(), count: Int(min(UInt32(DAW_MIDI_NOTES_PER_CALL), meta.note_count - offset)))
                var written: UInt32 = 0
                let result = page.withUnsafeMutableBufferPointer { daw_get_midi_clip(session, trackID, index, &meta, offset, $0.baseAddress, UInt32($0.count), &written) }
                guard result == 0, written > 0 else { break }
                notes += page.prefix(Int(written)).map { PianoRollNote(startFrames: $0.start, lengthFrames: $0.length, pitch: $0.pitch, channel: $0.channel, velocity: $0.velocity) }
                offset += written
            }
            view.clips.append(MidiArrangementClip(index: Int(index), start: meta.start, length: meta.length,
                                                  color: meta.color == 0 ? color : dawColorFromHex(UInt32(meta.color)), notes: notes))
        }
        view.onSelect = { [weak self] index, open in
            guard let self else { return }
            self.selectedMixerID = trackID; self.midiClipIndex = index; self.updateMixerInspector(trackID)
            if open { self.workspace?.selectDock(.midi) }
        }
        view.setAccessibilityLabel("MIDI-клипы дорожки \(title)")
        view.setAccessibilityHelp("Двойной клик открывает выбранный MIDI-клип в нижнем редакторе")
        return view
    }
}
