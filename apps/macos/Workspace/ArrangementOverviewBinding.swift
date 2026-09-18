import AppKit

extension DraftApp {
    func wireArrangementOverview() {
        guard let overview = workspace?.overview, let scroll = timelineScroll else { return }
        overview.bind(to: scroll)
        overview.onFitAll = { [weak self] in self?.setTimelineZoom(1) }
        let previousKeyHandler = window.onFocusedKeyDown
        window.onFocusedKeyDown = { [weak self] event in
            if self?.workspace?.overview.handleFocusedKey(event) == true { return true }
            return previousKeyHandler?(event) ?? false
        }
        updateArrangementOverview()
    }

    func updateArrangementOverview() {
        window?.arrangementEditing?.bindProjectionIfNeeded()
        guard let overview = workspace?.overview else { return }
        let key = ArrangementOverviewContentKey(documentID: midiDocumentID,
            revision: revision, frames: timelineRuler.projectFrames)
        // This path is also reached by transport polling. No parameter queries,
        // waveform reads or clip/note copies are made without a changed snapshot.
        if overview.contentKey != key {
            let midi = Dictionary(midiArrangementViews.map { ($0.trackID, $0) }, uniquingKeysWith: { first, _ in first })
            let tracks = mixerWorkspace.strips.filter { $0.kind == .track }.map { strip -> [ArrangementOverviewClip] in
                var clips: [ArrangementOverviewClip] = []
                if let wave = laneViews[strip.id] {
                    clips += wave.clips.map {
                        let color = $0.color == 0 ? wave.trackAccent : dawColorFromHex($0.color)
                        return ArrangementOverviewClip(start: $0.start, length: $0.length,
                            color: $0.muted ? color.withAlphaComponent(0.3) : color)
                    }
                }
                if let view = midi[strip.id] {
                    clips += view.clips.map { ArrangementOverviewClip(start: $0.start, length: $0.length, color: $0.color) }
                }
                return clips
            }
            overview.updateContent(key: key, tracks: tracks)
        }
        overview.playhead = playheadFrame
        overview.cycle = timelineRuler.cycleRange.selection
        overview.synchronizeViewport()
    }
}
