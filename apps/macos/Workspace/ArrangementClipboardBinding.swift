import AppKit

// The implementation in DraftApp must be visible to AppKit's Objective-C
// menu update path; a same-named Swift method alone is not a validator.
extension DraftApp: NSMenuItemValidation {
    /// The menu, context menu and keyboard share the document clipboard. Resolve
    /// selection from the current projection; never copy via a remembered index
    /// from another track or silently reinterpret MIDI as an audio region.
    func copyArrangementClipboard(trackID: UInt64? = nil, index: Int? = nil,
                                  midi: Bool? = nil, cut: Bool) {
        guard let editor = window.arrangementEditing as? ArrangementEditingController else { return }
        guard !isEditingText, editor.editable, editor.gesture == nil,
              editor.documentID == midiDocumentID,
              editor.projectionRevision == editor.currentRevision() else { return }
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
        guard !isEditingText, let editor = window.arrangementEditing as? ArrangementEditingController,
              editor.editable, editor.gesture == nil,
              editor.documentID == midiDocumentID, editor.projectionRevision == editor.currentRevision(),
              let track = inspectorTrackID ?? selectedMixerID else { return false }
        if action == #selector(menuPasteClip) {
            guard let board = editor.clipboard else { return false }
            let start = board.continuation?.track == track && board.continuation?.frame == playheadFrame ?
                playheadFrame : editor.snapped(playheadFrame, flags: [])
            return editor.clipboardDestinations(board, track: track, start: start) != nil
        }
        return arrangementMenuSelection() != nil
    }
}


extension DraftApp {
    /// All three edit-menu actions use the same selection and atomic command
    /// as keyboard/drag editing. Never silently reinterpret MIDI as audio.
    func arrangementMenuSelection() -> (ArrangementEditingController, ArrangementEditingController.Lane,
                                         [ArrangementEditingController.Item])? {
        guard !isEditingText, !isRecording, !midiTakeArmed,
              automationGesture == nil, pluginParameterGesture == nil,
              let editor = window.arrangementEditing as? ArrangementEditingController,
              editor.gesture == nil, editor.documentID == midiDocumentID,
              editor.projectionRevision == editor.currentRevision(),
              let track = inspectorTrackID ?? selectedMixerID,
              let lane = editor.lanes.first(where: { $0.track == track }) else { return nil }
        if editor.focusTrack == track, !editor.selection.isEmpty {
            let group = editor.items.filter { editor.selection.contains($0.key) }
            guard !group.isEmpty, group.count <= 256 else { return nil }
            return (editor, lane, group)
        }
        guard let kind = lane.kind else { return nil }
        let index = kind == .midi ? (midiClipIndex ?? 0) : (selectedClips[track] ?? 0)
        guard let item = editor.items.first(where: {
            $0.key.track == track && $0.key.kind == kind && $0.key.index == index
        }) else { return nil }
        return (editor, lane, [item])
    }
    func canEditArrangementSelection(_ action: Selector?) -> Bool {
        guard let (_, _, group) = arrangementMenuSelection() else { return false }
        if action == #selector(menuClipSplit) {
            return group.count == 1 && playheadFrame > group[0].bounds.start && playheadFrame < group[0].bounds.end
        }
        return true
    }
    func editArrangementSelection(_ action: Selector) {
        guard let (editor, lane, group) = arrangementMenuSelection() else { return }
        editor.selection = Set(group.map(\.key)); editor.focus(lane); editor.syncSelection()
        if action == #selector(menuClipDuplicate) { editor.duplicateSelection() }
        else if action == #selector(menuClipDelete) { editor.deleteSelection() }
        else if action == #selector(menuClipSplit), group.count == 1 { editor.split(group[0], at: playheadFrame) }
    }
}
