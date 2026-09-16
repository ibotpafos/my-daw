import AppKit

struct PRGhostLoadResult {
    var notes: [PianoRollNote]
    var sourceTracks: Int
    var limited: Bool
}

@MainActor
enum PRGhostLoader {
    static let maximumNotes = 4_096

    static func load(app: DraftApp, activeTrackID: UInt64,
                     activeClip: PianoRollClipModel,
                     limit: Int = maximumNotes) -> PRGhostLoadResult {
        guard limit > 0, activeClip.lengthFrames > 0 else {
            return PRGhostLoadResult(notes: [], sourceTracks: 0, limited: false)
        }
        let activeEndResult = activeClip.startFrames.addingReportingOverflow(activeClip.lengthFrames)
        guard !activeEndResult.overflow else {
            return PRGhostLoadResult(notes: [], sourceTracks: 0, limited: false)
        }
        let activeEnd = activeEndResult.partialValue
        var result: [PianoRollNote] = []
        result.reserveCapacity(min(limit, 512))
        var contributingTracks = Set<UInt64>()
        var limited = false

        for trackID in app.trackIDs.keys.sorted().compactMap({ app.trackIDs[$0] }) where trackID != activeTrackID {
            var clipCount: UInt32 = 0
            guard daw_get_midi_clip_count(app.session, trackID, &clipCount) == 0, clipCount > 0 else { continue }
            for clipIndex in 0..<clipCount {
                var meta = daw_midi_clip()
                meta.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
                meta.version = UInt32(DAW_MIDI_CLIP_VERSION)
                guard daw_get_midi_clip(app.session, trackID, clipIndex, &meta, 0, nil, 0, nil) == 0 else { continue }
                let ghostEndResult = meta.start.addingReportingOverflow(meta.length)
                guard !ghostEndResult.overflow else { continue }
                let ghostEnd = ghostEndResult.partialValue
                guard meta.start < activeEnd, ghostEnd > activeClip.startFrames, meta.note_count > 0 else { continue }

                var offset: UInt32 = 0
                while offset < meta.note_count {
                    if result.count >= limit {
                        limited = true
                        return PRGhostLoadResult(notes: result, sourceTracks: contributingTracks.count, limited: limited)
                    }
                    let remainingCapacity = min(limit - result.count, Int(DAW_MIDI_NOTES_PER_CALL))
                    let pageCount = min(Int(meta.note_count - offset), remainingCapacity)
                    guard pageCount > 0 else { break }
                    var page = [daw_midi_note](repeating: daw_midi_note(), count: pageCount)
                    var written: UInt32 = 0
                    let status = page.withUnsafeMutableBufferPointer { buffer in
                        daw_get_midi_clip(app.session, trackID, clipIndex, &meta,
                                          offset, buffer.baseAddress, UInt32(pageCount), &written)
                    }
                    guard status == 0, written > 0 else { break }
                    for item in page.prefix(Int(written)) {
                        let absoluteStartResult = meta.start.addingReportingOverflow(item.start)
                        guard !absoluteStartResult.overflow else { continue }
                        let absoluteStart = absoluteStartResult.partialValue
                        let absoluteEndResult = absoluteStart.addingReportingOverflow(item.length)
                        guard !absoluteEndResult.overflow else { continue }
                        let absoluteEnd = absoluteEndResult.partialValue
                        // Ghosts are visual references only. A note crossing the active
                        // clip boundary is omitted rather than silently clipped into a
                        // shape that could look editable or suggest a changed duration.
                        guard absoluteStart >= activeClip.startFrames,
                              absoluteEnd <= activeEnd,
                              absoluteEnd > absoluteStart else { continue }
                        result.append(PianoRollNote(startFrames: absoluteStart - activeClip.startFrames,
                                                   lengthFrames: item.length,
                                                   pitch: item.pitch,
                                                   channel: item.channel,
                                                   velocity: item.velocity))
                        contributingTracks.insert(trackID)
                        if result.count >= limit {
                            limited = offset + written < meta.note_count || clipIndex + 1 < clipCount
                            return PRGhostLoadResult(notes: result, sourceTracks: contributingTracks.count, limited: limited)
                        }
                    }
                    offset += written
                }
            }
        }
        return PRGhostLoadResult(notes: result, sourceTracks: contributingTracks.count, limited: limited)
    }
}

@MainActor
final class PRGhostOverlayView: NSView {
    let state: PRProState
    weak var workspace: PRProWorkspaceView?
    private var entities: [PRNoteEntity] = []
    private var index = PRNoteIndex([])
    private var sourceTracks = 0
    private var limited = false

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { false }

    init(state: PRProState) {
        self.state = state
        super.init(frame: .zero)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Ghost notes других MIDI-дорожек")
        setAccessibilityHelp("Справочный слой только для чтения; эти ноты нельзя выделить или изменить из текущего Piano Roll.")
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unavailable") }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func setGhosts(_ result: PRGhostLoadResult) {
        sourceTracks = result.sourceTracks
        limited = result.limited
        entities = result.notes.enumerated().map { index, note in
            PRNoteEntity(id: UInt64(index + 1), note: note)
        }
        index = PRNoteIndex(entities)
        setAccessibilityValue("\(entities.count) нот, дорожек \(sourceTracks)\(limited ? ", показ ограничен" : "")")
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        guard !entities.isEmpty, let map = state.timeMap, let workspace else { return }
        let rows = workspace.rows
        let leftBeat = max(0, Double(dirtyRect.minX - 3) / state.pixelsPerBeat)
        let rightBeat = min(map.durationBeats, Double(dirtyRect.maxX + 3) / state.pixelsPerBeat)
        let start = map.frame(at: leftBeat)
        let end = max(start + 1, map.frame(at: rightBeat))
        let firstRow = max(0, min(rows.pitches.count - 1, Int(floor(dirtyRect.minY / state.rowHeight))))
        let lastRow = max(firstRow, min(rows.pitches.count - 1, Int(floor(dirtyRect.maxY / state.rowHeight))))
        let pitches = Array(rows.pitches[firstRow...lastRow])
        let visible = index.query(start: start, end: end, pitches: pitches)
        for entity in visible {
            guard let rect = PRProDrawing.noteRect(entity.note, state: state, rows: rows), rect.intersects(dirtyRect) else { continue }
            let path = NSBezierPath(roundedRect: rect.insetBy(dx: 1, dy: 2), xRadius: 2, yRadius: 2)
            PRProDrawing.secondary.withAlphaComponent(0.18).setFill()
            path.fill()
            PRProDrawing.secondary.withAlphaComponent(0.42).setStroke()
            path.lineWidth = 1
            path.setLineDash([3, 3], count: 2, phase: 0)
            path.stroke()
        }
        if limited, visibleRect.intersects(dirtyRect) {
            PRProDrawing.label("Ghosts limited to \(PRGhostLoader.maximumNotes)",
                               in: NSRect(x: visibleRect.maxX - 180,
                                          y: visibleRect.minY + 6,
                                          width: 170, height: 15),
                               color: PRProDrawing.secondary, size: 8, alignment: .right)
        }
    }
}

@MainActor
extension PRProWorkspaceView {
    private var installedGhostOverlay: PRGhostOverlayView? {
        canvas.subviews.compactMap { $0 as? PRGhostOverlayView }.first
    }

    @discardableResult
    private func ensureGhostOverlay() -> PRGhostOverlayView {
        if let installedGhostOverlay { return installedGhostOverlay }
        let overlay = PRGhostOverlayView(state: state)
        overlay.workspace = self
        overlay.frame = canvas.bounds
        overlay.autoresizingMask = [.width, .height]
        canvas.addSubview(overlay)
        return overlay
    }

    func setGhostNotes(_ result: PRGhostLoadResult) {
        let overlay = ensureGhostOverlay()
        overlay.frame = canvas.bounds
        overlay.setGhosts(result)
    }
}
