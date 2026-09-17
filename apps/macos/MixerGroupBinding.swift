import Foundation

/// Shared by the application and the native C-ABI integration harness. There is
/// no second gain model: Session owns the preview, validation and one Undo commit.
@MainActor
final class MixerGroupBinding {
    private let session: () -> OpaquePointer?
    private let revision: () -> UInt64
    private let mayBegin: ([UInt64]) -> Bool
    private let started: (UInt64) -> Void
    private let finished: () -> Void
    private let reportError: (Int32) -> Void
    private var active: (session: OpaquePointer, revision: UInt64)?

    init(session: @escaping () -> OpaquePointer?, revision: @escaping () -> UInt64,
         mayBegin: @escaping ([UInt64]) -> Bool, started: @escaping (UInt64) -> Void,
         finished: @escaping () -> Void, reportError: @escaping (Int32) -> Void) {
        self.session = session; self.revision = revision; self.mayBegin = mayBegin
        self.started = started; self.finished = finished; self.reportError = reportError
    }
    func bind(to selection: MixerLinkedLevels) {
        selection.onBegin = { [self] in begin($0) }
        selection.onDelta = { [self] in write($0) }
        selection.onEnd = { [self] in end() }
        selection.onCancel = { [self] in cancel() }
    }
    private func begin(_ ids: [UInt64]) -> Bool {
        guard active == nil, ids.count >= 2, ids.count <= 256, mayBegin(ids), let s = session() else { return false }
        let r = revision()
        let result = ids.withUnsafeBufferPointer { daw_begin_track_gain_group(s,$0.baseAddress,UInt32($0.count),r) }
        guard result == 0 else { reportError(result); return false }
        active = (s,r); started(r); return true
    }
    private func write(_ delta: Double) -> Bool {
        guard let active, session() == active.session else { return false }
        let result = daw_write_mixer_gesture(active.session,delta)
        guard result == 0 else { reportError(result); return false }
        return true
    }
    private func end() {
        guard let captured = active else { return }
        active = nil
        if session() == captured.session {
            let result = daw_end_mixer_gesture(captured.session,captured.revision)
            if result != 0 { reportError(result); daw_cancel_mixer_gesture(captured.session) }
        }
        finished()
    }
    private func cancel() {
        guard let captured = active else { return }
        active = nil
        if session() == captured.session { daw_cancel_mixer_gesture(captured.session) }
        finished()
    }
}
