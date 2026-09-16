import Foundation

@main struct PreviewCoreTests {
    @MainActor static func main() throws {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            precondition(condition(), name)
            checks += 1
            print("PASS \(name)")
        }

        let map = try PRTimeMap(clipStart: 0, clipLength: 384_000,
            toBeat: { Double($0) / 24_000 },
            toFrame: { UInt64(($0 * 24_000).rounded()) })
        let original = [
            PianoRollNote(startFrames: 24_000, lengthFrames: 24_000, pitch: 60, channel: 0, velocity: 70),
            PianoRollNote(startFrames: 48_000, lengthFrames: 24_000, pitch: 64, channel: 0, velocity: 90),
            PianoRollNote(startFrames: 72_000, lengthFrames: 24_000, pitch: 67, channel: 0, velocity: 110)]
        let state = PRProState()
        var persisted = original
        var commits = 0
        state.onCommit = { notes in
            commits += 1
            persisted = notes
            state.receive(notes: notes, map: map, editable: true)
        }
        state.receive(notes: original, map: map, editable: true)
        state.selection = [state.entities[0].id]

        state.previewRatchet(count: 4, gate: 1)
        check(state.hasTransformPreview, "ratchet creates transform transaction")
        check(commits == 0, "preview does not commit")
        check(state.entities.count == 6, "ratchet preview is visible")
        check(persisted.count == 3, "host data unchanged during preview")

        state.previewRatchet(count: 2, gate: 1)
        check(state.entities.count == 4, "parameter change recalculates from original snapshot")
        check(commits == 0, "parameter change remains non-destructive")

        let pointerStarted = state.beginGesture()
        check(!pointerStarted, "pointer gesture cannot replace transform preview")
        check(state.hasTransformPreview, "blocked pointer gesture cannot lose transform source")
        check(state.entities.count == 4, "blocked pointer gesture leaves preview intact")

        let beforeDirect = state.entities
        state.quantize()
        check(commits == 0, "direct edit blocked while transform preview is active")
        check(state.hasTransformPreview && state.entities == beforeDirect, "blocked direct edit preserves preview")

        state.applyTransformPreview()
        check(commits == 1, "apply performs exactly one host commit")
        check(!state.hasTransformPreview, "apply closes transform transaction")
        check(persisted.count == 4, "applied ratchet reaches host")

        state.selection = Set(state.entities.prefix(3).map(\.id))
        state.previewStrum(spreadBeats: 0.5, descending: false)
        check(state.hasTransformPreview && commits == 1, "strum preview stays local")
        state.cancelTransformPreview()
        check(commits == 1, "cancel never commits")
        check(state.entities.map(\.note) == persisted, "cancel restores authoritative notes")
        check(!state.hasTransformPreview, "cancel closes transaction")

        var simultaneous = persisted
        simultaneous[0].startFrames = 96_000
        simultaneous[1].startFrames = 96_000
        simultaneous[2].startFrames = 96_000
        state.receive(notes: simultaneous, map: map, editable: true)
        state.selection = Set(state.entities.prefix(3).map(\.id))
        state.previewStrum(spreadBeats: 0.5, descending: false)
        let starts = Array(state.entities.prefix(3)).map(\.note.startFrames)
        check(Set(starts).count > 1, "strum preview visibly spreads simultaneous pitches")
        state.cancelTransformPreview()
        check(state.entities.map(\.note) == simultaneous, "cancel restores simultaneous source exactly")
        state.receive(notes: persisted, map: map, editable: true)

        state.selection = Set(state.entities.map(\.id))
        state.previewRatchet(count: 2, gate: 1)
        let ratchetCount = state.entities.count
        state.previewVelocityRamp(from: 20, to: 127)
        check(state.entities.count == persisted.count && state.entities.count < ratchetCount,
              "switching preview type uses original notes, not previous preview")
        check(state.entities.first?.note.velocity == 20, "velocity ramp preview updates candidate")
        check(commits == 1, "switching preview type remains non-destructive")
        state.cancelTransformPreview()

        state.selection = Set(state.entities.map(\.id))
        state.previewVelocityRamp(from: 0, to: 127)
        check(!state.hasTransformPreview, "invalid preview cancels transaction")
        check(commits == 1 && state.entities.map(\.note) == persisted, "invalid preview cannot leave stale candidate")

        state.selection = Set(state.entities.map(\.id))
        state.previewVelocityRamp(from: 30, to: 100)
        var external = persisted
        external[0].pitch = 48
        state.receive(notes: external, map: map, editable: true)
        check(!state.hasTransformPreview, "external snapshot cancels preview")
        check(state.entities[0].note.pitch == 48, "external authoritative data wins over preview")
        state.applyTransformPreview()
        check(commits == 1, "stale apply after external refresh is no-op")

        state.receive(notes: persisted, map: map, editable: false)
        state.selection = Set(state.entities.map(\.id))
        state.previewRatchet(count: 4, gate: 1)
        check(!state.hasTransformPreview && commits == 1, "disabled editor cannot start transform preview")

        print("RESULT \(checks) transform-preview checks passed")
    }
}
