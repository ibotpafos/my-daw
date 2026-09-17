import Foundation

@main struct BridgeIntegrationTests {
    @MainActor static func main() {
        do { try run() }
        catch { FileHandle.standardError.write(Data("FAIL: bridge integration: \(error)\n".utf8)); exit(1) }
    }
    @MainActor static func run() throws {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ label: String) {
            guard condition() else { FileHandle.standardError.write(Data("FAIL: \(label)\n".utf8)); exit(1) }
            checks += 1; FileHandle.standardOutput.write(Data("PASS \(label)\n".utf8))
        }
        func refuses(_ label: String, _ body: () throws -> Void) {
            do { try body(); check(false, label) } catch { check(true, label) }
        }
        guard let session = daw_create() else { fatalError("daw_create") }
        defer { daw_destroy(session) }
        let document = UUID()
        try PRBridge.check(daw_add_track(session, "Piano roll integration", try PRBridge.revision(session)), session: session)
        var track = daw_track(); track.struct_size = UInt32(MemoryLayout<daw_track>.size)
        try PRBridge.check(daw_get_track(session, 0, &track), session: session)
        let source: [PianoRollNote] = (0..<8195).map { (index: Int) -> PianoRollNote in
            let start = UInt64(index) * 24
            let pitch = UInt8(48 + index % 24)
            let channel = UInt8(index % 16)
            let velocity = UInt8(1 + index % 127)
            return PianoRollNote(startFrames: start, lengthFrames: 480, pitch: pitch, channel: channel, velocity: velocity)
        }
        let buffer = source.map { note -> daw_midi_note in
            var item = daw_midi_note(); item.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
            item.version = UInt32(DAW_MIDI_NOTE_VERSION); item.start = note.startFrames; item.length = note.lengthFrames
            item.pitch = note.pitch; item.channel = note.channel; item.velocity = note.velocity; return item
        }
        var clip = daw_midi_clip(); clip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        clip.version = UInt32(DAW_MIDI_CLIP_VERSION); clip.start = 96_000; clip.length = 384_000
        clip.note_count = UInt32(DAW_MIDI_NOTES_PER_CALL)
        let initialRevision = try PRBridge.revision(session)
        try PRBridge.check(buffer.withUnsafeBufferPointer {
            daw_add_midi_clip(session, track.id, &clip, $0.baseAddress, UInt32(DAW_MIDI_NOTES_PER_CALL), initialRevision)
        }, session: session)
        let appendedRevision = try PRBridge.revision(session)
        try PRBridge.check(buffer.withUnsafeBufferPointer {
            daw_append_midi_notes(session, track.id, 0, $0.baseAddress!.advanced(by: Int(DAW_MIDI_NOTES_PER_CALL)),
                UInt32($0.count) - UInt32(DAW_MIDI_NOTES_PER_CALL), appendedRevision)
        }, session: session)
        let snapshot = try PRBridge.readClip(session, document: document, track: track.id, index: 0)
        check(snapshot.notes == source && snapshot.notes.count > Int(DAW_MIDI_NOTES_PER_CALL), "paged C ABI snapshot contains every note exactly")
        check(snapshot.editable, "idle project permits editing")
        let map = try PRTimeMap(clipStart: clip.start, clipLength: clip.length,
            toBeat: { Double($0) / 24_000 }, toFrame: { UInt64(($0 * 24_000).rounded()) })
        let editor = PRProState()
        editor.receive(notes: snapshot.notes, map: map, editable: snapshot.editable, context: snapshot.context)
        editor.selection = Set(editor.entities.prefix(3).map(\.id))
        var commits = 0
        editor.onCommitRequest = { request in
            do {
                let result = try PRBridge.commit(request, session: session, document: document)
                commits += 1
                editor.receive(notes: result.notes, map: map, editable: result.editable, context: result.context)
            } catch { fatalError("unexpected bridge commit: \(error)") }
        }
        let beforeIDs = editor.selection
        editor.nudge(beats: 0.25, semitones: 2)
        let edited = try PRBridge.readClip(session, document: document, track: track.id, index: 0)
        check(commits == 1 && edited.context.revision == snapshot.context.revision + 1, "editor gesture uses one real project revision")
        check(editor.selection == beforeIDs && editor.status.contains("применено"), "real bridge echo keeps editor selection and success status")
        check(edited.notes.dropFirst(3).elementsEqual(source.dropFirst(3)), "edit leaves unselected ABI notes bit exact")
        check(edited.notes[0].startFrames == 6_000 && edited.notes[0].pitch == 50, "selected move reaches persisted domain frames and pitch")

        try PRBridge.check(daw_undo(session, edited.context.revision), session: session)
        let undone = try PRBridge.readClip(session, document: document, track: track.id, index: 0)
        check(undone.notes == source, "project Undo restores whole pre-gesture MIDI array")
        try PRBridge.check(daw_redo(session, undone.context.revision), session: session)
        let redone = try PRBridge.readClip(session, document: document, track: track.id, index: 0)
        check(redone.notes == edited.notes, "project Redo restores whole accepted MIDI array")
        let temp = FileManager.default.temporaryDirectory.appendingPathComponent("my-daw-midi-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: temp, withIntermediateDirectories: false)
        defer { try? FileManager.default.removeItem(at: temp) }
        let path = temp.appendingPathComponent("project.mydawdraft").path
        try PRBridge.check(daw_save_draft(session, path), session: session)
        try PRBridge.check(daw_set_midi_notes(session, track.id, 0, nil, 0, redone.context.revision), session: session)
        try PRBridge.check(daw_open_draft(session, path), session: session)
        let reopenedDocument = UUID()
        let reopened = try PRBridge.readClip(session, document: reopenedDocument, track: track.id, index: 0)
        check(reopened.notes == redone.notes, "save/open persists edited timing pitch channel and velocity")

        var request = PRCommitRequest(context: reopened.context, clipStart: reopened.clip.startFrames,
            clipLength: reopened.clip.lengthFrames, original: reopened.notes, notes: [])
        let beforeRejects = try PRBridge.revision(session)
        var stale = request; stale.context.revision -= 1
        refuses("stale revision cannot overwrite clip") { _ = try PRBridge.commit(stale, session: session, document: reopenedDocument) }
        stale = request; stale.context.documentID = document
        refuses("same saved revision in reopened document cannot accept old request") { _ = try PRBridge.commit(stale, session: session, document: reopenedDocument) }
        stale = request; stale.clipStart += 1
        refuses("different clip bounds cannot accept stale request") { _ = try PRBridge.commit(stale, session: session, document: reopenedDocument) }
        stale = request; stale.original.removeLast()
        refuses("partial source snapshot cannot replace whole clip") { _ = try PRBridge.commit(stale, session: session, document: reopenedDocument) }
        stale = request; stale.notes = [PianoRollNote(startFrames: UInt64.max, lengthFrames: 1)]
        refuses("invalid note array is rejected before mutation") { _ = try PRBridge.commit(stale, session: session, document: reopenedDocument) }
        let afterRejects = try PRBridge.readClip(session, document: reopenedDocument, track: track.id, index: 0)
        check(afterRejects.context.revision == beforeRejects && afterRejects.notes == reopened.notes, "all rejected operations leave revision and data unchanged")
        check(daw_set_midi_notes(session, track.id, 0, nil,
              UInt32(DAW_MIDI_NOTES_PER_REPLACEMENT) + 1, beforeRejects) != 0,
              "oversized replacement rejected before dereferencing note buffer")
        request.notes = request.original
        let noOp = try PRBridge.commit(request, session: session, document: reopenedDocument)
        check(noOp.context.revision == beforeRejects, "identical replacement costs no revision")
        request.notes = []
        let cleared = try PRBridge.commit(request, session: session, document: reopenedDocument)
        check(cleared.notes.isEmpty && cleared.context.revision == beforeRejects + 1, "intentional empty replacement is one undoable command")
        try PRBridge.check(daw_undo(session, cleared.context.revision), session: session)
        let restored = try PRBridge.readClip(session, document: reopenedDocument, track: track.id, index: 0)
        check(restored.notes == reopened.notes, "Undo of clear restores every paginated note")
        var otherClip = daw_midi_clip(); otherClip.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        otherClip.version = UInt32(DAW_MIDI_CLIP_VERSION); otherClip.start = 768_000; otherClip.length = 48_000
        otherClip.note_count = 1
        var otherNote = buffer[0]
        try PRBridge.check(daw_add_midi_clip(session, track.id, &otherClip, &otherNote, 1, restored.context.revision), session: session)
        let withOther = try PRBridge.readClip(session, document: reopenedDocument, track: track.id, index: 0)
        var full = PRCommitRequest(context: withOther.context, clipStart: withOther.clip.startFrames,
            clipLength: withOther.clip.lengthFrames, original: withOther.notes,
            notes: Array(repeating: withOther.notes[0], count: Int(DAW_MIDI_NOTES_PER_REPLACEMENT)))
        refuses("full replacement still respects other clips in project budget") {
            _ = try PRBridge.commit(full, session: session, document: reopenedDocument)
        }
        let rejectedFull = try PRBridge.readClip(session, document: reopenedDocument, track: track.id, index: 0)
        check(rejectedFull.notes == withOther.notes && rejectedFull.context.revision == withOther.context.revision,
              "project budget rejection is atomic")
        try PRBridge.check(daw_remove_midi_clip(session, track.id, 1, rejectedFull.context.revision), session: session)
        full.context.revision = try PRBridge.revision(session)
        let acceptedFull = try PRBridge.commit(full, session: session, document: reopenedDocument)
        check(acceptedFull.notes.count == Int(DAW_MIDI_NOTES_PER_REPLACEMENT) &&
              acceptedFull.context.revision == full.context.revision + 1, "65536-note replacement is one real project command")
        try PRBridge.check(daw_undo(session, acceptedFull.context.revision), session: session)
        let beforeFull = try PRBridge.readClip(session, document: reopenedDocument, track: track.id, index: 0)
        check(beforeFull.notes == restored.notes, "project Undo restores exact source after maximum-sized replacement")
        print("RESULT \(checks) real C ABI piano-roll integration checks passed")
    }
}
