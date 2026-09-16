import AppKit

// Narrow host stubs for the standalone AppKit smoke executable. The production
// app supplies richer versions from main.swift; the piano-roll view only reads
// these fields while drawing its ruler/playhead.
struct ProjectBarStart {
    var frame: UInt64
    var beats: Double
    var number: Int
}

struct ProjectSignaturePoint {
    var frame: UInt64
    var numerator: UInt8
    var denominator: UInt8
}

struct BeatFrameMap {
    var signatures: [ProjectSignaturePoint] = [ProjectSignaturePoint(frame: 0, numerator: 4, denominator: 4)]
    func signature(atFrame frame: UInt64) -> ProjectSignaturePoint {
        var selected = signatures[0]
        for candidate in signatures where candidate.frame <= frame { selected = candidate }
        return selected
    }
}

@MainActor
final class DraftApp: NSObject, NSApplicationDelegate {
    var tempoBars: [ProjectBarStart] = []
    var tempoMap = BeatFrameMap()
    var playheadFrame: UInt64 = 0
}

@main
struct AppKitSmokeTests {
    @MainActor
    static func main() throws {
        var checks = 0
        func check(_ condition: @autoclosure () -> Bool, _ name: String) {
            guard condition() else { fatalError("FAIL: \(name)") }
            checks += 1
            print("PASS \(name)")
        }

        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
        let appDelegate = DraftApp()
        appDelegate.tempoBars = (0..<9).map { index in
            ProjectBarStart(frame: UInt64(index * 96_000), beats: Double(index * 4), number: index + 1)
        }
        NSApp.delegate = appDelegate

        let map = try PRTimeMap(clipStart: 0, clipLength: 768_000,
            toBeat: { Double($0) / 24_000 },
            toFrame: { UInt64(($0 * 24_000).rounded()) })
        let source = [
            PianoRollNote(startFrames: 24_000, lengthFrames: 24_000, pitch: 60, channel: 0, velocity: 70),
            PianoRollNote(startFrames: 48_000, lengthFrames: 48_000, pitch: 64, channel: 0, velocity: 90),
            PianoRollNote(startFrames: 96_000, lengthFrames: 24_000, pitch: 67, channel: 0, velocity: 110)]

        let state = PRProState()
        var authoritative = source
        var commits = 0
        state.onCommit = { notes in
            commits += 1
            authoritative = notes
            state.receive(notes: notes, map: map, editable: true)
        }
        state.receive(notes: source, map: map, editable: true)

        let workspace = PRProWorkspaceView(state: state)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 760),
                              styleMask: [.titled, .closable, .resizable],
                              backing: .buffered, defer: false)
        window.contentView = workspace
        window.layoutIfNeeded()
        workspace.layoutSubtreeIfNeeded()
        workspace.refreshFromState()
        workspace.layoutSubtreeIfNeeded()

        check(workspace.canvas.frame.width > 500, "canvas document receives usable horizontal extent")
        check(workspace.canvas.frame.height >= 128 * state.rowHeight, "canvas contains all 128 chromatic rows")
        check(workspace.rows.pitches.count == 128, "chromatic workspace starts with 128 rows")
        check(state.pixelsPerBeat.isFinite && state.pixelsPerBeat >= 4, "horizontal zoom remains finite")
        check(state.rowHeight >= 9, "vertical zoom remains usable")

        state.selectAll()
        workspace.fit(selectionOnly: true)
        check(state.pixelsPerBeat > 0 && state.rowHeight > 0, "fit selection leaves valid zoom")
        state.fold = true
        state.changed()
        workspace.refreshGeometry()
        check(workspace.rows.pitches == [67, 64, 60], "fold shows only used pitches in descending order")
        state.fold = false
        workspace.refreshGeometry()

        let commandA = NSEvent.keyEvent(with: .keyDown, location: .zero,
            modifierFlags: [.command], timestamp: 0, windowNumber: 0, context: nil,
            characters: "a", charactersIgnoringModifiers: "a", isARepeat: false, keyCode: 0)!
        state.clearSelection()
        check(workspace.canvas.handleKey(commandA), "Cmd-A is handled by piano-roll responder")
        check(state.selection.count == source.count, "Cmd-A selects notes, not arrangement clips")

        let up = NSEvent.keyEvent(with: .keyDown, location: .zero,
            modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil,
            characters: "", charactersIgnoringModifiers: "", isARepeat: false, keyCode: 126)!
        let pitchesBefore = state.entities.map(\.note.pitch)
        check(workspace.canvas.handleKey(up), "up-arrow is handled locally")
        check(commits == 1, "keyboard transpose is one host commit")
        check(zip(state.entities.map(\.note.pitch), pitchesBefore).allSatisfy { Int($0.0) == Int($0.1) + 1 },
              "up-arrow transposes selection by one semitone")

        state.selection = Set(state.entities.map(\.id))
        state.previewRatchet(count: 4, gate: 0.75)
        workspace.refreshFromState()
        check(state.hasTransformPreview, "ratchet preview is visible without host commit")
        check(commits == 1, "transform preview does not mutate project")
        check(state.entities.count == authoritative.count * 4, "ratchet preview expands visible note candidates")
        state.cancelTransformPreview()
        check(state.entities.map(\.note) == authoritative, "cancel preview restores authoritative notes")

        // Exercise one actual pointer draw through AppKit event coordinates.
        state.tool = .draw
        state.clearSelection()
        workspace.refreshGeometry()
        workspace.layoutSubtreeIfNeeded()
        let rows = workspace.rows
        let targetPitch = UInt8(72)
        guard let targetRow = rows.row(for: targetPitch) else { fatalError("C5 row missing") }
        let local = NSPoint(x: CGFloat(state.pixelsPerBeat * 6.0),
                            y: CGFloat(Double(targetRow) * state.rowHeight + state.rowHeight * 0.5))
        let windowPoint = workspace.canvas.convert(local, to: nil)
        let down = NSEvent.mouseEvent(with: .leftMouseDown, location: windowPoint,
            modifierFlags: [], timestamp: 0, windowNumber: window.windowNumber,
            context: nil, eventNumber: 1, clickCount: 1, pressure: 1)!
        let upEvent = NSEvent.mouseEvent(with: .leftMouseUp, location: windowPoint,
            modifierFlags: [], timestamp: 0.01, windowNumber: window.windowNumber,
            context: nil, eventNumber: 2, clickCount: 1, pressure: 0)!
        workspace.canvas.mouseDown(with: down)
        workspace.canvas.mouseUp(with: upEvent)
        check(commits == 2, "draw gesture creates exactly one host commit")
        check(authoritative.contains(where: { $0.pitch == targetPitch }), "draw gesture creates requested pitch")

        let controller = PRProWindowController(state: state)
        check(controller.window?.minSize.width == 980, "dedicated editor window has desktop minimum width")
        check(controller.window?.contentView === controller.workspace, "dedicated window owns pro workspace")
        controller.close()
        window.close()

        print("RESULT \(checks) AppKit piano-roll smoke checks passed")
    }
}
