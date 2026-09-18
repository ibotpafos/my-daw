import AppKit

extension DraftApp {
    /// The menu, context menu and keyboard share the document clipboard. Resolve
    /// selection from the current projection; never copy via a remembered index
    /// from another track or silently reinterpret MIDI as an audio region.
    func copyArrangementClipboard(trackID: UInt64? = nil, index: Int? = nil,
                                  midi: Bool? = nil, cut: Bool) {
        guard let editor = window.arrangementEditing as? ArrangementEditingController else { return }
        editor.bindProjectionIfNeeded()
        guard let track = trackID ?? inspectorTrackID ?? selectedMixerID,
              let lane = editor.lanes.first(where: { $0.track == track }) else { return }
        if let index, let midi {
            let key = ArrangementClipKey(track: track, index: index, kind: midi ? .midi : .audio)
            guard editor.items.contains(where: { $0.key == key }) else { return }
            editor.selection = [key]
        } else if editor.focusTrack != track || editor.selection.isEmpty {
            guard let kind = lane.kind else { return }
            let index = kind == .midi ? (midiClipIndex ?? 0) : (selectedClips[track] ?? 0)
            guard let item = editor.items.first(where: {
                $0.key.track == track && $0.key.kind == kind && $0.key.index == index
            }) else { return }
            editor.selection = [item.key]
        }
        editor.focus(lane); editor.syncSelection(); editor.copySelection(cut: cut)
    }

    func canUseArrangementClipboard(_ action: Selector?) -> Bool {
        guard !isEditingText, !isRecording, !midiTakeArmed,
              automationGesture == nil, pluginParameterGesture == nil,
              let editor = window.arrangementEditing as? ArrangementEditingController,
              let track = inspectorTrackID ?? selectedMixerID,
              let lane = editor.lanes.first(where: { $0.track == track }) else { return false }
        if action == #selector(menuPasteClip) {
            guard let board = editor.clipboard, board.document == midiDocumentID else { return false }
            return lane.kind == nil || lane.kind == board.kind
        }
        return editor.items.contains { $0.key.track == track }
    }
}
