import Foundation

/// Transient UI selection. Audio ownership and comp/Undo stay in Session::compRange.
struct RecordCompRequest: Equatable {
    let documentID: UUID
    let trackID: UInt64
    let takeIndex: UInt32
    let revision: UInt64
    let range: TimelineFrameRange
}

struct RecordCompSwipe {
    // Same ten-minute range accepted by the existing public comp command.
    static let maximumFrame: UInt64 = 48000 * 600
    let documentID: UUID
    let trackID: UInt64
    let takeIndex: UInt32
    let revision: UInt64
    let takeStart: UInt64
    let takeEnd: UInt64
    let anchor: UInt64

    init?(documentID: UUID, trackID: UInt64, takeIndex: UInt32, revision: UInt64,
          takeStart: UInt64, takeFrames: UInt64, anchor: UInt64) {
        guard takeFrames > 0, takeStart < Self.maximumFrame,
              takeFrames <= Self.maximumFrame - takeStart,
              anchor >= takeStart, anchor <= takeStart + takeFrames else { return nil }
        self.documentID = documentID
        self.trackID = trackID; self.takeIndex = takeIndex; self.revision = revision
        self.takeStart = takeStart; takeEnd = takeStart + takeFrames; self.anchor = anchor
    }
    func request(at frame: UInt64) -> RecordCompRequest? {
        let end = min(takeEnd, max(takeStart, frame))
        guard let range = TimelineFrameRange(start: min(anchor, end), end: max(anchor, end),
                                             limit: Self.maximumFrame) else { return nil }
        return RecordCompRequest(documentID: documentID, trackID: trackID, takeIndex: takeIndex, revision: revision, range: range)
    }
    static func frame(seconds text: String) -> UInt64? {
        let raw = text.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: ",", with: ".")
        guard let seconds = Double(raw), seconds.isFinite, seconds >= 0,
              seconds <= Double(maximumFrame) / 48000 else { return nil }
        return UInt64((seconds * 48000).rounded())
    }
}
