import Foundation

/// All calls stay on the C ABI's owning, non-realtime thread. No new storage,
/// MIDI format, audio graph or Undo stack is introduced by this adapter.
@MainActor
enum PRBridge {
    struct Snapshot {
        let context: PRClipContext
        let clip: PianoRollClipModel
        let notes: [PianoRollNote]
        let editable: Bool
    }
    struct TrackSnapshot {
        let clips: [PianoRollClipModel]
        let selected: Snapshot?
    }
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static func check(_ result: Int32, session: OpaquePointer) throws {
        guard result != 0 else { return }
        var bytes = [CChar](repeating: 0, count: 1024)
        daw_error(session, &bytes, bytes.count)
        let message = String(decoding: bytes.prefix(while: { $0 != 0 }).map { UInt8(bitPattern: $0) }, as: UTF8.self)
        throw Failure(description: message.isEmpty ? "Мост MIDI отклонил операцию" : message)
    }

    static func revision(_ session: OpaquePointer) throws -> UInt64 {
        var snapshot = daw_snapshot()
        snapshot.struct_size = UInt32(MemoryLayout<daw_snapshot>.size)
        try check(daw_get_snapshot(session, &snapshot), session: session)
        return snapshot.revision
    }

    static func canEdit(_ session: OpaquePointer) throws -> Bool {
        var audio = daw_recording()
        audio.struct_size = UInt32(MemoryLayout<daw_recording>.size)
        try check(daw_get_recording(session, &audio), session: session)
        var midi = daw_midi_record_status_t()
        midi.struct_size = UInt32(MemoryLayout<daw_midi_record_status_t>.size)
        midi.version = UInt32(DAW_MIDI_RECORD_STATUS_VERSION)
        try check(daw_midi_record_status(session, &midi), session: session)
        return audio.recording == 0 && midi.armed == 0
    }

    private static func metadata(_ session: OpaquePointer, track: UInt64, index: Int) throws -> daw_midi_clip {
        guard (0..<64).contains(index) else { throw PREditError.invalidClip }
        var meta = daw_midi_clip()
        meta.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
        meta.version = UInt32(DAW_MIDI_CLIP_VERSION)
        try check(daw_get_midi_clip(session, track, UInt32(index), &meta, 0, nil, 0, nil), session: session)
        guard meta.version == UInt32(DAW_MIDI_CLIP_VERSION), meta.length > 0,
              meta.start <= PRLimits.timelineFrames, meta.length <= PRLimits.timelineFrames - meta.start,
              meta.note_count <= UInt32(PRLimits.noteCount) else { throw PREditError.invalidClip }
        return meta
    }

    private static func clipModel(_ meta: daw_midi_clip, index: Int) -> PianoRollClipModel {
        PianoRollClipModel(index: index, startFrames: meta.start, lengthFrames: meta.length, noteCount: meta.note_count)
    }

    static func readClip(_ session: OpaquePointer, document: UUID, track: UInt64, index: Int) throws -> Snapshot {
        let before = try revision(session)
        let original = try metadata(session, track: track, index: index)
        var notes: [PianoRollNote] = []
        notes.reserveCapacity(Int(original.note_count))
        var offset: UInt32 = 0
        while offset < original.note_count {
            let capacity = min(original.note_count - offset, UInt32(DAW_MIDI_NOTES_PER_CALL))
            var page = [daw_midi_note](repeating: daw_midi_note(), count: Int(capacity))
            var meta = daw_midi_clip()
            meta.struct_size = UInt32(MemoryLayout<daw_midi_clip>.size)
            var written: UInt32 = 0
            let result = page.withUnsafeMutableBufferPointer {
                daw_get_midi_clip(session, track, UInt32(index), &meta, offset, $0.baseAddress, capacity, &written)
            }
            try check(result, session: session)
            // A short/error page is a failed snapshot, never an editable prefix.
            guard written == capacity, meta.start == original.start, meta.length == original.length,
                  meta.note_count == original.note_count, meta.version == original.version else { throw PREditError.staleEdit }
            for item in page {
                guard item.struct_size == UInt32(MemoryLayout<daw_midi_note>.size),
                      item.version == UInt32(DAW_MIDI_NOTE_VERSION) else { throw PREditError.invalidNote }
                notes.append(PianoRollNote(startFrames: item.start, lengthFrames: item.length,
                                           pitch: item.pitch, channel: item.channel, velocity: item.velocity))
            }
            offset += written
        }
        try PREdits.validate(notes, clipLength: original.length)
        let editable = try canEdit(session)
        guard try revision(session) == before else { throw PREditError.staleEdit }
        return Snapshot(context: PRClipContext(documentID: document, trackID: track, clipIndex: index, revision: before),
                        clip: clipModel(original, index: index), notes: notes, editable: editable)
    }

    static func readTrack(_ session: OpaquePointer, document: UUID, track: UInt64, selected: Int?) throws -> TrackSnapshot {
        let before = try revision(session)
        var count: UInt32 = 0
        try check(daw_get_midi_clip_count(session, track, &count), session: session)
        guard count <= 64 else { throw PREditError.invalidClip }
        var clips: [PianoRollClipModel] = []
        for index in 0..<Int(count) {
            clips.append(clipModel(try metadata(session, track: track, index: index), index: index))
        }
        let index = selected.flatMap { clips.indices.contains($0) ? $0 : nil } ?? clips.first?.index
        let snapshot: Snapshot?
        if let index { snapshot = try readClip(session, document: document, track: track, index: index) }
        else { snapshot = nil }
        guard try revision(session) == before else { throw PREditError.staleEdit }
        return TrackSnapshot(clips: clips, selected: snapshot)
    }

    static func commit(_ request: PRCommitRequest, session: OpaquePointer, document: UUID) throws -> Snapshot {
        guard request.context.documentID == document,
              try revision(session) == request.context.revision else { throw PREditError.staleEdit }
        let current = try readClip(session, document: document, track: request.context.trackID, index: request.context.clipIndex)
        guard current.editable else { throw PREditError.unavailable }
        guard current.context == request.context, current.clip.startFrames == request.clipStart,
              current.clip.lengthFrames == request.clipLength, current.notes == request.original else { throw PREditError.staleEdit }
        try PREdits.validate(request.notes, clipLength: request.clipLength)
        guard request.notes != request.original else { return current }
        let marshaled = request.notes.map { note -> daw_midi_note in
            var item = daw_midi_note()
            item.struct_size = UInt32(MemoryLayout<daw_midi_note>.size)
            item.version = UInt32(DAW_MIDI_NOTE_VERSION)
            item.start = note.startFrames; item.length = note.lengthFrames
            item.pitch = note.pitch; item.channel = note.channel; item.velocity = note.velocity
            return item
        }
        let result = marshaled.withUnsafeBufferPointer {
            daw_set_midi_notes(session, request.context.trackID, UInt32(request.context.clipIndex),
                              $0.baseAddress, UInt32($0.count), request.context.revision)
        }
        try check(result, session: session)
        return try readClip(session, document: document, track: request.context.trackID, index: request.context.clipIndex)
    }
}
